#include <zmq.h>
#include <unordered_map>
#include <string>
#include <iostream>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <sstream>
#include <random>
#include "event_manager.h"
#include "timeline.h"

struct Platform {
    float x, y, w, h;
    float speed;
    int dir;
    float minBound, maxBound;
    bool isVertical;
};

struct PeerInfo {
    std::string clientId;
    std::string pubEndpoint;
};

class HybridP2PServer {
private:
    void *ctx{nullptr};
    void *socket{nullptr};

    // World state
    Platform horizontalPlatform{};
    Platform verticalPlatform{};
    bool bridge1Visible{true};
    float bridgeToggleTimer{0.f};

    // Peers / identity / timing
    std::unordered_map<std::string, PeerInfo> peers;
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point lastUpdate;

    // Identity / epoch
    std::string serverId;
    uint64_t epoch{1};

    // Engine timeline + events
    Timeline serverTimeline;
    Engine::EventManager eventManager;

    // Level geometry constants
    static constexpr int DESIGN_WIDTH = 1720;
    static constexpr int DESIGN_HEIGHT = 1080;
    static constexpr float leftWidth = 180.f;
    static constexpr float leftHeight = 300.f;
    static constexpr float middleWidth = 200.f;
    static constexpr float middleHeight = 350.f;
    static constexpr float leftX = 0.f;
    static constexpr float leftY = DESIGN_HEIGHT - leftHeight - 70.f;
    static constexpr float middleX = DESIGN_WIDTH / 2.f - middleWidth / 2.f - 200.f;
    static constexpr float middleY = DESIGN_HEIGHT - middleHeight;

public:
    HybridP2PServer()
        : eventManager(&serverTimeline) {

        // --- Server identity
        serverId = makeServerId();
        epoch = 1;

        // ZMQ
        ctx = zmq_ctx_new();
        socket = zmq_socket(ctx, ZMQ_REP);

        // Timeline
        serverTimeline.anchorToRealTime();

        // Server-side event listeners (for logging/metrics)
        eventManager.registerListener(Engine::EventType::Death, [this](const Engine::Event& e) {
            std::string who = std::get<std::string>(e.payload.at("entity"));
            std::cout << "[SERVER EVENT] Death: " << who
                      << " at t=" << e.timestamp << "\n";
        });

        eventManager.registerListener(Engine::EventType::Collision, [this](const Engine::Event& e) {
            std::string objA = std::get<std::string>(e.payload.at("A"));
            std::string objB = std::get<std::string>(e.payload.at("B"));
            std::cout << "[SERVER EVENT] Collision: " << objA << " <-> " << objB << "\n";
        });

        eventManager.registerListener(Engine::EventType::Input, [this](const Engine::Event& e) {
            std::string key = std::get<std::string>(e.payload.at("key"));
            bool pressed = std::get<bool>(e.payload.at("pressed"));
            std::cout << "[SERVER EVENT] Input: " << key << " " << (pressed ? "pressed" : "released") << "\n";
        });

        eventManager.registerListener(Engine::EventType::Spawn, [this](const Engine::Event& e) {
            std::string who = std::get<std::string>(e.payload.at("entity"));
            float x = std::get<float>(e.payload.at("x"));
            float y = std::get<float>(e.payload.at("y"));
            std::cout << "[SERVER EVENT] Spawn: " << who << " at (" << x << ", " << y << ")\n";
        });

        std::cout << "[SERVER] Event system initialized\n";

        // --- Initialize world
        horizontalPlatform = {
            leftX + leftWidth + 20.f,
            DESIGN_HEIGHT - middleHeight - 150.f,
            144.f, 72.f,
            200.f, 1,
            leftWidth, middleX,    // min/max travel bounds in X
            false
        };

        verticalPlatform = {
            middleX + middleWidth + 40.f,
            middleY - 180.f,
            144.f, 68.f,
            150.f, 1,
            DESIGN_HEIGHT - middleHeight - 350.f, middleY - 120.f, // min/max travel bounds in Y
            true
        };

        bridge1Visible = true;
        bridgeToggleTimer = 0.f;

        startTime = std::chrono::steady_clock::now();
        lastUpdate = startTime;
    }

    ~HybridP2PServer() {
        if (socket) zmq_close(socket);
        if (ctx) zmq_ctx_destroy(ctx);
    }

    void run(int port) {
        std::string address = "tcp://*:" + std::to_string(port);
        if (zmq_bind(socket, address.c_str()) != 0) {
            std::cerr << "Failed to bind to port " << port << "\n";
            return;
        }

        std::cout << "==================================\n";
        std::cout << "Hybrid P2P Server with Events\n";
        std::cout << "Port: " << port << "\n";
        std::cout << "ServerId: " << serverId << "  Epoch: " << epoch << "\n";
        std::cout << "==================================\n";

        while (true) {
            char buffer[4096];
            int size = zmq_recv(socket, buffer, sizeof(buffer) - 1, 0);
            if (size <= 0) continue;

            buffer[size] = '\0';
            std::string message(buffer);

            std::string response = handleMessage(message);
            zmq_send(socket, response.c_str(), (int)response.length(), 0);
        }
    }

private:
    static std::string makeServerId() {
        std::random_device rd; std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(1000, 9999);
        return "server_" + std::to_string(dis(gen));
    }

