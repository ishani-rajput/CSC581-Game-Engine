#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <zmq.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstdio>
#include <cstring>
#include <random>
#include "entity.h"
#include "input.h"
#include "scaling.h"
#include "physics.h"
#include "collision.h"
#include "timeline.h"

const int DESIGN_WIDTH = 1720;
const int DESIGN_HEIGHT = 1080;

const float PLAYER_SPEED = 300.f;
const float JUMP_VELOCITY = -1000.f;

struct RemotePlayer {
    float x, y;
    Entity* entity;
    std::string id;
    bool active;
    
    RemotePlayer() : x(0), y(0), entity(nullptr), active(false) {}
};

bool checkCollision(const SDL_FRect& a, const SDL_FRect& b) {
    return aabbIntersect(a, b);
}

void clampPlayerPosition(float& playerX, float& playerY, float playerWidth, float playerHeight) {
    if (playerX < 0) playerX = 0;
    if (playerX + playerWidth > DESIGN_WIDTH) playerX = DESIGN_WIDTH - playerWidth;
    
    if (playerY > DESIGN_HEIGHT) {
        playerY = DESIGN_HEIGHT - playerHeight;
    }
}

std::string generateClientId() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    return "client_" + std::to_string(dis(gen));
}

int main(int, char**) {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("Feeling Spikey - Multiplayer Client", 
                                         DESIGN_WIDTH, DESIGN_HEIGHT, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    Scaling::setMode(ScaleMode::Proportional);

    Physics::setGravity(2000.f);

    // Initialize Dual Timeline System
    Timeline personalTimeline;  // For player movement - pause/speed controls
    personalTimeline.anchorToRealTime();
    
    Timeline worldTimeline;     // For world objects - always synchronized
    worldTimeline.anchorToRealTime();
    
    // Initialize ZMQ
    void* zmqContext = zmq_ctx_new();
    void* zmqSocket = zmq_socket(zmqContext, ZMQ_REQ);
    
    if (zmq_connect(zmqSocket, "tcp://localhost:5555") != 0) {
        SDL_Log("Failed to connect to server: %s", zmq_strerror(errno));
        return 1;
    }
    
    std::string clientId = generateClientId();
    SDL_Log("Client ID: %s", clientId.c_str());

    // Game entities
    Entity groundBottom(renderer, "../assets/ground_bottom.png", 0, 0, 64, 64, 1, 0);
    Entity groundTop(renderer, "../assets/ground.png", 0, 0, 64, 64, 1, 0);
    Entity platform(renderer, "../assets/platform.png", 0, 0, 384, 128, 1, 0);
    Entity spike(renderer, "../assets/spikes.png", 0, 0, 256, 256, 1, 0);

    // Local player setup
    int frameCount = 8;
    int frameWidth = 128;
    int frameHeight = 128;
    float playerScale = 2.0f;
    float playerWidth = frameWidth * playerScale;
    float playerHeight = frameHeight * playerScale;
    
    Entity localPlayer(renderer, "../assets/player.png", 0, 0, frameWidth, frameHeight, frameCount, 150);

    // Remote players storage
    std::unordered_map<std::string, RemotePlayer> remotePlayers;

    // Platform setup
    float platformWidth = 400.f;
    float platformHeight = 500.f;
    float leftX = 0.f;
    float leftY = DESIGN_HEIGHT - platformHeight;
    float rightX = DESIGN_WIDTH - platformWidth;
    float rightY = DESIGN_HEIGHT - platformHeight;

    // Client-managed moving platform (deterministic)
    SDL_FRect movingPlatform = { 
        DESIGN_WIDTH / 2.f - 192, 
        DESIGN_HEIGHT - platformHeight - 200, 
        384, 
        128 
    };
    float platformSpeed = 200.f;
    int platformDir = 1;

    // Local player state
    float playerX = leftX + 100.f;
    float playerY = leftY - playerHeight;
    Body playerBody = { 0.f, 0.f, true };
    
    float prevPlayerX = playerX;
    float prevPlayerY = playerY;

    bool running = true;
    bool wasGrounded = false;
    bool isGrounded = false;
    bool serverConnected = true;
    
    int networkUpdateCounter = 0;
    const int NETWORK_UPDATE_FREQUENCY = 3; // Send updates every 3 frames

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) 
                running = false;
            
            if (e.type == SDL_EVENT_WINDOW_RESIZED) {
                clampPlayerPosition(playerX, playerY, playerWidth, playerHeight);
            }
            
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_S) {
                if (Scaling::mode() == ScaleMode::Pixel) {
                    Scaling::setMode(ScaleMode::Proportional);
                    SDL_Log("Switched to Proportional scaling mode");
                } else {
                    Scaling::setMode(ScaleMode::Pixel);
                    SDL_Log("Switched to Pixel scaling mode");
                }
            }

            // Timeline Controls (only affect personal timeline)
            if (e.type == SDL_EVENT_KEY_DOWN) {
                switch (e.key.scancode) {
                    case SDL_SCANCODE_P:
                        personalTimeline.togglePause();
                        SDL_Log(personalTimeline.isPaused() ? "Personal timeline PAUSED" : "Personal timeline RESUMED");
                        break;
                    case SDL_SCANCODE_1:
                        personalTimeline.setScale(0.5);
                        SDL_Log("Personal speed: SLOW (0.5x)");
                        break;
                    case SDL_SCANCODE_2:
                        personalTimeline.setScale(1.0);
                        SDL_Log("Personal speed: NORMAL (1.0x)");
                        break;
                    case SDL_SCANCODE_3:
                        personalTimeline.setScale(2.0);
                        SDL_Log("Personal speed: FAST (2.0x)");
                        break;
                }
            }
        }

        Input::poll();

        // Get time deltas from both timelines
        double personalDeltaTime = personalTimeline.tick();
        double worldDeltaTime = worldTimeline.tick();
        
        float personalDT = static_cast<float>(personalDeltaTime);
        float worldDT = static_cast<float>(worldDeltaTime);
        
        if (personalDT > 0.05f) personalDT = 0.05f;
        if (worldDT > 0.05f) worldDT = 0.05f;

        // Update game logic - personal vs world objects use different timelines
        if (!personalTimeline.isPaused()) {
            prevPlayerX = playerX;
            prevPlayerY = playerY;

            playerBody.vx = 0.f;
            
            // Local player input
            if (Input::isKeyPressed(SDL_SCANCODE_A)) {
                playerBody.vx = -PLAYER_SPEED;
            }
            if (Input::isKeyPressed(SDL_SCANCODE_D)) {
                playerBody.vx = PLAYER_SPEED;
            }
            if ((Input::isKeyPressed(SDL_SCANCODE_W) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) && wasGrounded) {
                playerBody.vy = JUMP_VELOCITY;
            }

            // Physics update - uses personal timeline
            Physics::step(personalDT * 1000, playerX, playerY, playerBody);
            clampPlayerPosition(playerX, playerY, playerWidth, playerHeight);

            // Collision detection with platforms
            SDL_FRect playerRect = { playerX, playerY, playerWidth, playerHeight };
            isGrounded = false;

            // Static platforms collision
            SDL_FRect leftPlatformRect = { leftX, leftY, platformWidth, platformHeight };
            if (checkCollision(playerRect, leftPlatformRect)) {
                if (playerBody.vy >= 0 && prevPlayerY + playerHeight <= leftY + 20) {
                    playerY = leftY - playerHeight;
                    playerBody.vy = 0;
                    isGrounded = true;
                } else if (playerBody.vx < 0 && prevPlayerX >= leftX + platformWidth - 10) {
                    playerX = leftX + platformWidth;
                    playerBody.vx = 0;
                } else if (playerBody.vx > 0 && prevPlayerX + playerWidth <= leftX + 10) {
                    playerX = leftX - playerWidth;
                    playerBody.vx = 0;
                }
            }

            SDL_FRect rightPlatformRect = { rightX, rightY, platformWidth, platformHeight };
            if (checkCollision(playerRect, rightPlatformRect)) {
                if (playerBody.vy >= 0 && prevPlayerY + playerHeight <= rightY + 20) {
                    playerY = rightY - playerHeight;
                    playerBody.vy = 0;
                    isGrounded = true;
                } else if (playerBody.vx < 0 && prevPlayerX >= rightX + platformWidth - 10) {
                    playerX = rightX + platformWidth;
                    playerBody.vx = 0;
                } else if (playerBody.vx > 0 && prevPlayerX + playerWidth <= rightX + 10) {
                    playerX = rightX - playerWidth;
                    playerBody.vx = 0;
                }
            }

            // Client-managed moving platform collision (deterministic)
            SDL_FRect movingPlatformRect = { movingPlatform.x, movingPlatform.y, 
                                           movingPlatform.w, movingPlatform.h };
            if (checkCollision(playerRect, movingPlatformRect)) {
                if (playerBody.vy >= 0 && prevPlayerY + playerHeight <= movingPlatform.y + 20) {
                    playerY = movingPlatform.y - playerHeight;
                    playerBody.vy = 0;
                    playerX += platformSpeed * platformDir * worldDT; // Platform movement uses world timeline
                    isGrounded = true;
                }
            }

            // Spike collision
            float spikeW = 300.f;
            float spikeH = 389.f;
            float spikeY = DESIGN_HEIGHT - spikeH;
            for (float x = platformWidth; x < DESIGN_WIDTH - platformWidth; x += spikeW) {
                SDL_FRect spikeRect = { x, spikeY, spikeW, spikeH };
                if (checkCollision(playerRect, spikeRect)) {
                    playerX = leftX + 100.f;
                    playerY = leftY - playerHeight;
                    playerBody.vx = 0.f;
                    playerBody.vy = 0.f;
                    SDL_Log("You died! Resetting...");
                    break;
                }
            }

            // Fall off screen reset
            if (playerY > DESIGN_HEIGHT + 100) {
                playerX = leftX + 100.f;
                playerY = leftY - playerHeight;
                playerBody.vx = 0.f;
                playerBody.vy = 0.f;
            }

            wasGrounded = isGrounded;
            localPlayer.update(); // Player animation uses personal timeline
        }
        
        // World objects update regardless of personal pause state (always synchronized)
        // Update moving platform with world timeline (synchronized across all clients)
        movingPlatform.x += platformSpeed * platformDir * worldDT;
        if (movingPlatform.x < platformWidth) {
            movingPlatform.x = platformWidth;
            platformDir = 1;
        } else if (movingPlatform.x + movingPlatform.w > DESIGN_WIDTH - platformWidth) {
            movingPlatform.x = DESIGN_WIDTH - platformWidth - movingPlatform.w;
            platformDir = -1;
        }

        // ---- Networking (simple pattern) ----
        networkUpdateCounter++;
        if (networkUpdateCounter >= NETWORK_UPDATE_FREQUENCY && serverConnected) {
            networkUpdateCounter = 0;
            
            char msg[256];
            snprintf(msg, sizeof(msg), "ID %s X %.3f Y %.3f", clientId.c_str(), playerX, playerY);

            if (zmq_send(zmqSocket, msg, strlen(msg), 0) != -1) {
                char rx[8192];
                int rb = zmq_recv(zmqSocket, rx, sizeof(rx)-1, 0);
                if (rb > 0) {
                    rx[rb] = '\0';
                    
                    // Parse simple response: "N <count>\n<player_data>..."
                    size_t numPlayers = 0;
                    if (sscanf(rx, "N %zu", &numPlayers) == 1) {
                        const char* lines = strchr(rx, '\n');
                        
                        // Mark all as inactive first
                        for (auto& pair : remotePlayers) {
                            pair.second.active = false;
                        }
                        
                        // Parse player lines
                        while (lines) {
                            lines++;
                            char id[256];
                            float x = 0, y = 0;
                            if (sscanf(lines, "%255s %f %f", id, &x, &y) == 3 && clientId != id) {
                                remotePlayers[id].x = x;
                                remotePlayers[id].y = y;
                                remotePlayers[id].id = id;
                                remotePlayers[id].active = true;
                            }
                            lines = strchr(lines, '\n');
                        }
                        
                        // Remove inactive and create entities for new players
                        auto it = remotePlayers.begin();
                        while (it != remotePlayers.end()) {
                            if (!it->second.active) {
                                if (it->second.entity) {
                                    delete it->second.entity;
                                }
                                it = remotePlayers.erase(it);
                            } else {
                                // Create entity if needed
                                if (it->second.entity == nullptr) {
                                    it->second.entity = new Entity(renderer, "../assets/player.png", 
                                                                   0, 0, frameWidth, frameHeight, frameCount, 150);
                                    SDL_Log("New remote player: %s", it->first.c_str());
                                }
                                ++it;
                            }
                        }
                    }
                } else {
                    SDL_Log("Failed to receive server response");
                }
            } else {
                SDL_Log("Failed to send to server");
                serverConnected = false;
            }
        }

        // Rendering
        SDL_SetRenderDrawColor(renderer, 135, 206, 235, 255);
        SDL_RenderClear(renderer);

        // Render spikes
        float spikeW = 300.f;
        float spikeH = 389.f;
        float spikeY = DESIGN_HEIGHT - spikeH;
        for (float x = platformWidth; x < DESIGN_WIDTH - platformWidth; x += spikeW) {
            spike.setPosition(x, spikeY);
            spike.setSize(spikeW, spikeH);
            spike.render(renderer, window);
        }

        // Render static platforms
        groundBottom.setPosition(leftX, leftY);
        groundBottom.setSize(platformWidth, platformHeight);
        groundBottom.render(renderer, window);

        groundTop.setPosition(leftX, leftY - 64);
        groundTop.setSize(platformWidth, 64);
        groundTop.render(renderer, window);

        groundBottom.setPosition(rightX, rightY);
        groundBottom.setSize(platformWidth, platformHeight);
        groundBottom.render(renderer, window);

        groundTop.setPosition(rightX, rightY - 64);
        groundTop.setSize(platformWidth, 64);
        groundTop.render(renderer, window);

        // Render client-calculated moving platform
        platform.setPosition(movingPlatform.x, movingPlatform.y);
        platform.setSize(movingPlatform.w, movingPlatform.h);
        platform.render(renderer, window);

        // Render local player
        localPlayer.setPosition(playerX, playerY);
        localPlayer.setSize(playerWidth, playerHeight);
        localPlayer.render(renderer, window);

        // Render remote players
        for (auto& pair : remotePlayers) {
            if (pair.second.entity) {
                pair.second.entity->setPosition(pair.second.x, pair.second.y);
                pair.second.entity->setSize(playerWidth, playerHeight);
                pair.second.entity->render(renderer, window);
            }
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    // Cleanup
    for (auto& pair : remotePlayers) {
        if (pair.second.entity) {
            delete pair.second.entity;
        }
    }

    zmq_close(zmqSocket);
    zmq_ctx_destroy(zmqContext);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}