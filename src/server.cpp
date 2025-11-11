#include "network_server.h"
#include "object_model.h"
#include "registry.h"
#include "event_manager.h"
#include "timeline.h"
#include <iostream>
#include <cstdio>
#include <chrono>
#include <thread>
#include <atomic>

struct Platform {
    float x, y, w, h;
    float speed;
    int dir;
    float minBound, maxBound;
    bool isVertical;
};

class SpikeyServer : public Engine::NetworkServer {
private:
    Engine::Registry registry;
    Timeline serverTimeline;  // Timeline is not in Engine namespace
    Engine::EventManager eventManager;
    
    Platform horizontalPlatform;
    Platform verticalPlatform;
    bool bridge1Visible;
    float bridgeToggleTimer;
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point lastUpdate;
    
    static constexpr int DESIGN_WIDTH = 1720;
    static constexpr int DESIGN_HEIGHT = 1080;
    static constexpr float platformWidth = 200.f;
    static constexpr float platformHeight = 350.f;
    static constexpr float leftX = 0.f;
    static constexpr float leftY = DESIGN_HEIGHT - platformHeight;
    static constexpr float middleX = DESIGN_WIDTH / 2.f - platformWidth / 2.f - 200.f;
    static constexpr float middleY = DESIGN_HEIGHT - platformHeight;
    static constexpr float platform3X = DESIGN_WIDTH - platformWidth - 470.f;
    static constexpr float platform3TopY = DESIGN_HEIGHT - platformHeight - 200.f;
    
    static constexpr int CLIENT_TIMEOUT_SECONDS = 3;
    std::thread timeoutThread;
    std::atomic<bool> timeoutRunning{false};
    
    // Store events to broadcast
    std::vector<std::string> eventsToBroadcast;
    std::mutex eventBroadcastMutex;

public:
    SpikeyServer() : eventManager(&serverTimeline) {
        serverTimeline.anchorToRealTime();
        
        // Register server-side event listeners
        eventManager.registerListener(Engine::EventType::Death, [this](const Engine::Event& e) {
            std::string who = std::get<std::string>(e.payload.at("entity"));
            std::cout << "[SERVER EVENT] Death: " << who << " at time " << e.timestamp << std::endl;
        });
        
        eventManager.registerListener(Engine::EventType::Collision, [this](const Engine::Event& e) {
            std::string objA = std::get<std::string>(e.payload.at("A"));
            std::string objB = std::get<std::string>(e.payload.at("B"));
            std::cout << "[SERVER EVENT] Collision: " << objA << " <-> " << objB << std::endl;
        });
        
        eventManager.registerListener(Engine::EventType::Input, [this](const Engine::Event& e) {
            std::string key = std::get<std::string>(e.payload.at("key"));
            bool pressed = std::get<bool>(e.payload.at("pressed"));
            std::cout << "[SERVER EVENT] Input: " << key << " " << (pressed ? "pressed" : "released") << std::endl;
        });
        
        eventManager.registerListener(Engine::EventType::Spawn, [this](const Engine::Event& e) {
            std::string who = std::get<std::string>(e.payload.at("entity"));
            float x = std::get<float>(e.payload.at("x"));
            float y = std::get<float>(e.payload.at("y"));
            std::cout << "[SERVER EVENT] Spawn: " << who << " at (" << x << ", " << y << ")" << std::endl;
        });
        
        std::cout << "[SERVER] Event system initialized" << std::endl;
        
        horizontalPlatform = {
            leftX + platformWidth + 20.f,
            DESIGN_HEIGHT - platformHeight - 150.f,
            144.f, 72.f,
            200.f, 1,
            platformWidth, middleX,
            false
        };
        
        verticalPlatform = {
            middleX + platformWidth + 40.f,
            middleY - 180.f,
            144.f, 68.f,
            150.f, 1,
            platform3TopY - 100.f, middleY - 120.f,
            true
        };
        
        bridge1Visible = true;
        bridgeToggleTimer = 0.f;
        
        startTime = std::chrono::steady_clock::now();
        lastUpdate = startTime;
        setWorldUpdateRate(60);
        
        startTimeoutDetection();
    }
    
    ~SpikeyServer() {
        stopTimeoutDetection();
    }

protected:
    void handleClientMessage(const std::string& clientId, const std::string& message) override {
        // Check if this is an event message
        if (message.find("EVENT ") == 0) {
            std::string eventData = message.substr(6);
            eventManager.raiseEventFromNetwork(eventData);
            
            // Store event to broadcast in the world state
            std::lock_guard<std::mutex> lock(eventBroadcastMutex);
            eventsToBroadcast.push_back(eventData);
            return;
        }
        
        char id[256];
        float x = 0, y = 0;
        
        if (sscanf(message.c_str(), "ID %255s X %f Y %f", id, &x, &y) == 3) {
            std::string playerId(id);
            
            auto& playerObj = registry.upsert(playerId);
            playerObj.set<Engine::Vec2>("pos", {x, y});
            
            auto now = std::chrono::steady_clock::now();
            float timeSinceStart = std::chrono::duration<float>(now - startTime).count();
            playerObj.set<float>("last_update", timeSinceStart);
        }
    }
    