    uint64_t uptimeMs() const {
        auto now = std::chrono::steady_clock::now();
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
    }

    void tickWorld(float deltaTime) {
        // Toggle bridge visibility
        bridgeToggleTimer += deltaTime;
        if (bridge1Visible && bridgeToggleTimer > 3.0f) {
            bridge1Visible = false;
            bridgeToggleTimer = 0.f;
        } else if (!bridge1Visible && bridgeToggleTimer > 2.0f) {
            bridge1Visible = true;
            bridgeToggleTimer = 0.f;
        }

        // Horizontal platform motion
        if (!horizontalPlatform.isVertical) {
            horizontalPlatform.x += horizontalPlatform.speed * horizontalPlatform.dir * deltaTime;
            if (horizontalPlatform.x < horizontalPlatform.minBound) {
                horizontalPlatform.x = horizontalPlatform.minBound;
                horizontalPlatform.dir = 1;
            } else if (horizontalPlatform.x + horizontalPlatform.w > horizontalPlatform.maxBound) {
                horizontalPlatform.x = horizontalPlatform.maxBound - horizontalPlatform.w;
                horizontalPlatform.dir = -1;
            }
        }

        // Vertical platform motion
        if (verticalPlatform.isVertical) {
            verticalPlatform.y += verticalPlatform.speed * verticalPlatform.dir * deltaTime;
            if (verticalPlatform.y < verticalPlatform.minBound) {
                verticalPlatform.y = verticalPlatform.minBound;
                verticalPlatform.dir = 1;
            } else if (verticalPlatform.y > verticalPlatform.maxBound) {
                verticalPlatform.y = verticalPlatform.maxBound;
                verticalPlatform.dir = -1;
            }
        }
    }

    std::string hostLine() {
        std::ostringstream oss;
        oss << "HOST " << serverId << " " << epoch << " " << uptimeMs() << "\n";
        return oss.str();
    }

    std::string peersLines() {
        std::ostringstream oss;
        for (const auto& [id, info] : peers) {
            oss << "PEER " << info.clientId << " " << info.pubEndpoint << "\n";
        }
        return oss.str();
    }

    std::string worldLines() {
        std::ostringstream oss;
        oss << "HP " << horizontalPlatform.x << " " << horizontalPlatform.y << " "
            << horizontalPlatform.w << " " << horizontalPlatform.h << " " << horizontalPlatform.dir << "\n";
        oss << "VP " << verticalPlatform.x << " " << verticalPlatform.y << " "
            << verticalPlatform.w << " " << verticalPlatform.h << " " << verticalPlatform.dir << "\n";
        oss << "B1 " << (bridge1Visible ? 1 : 0) << "\n";
        return oss.str();
    }

    std::string handleMessage(const std::string &message) {
        serverTimeline.tick();
        eventManager.dispatchEvents();

        auto now = std::chrono::steady_clock::now();
        float deltaTime = std::chrono::duration<float>(now - lastUpdate).count();
        lastUpdate = now;
        tickWorld(deltaTime);

        std::istringstream iss(message);
        std::string command;
        iss >> command;

        if (command == "PING") {
            std::ostringstream reply;
            reply << "PONG " << uptimeMs() << "\n" << hostLine();
            return reply.str();
        }

        if (command == "REGISTER_PEER") {
            std::string clientId, pubEndpoint;
            if (iss >> clientId >> pubEndpoint) {
                peers[clientId] = {clientId, pubEndpoint};
                std::cout << "[PEER] Registered: " << clientId << " @ " << pubEndpoint << "\n";

                auto spawnEv = Engine::Events::Spawn(clientId, leftX + 80.f, leftY - 256.f, &serverTimeline, 1);
                eventManager.raiseEvent(spawnEv);
            }
            std::ostringstream reply;
            reply << hostLine() << worldLines() << peersLines();
            return reply.str();
        }

        if (command == "DEREGISTER_PEER") {
            std::string clientId;
            if (iss >> clientId) {
                peers.erase(clientId);
                std::cout << "[PEER] Deregistered: " << clientId << "\n";
            }
            std::ostringstream reply;
            reply << hostLine() << peersLines();
            return reply.str();
        }

        if (command == "EVENT") {
            if (message.size() > 6) {
                std::string eventData = message.substr(6);
                eventManager.raiseEventFromNetwork(eventData);
            }
            std::ostringstream reply;
            reply << hostLine() << worldLines() << peersLines();
            return reply.str();
        }

        if (command == "LIST_PEERS") {
            std::ostringstream reply;
            reply << hostLine() << peersLines();
            return reply.str();
        }

        std::ostringstream reply;
        reply << hostLine() << worldLines() << peersLines();
        return reply.str();
    }
};

int main() {
    HybridP2PServer server;
    server.run(5556);  // Different port from client-server (5555)
    return 0;
}
