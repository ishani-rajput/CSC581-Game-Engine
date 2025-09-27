#include "network_server.h"
#include "collision.h"
#include <iostream>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <SDL3/SDL.h>

// Simple 2D vector
struct Vec2 { float x=0, y=0; };

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
static const float CHARACTER_SIZE = 64.f;

// RNG helper
static float floatRand(float a, float b) { 
    return a + (b-a) * (float)rand()/(float)RAND_MAX; 
}

// Player data structure
struct PlayerData {
    Vec2 pos;
    bool paused = false;
    float scale = 1.0f;
};

// Game server implementation using the engine framework (Section 4: Asynchronicity)
class GameServer : public Engine::NetworkServer {
private:
    // Game state (thread-safe)
    std::unordered_map<std::string, PlayerData> players;
    std::vector<PipePair> pipes;
    mutable std::mutex gameStateMutex;
    
    // Pipe management (independent of client pause states)
    double spawnTimer = 0.0;
    clock_t lastPipeUpdate = clock();

public:
    GameServer() {
        // Initialize RNG with fixed seed for deterministic pipes
        srand(12345);
        setWorldUpdateRate(60); // 60 FPS
    }

protected:
    void handleClientMessage(const std::string& clientId, const std::string& message) override {
        // Parse client message - track position AND pause state
        char id[256]; float x=0,y=0; int paused=0; float scale=1.0f;
        if (sscanf(message.c_str(), "ID %255s X %f Y %f PAUSED %d SCALE %f", id, &x, &y, &paused, &scale) == 5) {
            
            // Check collision on server side
            {
                std::lock_guard<std::mutex> lock(gameStateMutex);
                
                // Create player rectangle for collision detection (same as main.cpp)
                SDL_FRect playerRect = {x, y, CHARACTER_SIZE, CHARACTER_SIZE};
                bool hit = false;
                
                for (const auto& pipe : pipes) {
                    SDL_FRect topRect = {pipe.topX, pipe.topY, pipe.topW, pipe.topH};
                    SDL_FRect bottomRect = {pipe.bottomX, pipe.bottomY, pipe.bottomW, pipe.bottomH};
                    
                    // Use the same collision detection as main.cpp
                    if (aabbIntersect(playerRect, topRect) || aabbIntersect(playerRect, bottomRect)) {
                        hit = true;
                        break;
                    }
                }
                
                if (hit) {
                    // Clear pipes on collision (exactly like main.cpp)
                    pipes.clear();
                    spawnTimer = 0.0;
                }
                
                players[clientId] = PlayerData{ Vec2{x,y}, paused==1, scale };
            }
        }
    }

    std::string generateWorldState() override {
        // Update pipes first (Section 4: Server-controlled environment)
        updatePipes();
        
        // Build response with current game state
        std::lock_guard<std::mutex> lock(gameStateMutex);
        
        std::string response = "N " + std::to_string(players.size()) + " P " + std::to_string(pipes.size()) + "\n";
        
        // Add player data (including pause state)
        for (auto& kv : players) {
            response += kv.first + " " + 
                       std::to_string(kv.second.pos.x) + " " + 
                       std::to_string(kv.second.pos.y) + " " + 
                       std::to_string(kv.second.paused ? 1 : 0) + " " + 
                       std::to_string(kv.second.scale) + "\n";
        }
        
        // Add pipe data
        for (const auto& pipe : pipes) {
            response += std::to_string(pipe.topX) + " " + std::to_string(pipe.topY) + " " + 
                       std::to_string(pipe.topW) + " " + std::to_string(pipe.topH) + " " +
                       std::to_string(pipe.bottomX) + " " + std::to_string(pipe.bottomY) + " " + 
                       std::to_string(pipe.bottomW) + " " + std::to_string(pipe.bottomH) + "\n";
        }
        
        return response;
    }

    void onClientConnected(const std::string& clientId) override {
        std::cout << "Client connected: " << clientId << std::endl;
    }

    void onClientDisconnected(const std::string& clientId) override {
        std::lock_guard<std::mutex> lock(gameStateMutex);
        players.erase(clientId);
        std::cout << "Client disconnected: " << clientId << std::endl;
    }

private:
    void updatePipes() {
        std::lock_guard<std::mutex> lock(gameStateMutex);
        
        clock_t currentTime = clock();
        double dtSec = (double)(currentTime - lastPipeUpdate) / CLOCKS_PER_SEC;
        lastPipeUpdate = currentTime;
        
        if (dtSec > 0.1) dtSec = 0.1;
        
        // Spawn new pipes
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
        
        // Move pipes
        for (auto& pipe : pipes) {
            pipe.topX += PIPE_SPEED * dtSec;
            pipe.bottomX += PIPE_SPEED * dtSec;
        }
        
        // Remove off-screen pipes
        pipes.erase(std::remove_if(pipes.begin(), pipes.end(),
                    [](const PipePair& p) { return (p.topX + p.topW) < -50.f; }), pipes.end());
    }
};

int main(){
    GameServer server;
    
    std::cout << "Starting game server on port 5555..." << std::endl;
    server.startServer(5555);
    
    // Keep server running until interrupted
    std::cout << "Server running. Press Ctrl+C to stop." << std::endl;
    while (server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    server.stopServer();
    return 0;
}