    std::string generateWorldState() override {
        // Tick the server timeline
        serverTimeline.tick();
        
        // Dispatch events
        eventManager.dispatchEvents();
        
        auto now = std::chrono::steady_clock::now();
        float currentTime = std::chrono::duration<float>(now - startTime).count();
        float deltaTime = std::chrono::duration<float>(now - lastUpdate).count();
        lastUpdate = now;
        
        bridgeToggleTimer += deltaTime;
        if (bridge1Visible && bridgeToggleTimer > 3.0f) {
            bridge1Visible = false;
            bridgeToggleTimer = 0.f;
        } else if (!bridge1Visible && bridgeToggleTimer > 2.0f) {
            bridge1Visible = true;
            bridgeToggleTimer = 0.f;
        }
        
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
        
        char buffer[16384];
        int offset = 0;
        
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "T %.3f\n", currentTime);
        
        auto playerIds = registry.getAllIds();
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "N %zu\n", playerIds.size());
        
        for (const auto& playerId : playerIds) {
            const auto* playerObj = registry.get(playerId);
            if (playerObj) {
                auto pos = playerObj->get<Engine::Vec2>("pos", {0.f, 0.f});
                offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                  "%s %.3f %.3f\n", playerId.c_str(), pos.x, pos.y);
            }
        }
        
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                          "HP %.3f %.3f %.3f %.3f %d\n",
                          horizontalPlatform.x, horizontalPlatform.y, 
                          horizontalPlatform.w, horizontalPlatform.h, 
                          horizontalPlatform.dir);
        
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                          "VP %.3f %.3f %.3f %.3f %d\n",
                          verticalPlatform.x, verticalPlatform.y, 
                          verticalPlatform.w, verticalPlatform.h, 
                          verticalPlatform.dir);
        
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                          "B1 %d\n", bridge1Visible ? 1 : 0);
        
        // Add events to broadcast
        {
            std::lock_guard<std::mutex> lock(eventBroadcastMutex);
            for (const auto& eventData : eventsToBroadcast) {
                offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                  "EVENT %s\n", eventData.c_str());
            }
            eventsToBroadcast.clear();
        }
        
        return std::string(buffer, offset);
    }
    
    void onClientConnected(const std::string& clientId) override {
        std::cout << "Spikey player joined: " << clientId << std::endl;
        
        auto& playerObj = registry.upsert(clientId);
        playerObj.set<Engine::Vec2>("pos", {leftX + 80.f, leftY - 256.f});
        playerObj.set<bool>("active", true);
        
        auto now = std::chrono::steady_clock::now();
        float timeSinceStart = std::chrono::duration<float>(now - startTime).count();
        playerObj.set<float>("spawn_time", timeSinceStart);
        playerObj.set<float>("last_update", timeSinceStart);
        
        // Generate spawn event
        auto spawnEv = Engine::Events::Spawn(clientId, leftX + 80.f, leftY - 256.f, &serverTimeline, 1);
        eventManager.raiseEvent(spawnEv);
        
        // Add to broadcast queue
        std::lock_guard<std::mutex> lock(eventBroadcastMutex);
        eventsToBroadcast.push_back(spawnEv.serialize());
    }
    
    void onClientDisconnected(const std::string& clientId) override {
        std::cout << "Spikey player left: " << clientId << std::endl;
        registry.erase(clientId);
        std::cout << "Remaining players: " << registry.size() << std::endl;
    }

private:
    void startTimeoutDetection() {
        if (timeoutRunning.exchange(true)) return;
        
        timeoutThread = std::thread([this]() {
            std::cout << "[SERVER] Timeout detection thread started (timeout: " 
                      << CLIENT_TIMEOUT_SECONDS << "s)" << std::endl;
            
            while (timeoutRunning) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                
                auto now = std::chrono::steady_clock::now();
                float currentTime = std::chrono::duration<float>(now - startTime).count();
                
                std::vector<std::string> timedOutPlayers;
                
                auto playerIds = registry.getAllIds();
                for (const auto& playerId : playerIds) {
                    const auto* playerObj = registry.get(playerId);
                    if (playerObj) {
                        float lastUpdate = playerObj->get<float>("last_update", currentTime);
                        float timeSinceUpdate = currentTime - lastUpdate;
                        
                        if (timeSinceUpdate >= CLIENT_TIMEOUT_SECONDS) {
                            timedOutPlayers.push_back(playerId);
                        }
                    }
                }
                
                for (const auto& playerId : timedOutPlayers) {
                    std::cout << "[SERVER] Player timed out (>" << CLIENT_TIMEOUT_SECONDS 
                              << "s): " << playerId << std::endl;
                    registry.erase(playerId);
                }
            }
            
            std::cout << "[SERVER] Timeout detection thread stopped" << std::endl;
        });
    }
    
    void stopTimeoutDetection() {
        if (!timeoutRunning.exchange(false)) return;
        if (timeoutThread.joinable()) {
            timeoutThread.join();
        }
    }
};

int main() {
    SpikeyServer server;
    server.startServer(5555);
    
    std::cout << "Spikey Server running with Event System..." << std::endl;
    std::cout << "Press Enter to stop..." << std::endl;
    std::cin.get();
    
    server.stopServer();
    return 0;
}