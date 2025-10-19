#include "network_server.h"
#include "collision.h"

// ENGINE INTEGRATION
#include "net_strategy.h"
#include "object_model.h"
#include "registry.h"

#include <iostream>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <mutex>
#include <SDL3/SDL.h>

// ENGINE GLOBAL REGISTRY (object model)
static Engine::Registry serverRegistry;

// Pipe structures
struct PipePair {
    float topX, topY, topW, topH;
    float bottomX, bottomY, bottomW, bottomH;
};

// Game constants
static const float PIPE_SPEED = -450.f;
static const float PIPE_W = 140.f;
static const float PIPE_GAP = 280.f;
static const double PIPE_SPAWN_EVERY = 1.4;
static const float SCREEN_WIDTH = 1920.f;
static const float SCREEN_HEIGHT = 1080.f;
static const float CHARACTER_SIZE = 108.f;

static float floatRand(float a, float b) {
    return a + (b - a) * (float)rand() / (float)RAND_MAX;
}

class GameServer : public Engine::NetworkServer {
private:
    std::vector<PipePair> pipes;
    mutable std::mutex gameStateMutex;

    std::string lastMessage;
    mutable std::mutex messageMutex;

    double  spawnTimer     = 0.0;
    clock_t lastPipeUpdate = clock();

public:
    GameServer() {
        srand(12345);
        setWorldUpdateRate(60);
    }

protected:
    void handleClientMessage(const std::string& clientId, const std::string& message) override {
        {
            std::lock_guard<std::mutex> lock(messageMutex);
            lastMessage = message;
        }
        if (message == "CONNECT") return;

        char id[256]; float x=0,y=0; int paused=0; float scale=1.0f;
        if (sscanf(message.c_str(), "ID %255s X %f Y %f PAUSED %d SCALE %f", id, &x, &y, &paused, &scale) == 5) {
            std::lock_guard<std::mutex> lock(gameStateMutex);
            // ENGINE OBJECT MODEL update
            auto& obj = serverRegistry.upsert(clientId);
            obj.set<Engine::Vec2>("pos", {x, y});
            obj.set<bool>("paused", paused == 1);
            obj.set<float>("scale", scale);

            SDL_FRect playerRect{ x, y, CHARACTER_SIZE, CHARACTER_SIZE };
            bool hit = false;
            for (const auto& p : pipes) {
                SDL_FRect top{ p.topX, p.topY, p.topW, p.topH };
                SDL_FRect bot{ p.bottomX, p.bottomY, p.bottomW, p.bottomH };
                if (aabbIntersect(playerRect, top) || aabbIntersect(playerRect, bot)) { hit = true; break; }
            }
            if (hit) { pipes.clear(); spawnTimer = 0.0; }
        }
    }

    std::string generateWorldState() override {
        {
            std::lock_guard<std::mutex> lock(messageMutex);
            if (lastMessage == "CONNECT") {
                return "CONNECTED " + std::to_string(rand() % 1000);
            }
        }

        std::lock_guard<std::mutex> lock(gameStateMutex);
        std::string response =
            "N " + std::to_string(serverRegistry.size()) + " P " + std::to_string(pipes.size()) + "\n";

        for (const auto& [id, objPtr] : serverRegistry) {
            auto& obj = *objPtr;
            Engine::Vec2 pos = obj.get<Engine::Vec2>("pos");
            bool paused = obj.get<bool>("paused", false);
            float scale = obj.get<float>("scale", 1.0f);
            response += id + " " + std::to_string(pos.x) + " " + std::to_string(pos.y) + " " +
                        std::to_string(paused ? 1 : 0) + " " + std::to_string(scale) + "\n";
        }

        for (const auto& p : pipes) {
            response += std::to_string(p.topX) + " " + std::to_string(p.topY) + " " +
                        std::to_string(p.topW) + " " + std::to_string(p.topH) + " " +
                        std::to_string(p.bottomX) + " " + std::to_string(p.bottomY) + " " +
                        std::to_string(p.bottomW) + " " + std::to_string(p.bottomH) + "\n";
        }
        return response;
    }

    void onClientDisconnected(const std::string& clientId) override {
        std::lock_guard<std::mutex> lock(gameStateMutex);
        serverRegistry.erase(clientId);
        std::string msg = "DISCONNECT " + clientId;
        broadcastToAllClients(msg);
    }

public:
    void runPipeLoop() {
        using namespace std::chrono;
        while (true) {
            {
                std::lock_guard<std::mutex> lock(gameStateMutex);

                clock_t now = clock();
                double dtSec = double(now - lastPipeUpdate) / CLOCKS_PER_SEC;
                lastPipeUpdate = now;
                if (dtSec > 0.1) dtSec = 0.1;

                spawnTimer += dtSec;
                while (spawnTimer >= PIPE_SPAWN_EVERY) {
                    spawnTimer -= PIPE_SPAWN_EVERY;
                    float center = floatRand(SCREEN_HEIGHT * 0.30f, SCREEN_HEIGHT * 0.70f);
                    float topH = center - PIPE_GAP * 0.5f;
                    float bottomY = center + PIPE_GAP * 0.5f;
                    pipes.push_back({
                        SCREEN_WIDTH + PIPE_W, 0.f, PIPE_W, topH,
                        SCREEN_WIDTH + PIPE_W, bottomY, PIPE_W, SCREEN_HEIGHT - bottomY - 120.f
                    });
                }
                for (auto& p : pipes) {
                    p.topX += PIPE_SPEED * (float)dtSec;
                    p.bottomX += PIPE_SPEED * (float)dtSec;
                }
                pipes.erase(std::remove_if(pipes.begin(), pipes.end(),
                    [](const PipePair& p){ return (p.topX + p.topW) < -50.f; }),
                    pipes.end());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
};

int main() {
    GameServer server;
    std::cout << "Starting game server on port 5555...\n";
    server.startServer(5555);

    std::thread pipeThread(&GameServer::runPipeLoop, &server);
    std::cout << "Server running.\n";
    std::cin.get();

    server.stopServer();
    if (pipeThread.joinable()) pipeThread.detach();
    return 0;
}
