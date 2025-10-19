#include <zmq.h>
#include <unordered_map>
#include <string>
#include <iostream>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <sstream>

struct Vec2
{
    float x = 0, y = 0;
};
struct Platform
{
    float x, y, w, h;
    float speed;
    int dir;
};

struct PeerInfo
{
    std::string clientId;
    std::string pubEndpoint;
};

// Section 5: Hybrid P2P Server
// - Controls moving platforms (authoritative)
// - Facilitates peer discovery (tracks peer PUB endpoints)
// - Does NOT relay player positions (peers handle that directly)
class Section5Server
{
private:
    void *ctx;
    void *socket;
    Platform movingPlatform;
    std::unordered_map<std::string, PeerInfo> peers;
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point lastUpdate;

    static constexpr int DESIGN_WIDTH = 1720;
    static constexpr int DESIGN_HEIGHT = 1080;
    static constexpr float STATIC_PLATFORM_W = 400.f;
    static constexpr float STATIC_PLATFORM_H = 500.f;
    static constexpr float MP_W = 384.f;
    static constexpr float MP_H = 128.f;
    static constexpr float MP_Y = DESIGN_HEIGHT - STATIC_PLATFORM_H - 200.f;
    static constexpr float MP_MIN_X = STATIC_PLATFORM_W;
    static constexpr float MP_MAX_X = DESIGN_WIDTH - STATIC_PLATFORM_W - MP_W;

public:
    Section5Server()
    {
        ctx = zmq_ctx_new();
        socket = zmq_socket(ctx, ZMQ_REP);

        movingPlatform = {
            (MP_MIN_X + MP_MAX_X) * 0.5f,
            MP_Y,
            MP_W, MP_H,
            200.f,
            1};

        startTime = std::chrono::steady_clock::now();
        lastUpdate = startTime;
    }

    ~Section5Server()
    {
        if (socket)
            zmq_close(socket);
        if (ctx)
            zmq_ctx_destroy(ctx);
    }

    void run(int port)
    {
        std::string address = "tcp://*:" + std::to_string(port);
        if (zmq_bind(socket, address.c_str()) != 0)
        {
            std::cerr << "Failed to bind to port " << port << std::endl;
            return;
        }

        std::cout << "Section 5 Hybrid P2P Server running on port " << port << std::endl;
        std::cout << "- Server controls: Moving platforms" << std::endl;
        std::cout << "- Peers handle: Player positions (direct P2P)" << std::endl;

        while (true)
        {
            char buffer[1024];
            int size = zmq_recv(socket, buffer, sizeof(buffer) - 1, 0);
            if (size <= 0)
                continue;

            buffer[size] = '\0';
            std::string message(buffer);

            std::string response = handleMessage(message);
            zmq_send(socket, response.c_str(), response.length(), 0);
        }
    }

private:
    std::string handleMessage(const std::string &message)
    {
        std::istringstream iss(message);
        std::string command;
        iss >> command;

        if (command == "REGISTER_PEER")
        {
            // Client registering its PUB endpoint
            std::string clientId, pubEndpoint;
            if (iss >> clientId >> pubEndpoint)
            {
                peers[clientId] = {clientId, pubEndpoint};
                std::cout << "Registered peer: " << clientId << " at " << pubEndpoint << std::endl;
            }
        }

        // Always return world state + peer list
        return generateWorldState();
    }

    std::string generateWorldState()
    {
        // Update moving platform
        auto now = std::chrono::steady_clock::now();
        float deltaTime = std::chrono::duration<float>(now - lastUpdate).count();
        lastUpdate = now;

        movingPlatform.x += movingPlatform.speed * movingPlatform.dir * deltaTime;
        if (movingPlatform.x < MP_MIN_X)
        {
            movingPlatform.x = MP_MIN_X;
            movingPlatform.dir = 1;
        }
        else if (movingPlatform.x + movingPlatform.w > MP_MAX_X + movingPlatform.w)
        {
            movingPlatform.x = MP_MAX_X;
            movingPlatform.dir = -1;
        }

        // Build response: platform + peer list
        std::ostringstream oss;
        oss << "PLATFORM " << movingPlatform.x << " " << movingPlatform.y << " "
            << movingPlatform.w << " " << movingPlatform.h << " " << movingPlatform.dir << "\n";

        // Send peer list for discovery
        for (const auto &[id, info] : peers)
        {
            oss << "PEER " << info.clientId << " " << info.pubEndpoint << "\n";
        }

        return oss.str();
    }
};

int main()
{
    Section5Server server;
    server.run(5555);
    return 0;
}