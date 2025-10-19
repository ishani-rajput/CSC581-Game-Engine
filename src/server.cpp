#include "network_server.h"
#include <unordered_map>
#include <iostream>
#include <cstdio>
#include <chrono>

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

class SpikeyServer : public Engine::NetworkServer
{
private:
    std::unordered_map<std::string, Vec2> players;
    Platform movingPlatform;
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point lastUpdate;

    // Level constants
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
    SpikeyServer()
    {
        movingPlatform = {
            (MP_MIN_X + MP_MAX_X) * 0.5f,
            MP_Y,
            MP_W, MP_H,
            200.f, // speed
            1      // direction
        };

        startTime = std::chrono::steady_clock::now();
        lastUpdate = startTime;
        setWorldUpdateRate(60);
    }

protected:
    void handleClientMessage(const std::string &clientId, const std::string &message) override
    {

        if (message.find("DISCONNECT") == 0)
        {
            char disconnectClientId[256];
            if (sscanf(message.c_str(), "DISCONNECT %255s", disconnectClientId) == 1)
            {
                std::string actualClientId(disconnectClientId);
                players.erase(actualClientId);
                std::cout << "Player disconnected and removed: " << actualClientId << std::endl;
            }
            return;
        }
        char id[256];
        float x = 0, y = 0;

        if (sscanf(message.c_str(), "ID %255s X %f Y %f", id, &x, &y) == 3)
        {
            players[std::string(id)] = Vec2{x, y};
        }
    }

    std::string generateWorldState() override
    {
        auto now = std::chrono::steady_clock::now();
        float currentTime = std::chrono::duration<float>(now - startTime).count();
        float deltaTime = std::chrono::duration<float>(now - lastUpdate).count();
        lastUpdate = now;

        // Update moving platform (server timeline)
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

        // Build response
        char buffer[8192];
        int offset = snprintf(buffer, sizeof(buffer), "T %.3f\nN %zu\n",
                              currentTime, players.size());

        for (const auto &[playerId, pos] : players)
        {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               "%s %.3f %.3f\n", playerId.c_str(), pos.x, pos.y);
        }

        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "P %.3f %.3f %.3f %.3f %d\n",
                           movingPlatform.x, movingPlatform.y,
                           movingPlatform.w, movingPlatform.h,
                           movingPlatform.dir);

        return std::string(buffer, offset);
    }

    void onClientConnected(const std::string &clientId) override
    {
        std::cout << "Spikey player joined: " << clientId << std::endl;
        players[clientId] = Vec2{100.f, MP_Y - 256.f};
    }

    void onClientDisconnected(const std::string &clientId) override
    {
        std::cout << "Spikey player left: " << clientId << std::endl;
        players.erase(clientId);
    }
};

int main()
{
    SpikeyServer server;
    server.startServer(5555);

    std::cout << "Spikey server running on port 5555. Press Enter to stop..." << std::endl;
    std::cin.get();

    server.stopServer();
    return 0;
}