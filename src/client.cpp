// Part 1A: Uses Engine::Registry and GameObject for remote players
// Part 1B: Multithreaded client (network thread + main thread)

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <zmq.h>
#include <thread>
#include <mutex>
#include <atomic>
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
#include "object_model.h"  // Part 1A
#include "registry.h"      // Part 1A

const int DESIGN_WIDTH  = 1720;
const int DESIGN_HEIGHT = 1080;

const float PLAYER_SPEED   = 300.f;
const float JUMP_VELOCITY  = -750.f;

const float platformWidth = 200.f;
const float platformHeight = 350.f;
const float bridgeWidth = 135.f;
const float bridgeHeight = 54.f;

const float leftX = 0.f;
const float leftY = DESIGN_HEIGHT - platformHeight - 70.f;
const float middleX = DESIGN_WIDTH / 2.f - platformWidth / 2.f - 200.f;
const float middleY = DESIGN_HEIGHT - platformHeight;
const float platform3X = DESIGN_WIDTH - platformWidth - 470.f;
const float platform3TopY = DESIGN_HEIGHT - platformHeight - 200.f;
const float platform3Height = DESIGN_HEIGHT - platform3TopY;
const float bridge1X = platform3X + platformWidth + 70.f;
const float bridge1Y = platform3TopY + 100.f;
const float finalX = DESIGN_WIDTH - platformWidth;
const float finalY = DESIGN_HEIGHT - platformHeight;

// Part 1A: Remote player entities stored with GameObjects
struct RemotePlayerEntity {
    Entity* entity = nullptr;
};

struct ServerState {
    SDL_FRect horizontalPlatform;
    int horizontalDir;
    SDL_FRect verticalPlatform;
    int verticalDir;
    bool bridge1Visible;
    std::mutex mutex;
};

struct LocalPlayerState {
    float x, y;
    float vx, vy;
    bool grounded;
    std::mutex mutex;
};

static inline bool checkCollision(const SDL_FRect& a, const SDL_FRect& b) {
    return aabbIntersect(a, b);
}

static inline void clampPlayerPosition(float& playerX, float& playerY, float w, float h) {
    if (playerX < 0) playerX = 0;
    if (playerX + w > DESIGN_WIDTH) playerX = DESIGN_WIDTH - w;
    if (playerY > DESIGN_HEIGHT) playerY = DESIGN_HEIGHT - h;
}

static void renderSimpleText(SDL_Renderer* renderer, const std::string& text, float x, float y, 
                             uint8_t r = 255, uint8_t g = 255, uint8_t b = 255) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
    float textWidth = text.length() * 9.f;
    SDL_FRect bgRect = {x - 4, y - 2, textWidth, 16};
    SDL_RenderFillRect(renderer, &bgRect);
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    SDL_RenderDebugText(renderer, x, y, text.c_str());
}

static std::string generateClientId() {
    std::random_device rd; std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    return "client_" + std::to_string(dis(gen));
}

int main(int, char**) {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("Feeling Spikey - Part 1 Compliant Client",
                                          DESIGN_WIDTH, DESIGN_HEIGHT, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    Scaling::setMode(ScaleMode::Proportional);

    Physics::setGravity(2000.f);

    Timeline gameTimeline; 
    gameTimeline.anchorToRealTime();

    void* ctx  = zmq_ctx_new();
    void* sock = zmq_socket(ctx, ZMQ_REQ);
    if (zmq_connect(sock, "tcp://localhost:5555") != 0) {
        SDL_Log("Failed to connect: %s", zmq_strerror(errno));
        return 1;
    }
    std::string clientId = generateClientId();
    SDL_Log("[Part 1B] Client ID: %s", clientId.c_str());

    Entity groundBottom(renderer, "../assets/ground_bottom.png", 0, 0, 64, 64, 1, 0);
    Entity groundTop   (renderer, "../assets/ground.png",        0, 0, 64, 64, 1, 0);
    Entity platformTex (renderer, "../assets/platform.png",      0, 0, 384, 128, 1, 0);
    Entity spikeTex    (renderer, "../assets/spikes.png",        0, 0, 256, 256, 1, 0);
    Entity flagTex     (renderer, "../assets/flag.png",          0, 0, 256, 256, 1, 0);

    const int frameCount = 8, frameWidth = 128, frameHeight = 128;
    const float playerScale = 1.1f;
    const float playerW = frameWidth * playerScale;
    const float playerH = frameHeight * playerScale;

    Entity localPlayer(renderer, "../assets/player.png", 0, 0, frameWidth, frameHeight, frameCount, 150);

    // Part 1A: Use Registry for remote players (GameObject model)
    Engine::Registry remotePlayersRegistry;
    std::unordered_map<std::string, RemotePlayerEntity> remoteEntities;
    std::mutex remotePlayersMutex;

    ServerState serverState;
    serverState.horizontalPlatform = {leftX + platformWidth + 20.f, DESIGN_HEIGHT - platformHeight - 150.f, 144.f, 72.f};
    serverState.horizontalDir = 1;
    serverState.verticalPlatform = {middleX + platformWidth + 40.f, middleY - 180.f, 144.f, 68.f};
    serverState.verticalDir = 1;
    serverState.bridge1Visible = true;

    LocalPlayerState localPlayerState;
    localPlayerState.x = leftX + 80.f;
    localPlayerState.y = leftY - playerH;
    localPlayerState.vx = 0.f;
    localPlayerState.vy = 0.f;
    localPlayerState.grounded = false;

    Body playerBody = { 0.f, 0.f, true };

    std::atomic<bool> running{true};
    std::atomic<bool> gameWon{false};
    
    std::string statusMessage = "";
    int statusMessageTimer = 0;

    // Part 1B: Network thread for concurrent reading/sending
    std::thread networkThread([&]() {
        float networkTimer = 0.0f;
        auto lastTick = SDL_GetTicks();
        
        SDL_Log("[Part 1B] Network thread started");
        
        while (running) {
            auto now = SDL_GetTicks();
            float dt = (now - lastTick) / 1000.0f;
            lastTick = now;
            networkTimer += dt;

            if (networkTimer >= (1.0f / 60.0f)) {
                float px, py;
                {
                    std::lock_guard<std::mutex> lock(localPlayerState.mutex);
                    px = localPlayerState.x;
                    py = localPlayerState.y;
                }

                char msg[256];
                snprintf(msg, sizeof(msg), "ID %s X %.3f Y %.3f", clientId.c_str(), px, py);
                zmq_send(sock, msg, strlen(msg), 0);

                char rx[16384];
                int rb = zmq_recv(sock, rx, sizeof(rx)-1, 0);
                if (rb > 0) {
                    rx[rb] = '\0';

                    // Part 1A: Mark all remote players inactive before update
                    {
                        std::lock_guard<std::mutex> lock(remotePlayersMutex);
                        for (const auto& id : remotePlayersRegistry.getAllIds()) {
                            auto& obj = remotePlayersRegistry.upsert(id);
                            obj.set<bool>("active", false);
                        }
                    }

                    const char* line = std::strtok(rx, "\n");
                    while (line) {
                        if (strncmp(line, "HP ", 3) == 0) {
                            float x, y, w, h; int dir;
                            if (sscanf(line, "HP %f %f %f %f %d", &x, &y, &w, &h, &dir) == 5) {
                                std::lock_guard<std::mutex> lock(serverState.mutex);
                                serverState.horizontalPlatform = {x, y, w, h};
                                serverState.horizontalDir = dir;
                            }
                        }
                        else if (strncmp(line, "VP ", 3) == 0) {
                            float x, y, w, h; int dir;
                            if (sscanf(line, "VP %f %f %f %f %d", &x, &y, &w, &h, &dir) == 5) {
                                std::lock_guard<std::mutex> lock(serverState.mutex);
                                serverState.verticalPlatform = {x, y, w, h};
                                serverState.verticalDir = dir;
                            }
                        }
                        else if (strncmp(line, "B1 ", 3) == 0) {
                            int visible;
                            if (sscanf(line, "B1 %d", &visible) == 1) {
                                std::lock_guard<std::mutex> lock(serverState.mutex);
                                serverState.bridge1Visible = (visible != 0);
                            }
                        }
                        else if (line[0] != 'T' && line[0] != 'N') {
                            char id[256]; float rx_, ry_;
                            if (sscanf(line, "%255s %f %f", id, &rx_, &ry_) == 3) {
                                std::string remoteId(id);
                                if (remoteId != clientId) {
                                    // Part 1A: Store remote player as GameObject
                                    std::lock_guard<std::mutex> lock(remotePlayersMutex);
                                    auto& remoteObj = remotePlayersRegistry.upsert(remoteId);
                                    remoteObj.set<Engine::Vec2>("pos", {rx_, ry_});
                                    remoteObj.set<bool>("active", true);
                                    
                                    // Create Entity if needed
                                    if (remoteEntities.find(remoteId) == remoteEntities.end() ||
                                        !remoteEntities[remoteId].entity) {
                                        remoteEntities[remoteId].entity = 
                                            new Entity(renderer, "../assets/player.png",
                                                      0, 0, frameWidth, frameHeight, frameCount, 150);
                                    }
                                }
                            }
                        }
                        line = std::strtok(nullptr, "\n");
                    }

                    // Part 1A & 1B: Remove inactive remote players (disconnect handling)
                    {
                        std::lock_guard<std::mutex> lock(remotePlayersMutex);
                        auto allIds = remotePlayersRegistry.getAllIds();
                        for (const auto& id : allIds) {
                            const auto* obj = remotePlayersRegistry.get(id);
                            if (obj && !obj->get<bool>("active", false)) {
                                // Remove from registry
                                remotePlayersRegistry.erase(id);
                                
                                // Delete entity
                                auto it = remoteEntities.find(id);
                                if (it != remoteEntities.end() && it->second.entity) {
                                    delete it->second.entity;
                                    remoteEntities.erase(it);
                                }
                                
                                SDL_Log("[Part 1B] Remote player disconnected: %s", id.c_str());
                            }
                        }
                    }
                }
                
                networkTimer = 0.0f;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        
        SDL_Log("[Part 1B] Network thread stopped");
    });

    float prevHorizX = serverState.horizontalPlatform.x;
    float prevVertY = serverState.verticalPlatform.y;

    // Main loop: Input, physics, rendering
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) 
                running = false;
            
            if (e.type == SDL_EVENT_WINDOW_RESIZED) {
                std::lock_guard<std::mutex> lock(localPlayerState.mutex);
                clampPlayerPosition(localPlayerState.x, localPlayerState.y, playerW, playerH);
            }
            
            if (e.type == SDL_EVENT_KEY_DOWN) {
                switch (e.key.scancode) {
                    case SDL_SCANCODE_P:
                        gameTimeline.togglePause();
                        statusMessage = gameTimeline.isPaused() ? "PAUSED" : "RESUMED";
                        statusMessageTimer = 120;
                        break;
                    case SDL_SCANCODE_1:
                        gameTimeline.setScale(0.5);
                        statusMessage = "Speed: SLOW (0.5x)";
                        statusMessageTimer = 120;
                        break;
                    case SDL_SCANCODE_2:
                        gameTimeline.setScale(1.0);
                        statusMessage = "Speed: NORMAL (1.0x)";
                        statusMessageTimer = 120;
                        break;
                    case SDL_SCANCODE_3:
                        gameTimeline.setScale(2.0);
                        statusMessage = "Speed: FAST (2.0x)";
                        statusMessageTimer = 120;
                        break;
                    case SDL_SCANCODE_R:
                        if (gameWon) {
                            gameWon = false;
                            std::lock_guard<std::mutex> lock(localPlayerState.mutex);
                            localPlayerState.x = leftX + 80.f;
                            localPlayerState.y = leftY - playerH;
                            localPlayerState.vx = localPlayerState.vy = 0.f;
                            statusMessage = "Game Restarted";
                            statusMessageTimer = 120;
                        }
                        break;
                    case SDL_SCANCODE_S:
                        if (Scaling::mode() == ScaleMode::Pixel) {
                            Scaling::setMode(ScaleMode::Proportional);
                            statusMessage = "Proportional Scaling";
                        } else {
                            Scaling::setMode(ScaleMode::Pixel);
                            statusMessage = "Pixel Scaling";
                        }
                        statusMessageTimer = 120;
                        break;
                }
            }
        }

        double dt = gameTimeline.tick();
        if (dt > 0.05) dt = 0.05;

        Input::poll();

        float px, py;
        bool wasGrounded;
        {
            std::lock_guard<std::mutex> lock(localPlayerState.mutex);
            px = localPlayerState.x;
            py = localPlayerState.y;
            wasGrounded = localPlayerState.grounded;
        }

        float prevPX = px, prevPY = py;
        playerBody.vx = 0.f;

        if (Input::isKeyPressed(SDL_SCANCODE_A)) playerBody.vx = -PLAYER_SPEED;
        if (Input::isKeyPressed(SDL_SCANCODE_D)) playerBody.vx =  PLAYER_SPEED;
        if ((Input::isKeyPressed(SDL_SCANCODE_W) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) && wasGrounded) {
            playerBody.vy = JUMP_VELOCITY;
        }

        Physics::step(dt * 1000, px, py, playerBody);
        clampPlayerPosition(px, py, playerW, playerH);

        SDL_FRect playerRect { px, py, playerW, playerH };
        bool grounded = false;

        SDL_FRect leftRect  { leftX, leftY, platformWidth, platformHeight };
        SDL_FRect middleRect { middleX, middleY, platformWidth, platformHeight };
        SDL_FRect plat3Rect { platform3X, platform3TopY, platformWidth, platform3Height };
        SDL_FRect bridge1Rect { bridge1X, bridge1Y, bridgeWidth, bridgeHeight };
        SDL_FRect finalRect { finalX, finalY, platformWidth, platformHeight };

        bool bridge1Vis;
        {
            std::lock_guard<std::mutex> lock(serverState.mutex);
            bridge1Vis = serverState.bridge1Visible;
        }

        if (checkCollision(playerRect, leftRect) && playerBody.vy >= 0 && prevPY + playerH <= leftY + 20) {
            py = leftY - playerH; playerBody.vy = 0; grounded = true;
        }
        if (checkCollision(playerRect, middleRect) && playerBody.vy >= 0 && prevPY + playerH <= middleY + 20) {
            py = middleY - playerH; playerBody.vy = 0; grounded = true;
        }
        if (checkCollision(playerRect, plat3Rect) && playerBody.vy >= 0 && prevPY + playerH <= platform3TopY + 20) {
            py = platform3TopY - playerH; playerBody.vy = 0; grounded = true;
        }
        if (bridge1Vis && checkCollision(playerRect, bridge1Rect) && playerBody.vy >= 0 && prevPY + playerH <= bridge1Y + 20) {
            py = bridge1Y - playerH; playerBody.vy = 0; grounded = true;
        }
        if (checkCollision(playerRect, finalRect) && playerBody.vy >= 0 && prevPY + playerH <= finalY + 20) {
            py = finalY - playerH; playerBody.vy = 0; grounded = true;
            if (px > finalX + 50.f) {
                gameWon = true;
                statusMessage = "YOU WIN!";
                statusMessageTimer = 300;
            }
        }

        SDL_FRect horizontalPlat, verticalPlat;
        {
            std::lock_guard<std::mutex> lock(serverState.mutex);
            horizontalPlat = serverState.horizontalPlatform;
            verticalPlat = serverState.verticalPlatform;
        }

        SDL_FRect mpRect { horizontalPlat.x, horizontalPlat.y, horizontalPlat.w, horizontalPlat.h };
        if (checkCollision(playerRect, mpRect)) {
            if (playerBody.vy >= 0 && prevPY + playerH <= horizontalPlat.y + 20) {
                py = horizontalPlat.y - playerH;
                playerBody.vy = 0;
                float platDX = horizontalPlat.x - prevHorizX;
                px += platDX;
                grounded = true;
            }
        }
        prevHorizX = horizontalPlat.x;

        SDL_FRect vpRect { verticalPlat.x, verticalPlat.y, verticalPlat.w, verticalPlat.h };
        if (checkCollision(playerRect, vpRect)) {
            if (playerBody.vy >= 0 && prevPY + playerH <= verticalPlat.y + 20) {
                py = verticalPlat.y - playerH;
                playerBody.vy = 0;
                float platDY = verticalPlat.y - prevVertY;
                py += platDY;
                grounded = true;
            }
        }
        prevVertY = verticalPlat.y;

        float spikeW = 220.f, spikeH = 280.f, spikeY = DESIGN_HEIGHT - spikeH;
        for (float x = platformWidth; x < middleX; x += spikeW) {
            SDL_FRect spikeRect{ x, spikeY, spikeW, spikeH };
            if (checkCollision(playerRect, spikeRect)) {
                px = leftX + 80.f; py = leftY - playerH;
                playerBody.vx = playerBody.vy = 0;
                statusMessage = "OUCH! Respawned";
                statusMessageTimer = 120;
            }
        }
        for (float x = middleX + platformWidth; x < platform3X; x += spikeW) {
            SDL_FRect spikeRect{ x, spikeY, spikeW, spikeH };
            if (checkCollision(playerRect, spikeRect)) {
                px = leftX + 80.f; py = leftY - playerH;
                playerBody.vx = playerBody.vy = 0;
                statusMessage = "OUCH! Respawned";
                statusMessageTimer = 120;
            }
        }
        for (float x = platform3X + platformWidth; x < finalX; x += spikeW) {
            SDL_FRect spikeRect{ x, spikeY, spikeW, spikeH };
            if (checkCollision(playerRect, spikeRect)) {
                px = leftX + 80.f; py = leftY - playerH;
                playerBody.vx = playerBody.vy = 0;
                statusMessage = "OUCH! Respawned";
                statusMessageTimer = 120;
            }
        }

        if (py > DESIGN_HEIGHT + 100) {
            px = leftX + 80.f; py = leftY - playerH;
            playerBody.vx = playerBody.vy = 0;
            statusMessage = "Fell! Respawned";
            statusMessageTimer = 120;
        }

        {
            std::lock_guard<std::mutex> lock(localPlayerState.mutex);
            localPlayerState.x = px;
            localPlayerState.y = py;
            localPlayerState.vx = playerBody.vx;
            localPlayerState.vy = playerBody.vy;
            localPlayerState.grounded = grounded;
        }

        localPlayer.update();

        SDL_SetRenderDrawColor(renderer, 135, 206, 235, 255);
        SDL_RenderClear(renderer);

        for (float x = platformWidth; x < middleX; x += spikeW) {
            spikeTex.setPosition(x, spikeY);
            spikeTex.setSize(spikeW, spikeH);
            spikeTex.render(renderer, window);
        }
        for (float x = middleX + platformWidth; x < platform3X; x += spikeW) {
            spikeTex.setPosition(x, spikeY);
            spikeTex.setSize(spikeW, spikeH);
            spikeTex.render(renderer, window);
        }
        for (float x = platform3X + platformWidth; x < finalX; x += spikeW) {
            spikeTex.setPosition(x, spikeY);
            spikeTex.setSize(spikeW, spikeH);
            spikeTex.render(renderer, window);
        }

        groundBottom.setPosition(leftX, leftY);
        groundBottom.setSize(platformWidth, platformHeight);
        groundBottom.render(renderer, window);
        groundTop.setPosition(leftX, leftY - 48.f);
        groundTop.setSize(platformWidth, 48.f);
        groundTop.render(renderer, window);

        groundBottom.setPosition(middleX, middleY);
        groundBottom.setSize(platformWidth, platformHeight);
        groundBottom.render(renderer, window);
        groundTop.setPosition(middleX, middleY - 48.f);
        groundTop.setSize(platformWidth, 48.f);
        groundTop.render(renderer, window);

        groundBottom.setPosition(platform3X, platform3TopY);
        groundBottom.setSize(platformWidth, 100.f);
        groundBottom.render(renderer, window);
        groundTop.setPosition(platform3X, platform3TopY - 48.f);
        groundTop.setSize(platformWidth, 48.f);
        groundTop.render(renderer, window);

        if (bridge1Vis) {
            platformTex.setPosition(bridge1X, bridge1Y);
            platformTex.setSize(bridgeWidth, bridgeHeight);
            platformTex.render(renderer, window);
        }

        groundBottom.setPosition(finalX, finalY);
        groundBottom.setSize(platformWidth, platformHeight);
        groundBottom.render(renderer, window);
        groundTop.setPosition(finalX, finalY - 48.f);
        groundTop.setSize(platformWidth, 48.f);
        groundTop.render(renderer, window);

        float flagW = 150.f, flagH = 150.f;
        flagTex.setPosition(finalX + platformWidth - flagW - 30.f, finalY - flagH - 48.f);
        flagTex.setSize(flagW, flagH);
        flagTex.render(renderer, window);

        platformTex.setPosition(horizontalPlat.x, horizontalPlat.y);
        platformTex.setSize(horizontalPlat.w, horizontalPlat.h);
        platformTex.render(renderer, window);

        platformTex.setPosition(verticalPlat.x, verticalPlat.y);
        platformTex.setSize(verticalPlat.w, verticalPlat.h);
        platformTex.render(renderer, window);

        localPlayer.setPosition(px, py);
        localPlayer.setSize(playerW, playerH);
        localPlayer.render(renderer, window);

        // Part 1A: Render all remote players from Registry
        {
            std::lock_guard<std::mutex> lock(remotePlayersMutex);
            auto remoteIds = remotePlayersRegistry.getAllIds();
            for (const auto& remoteId : remoteIds) {
                const auto* remoteObj = remotePlayersRegistry.get(remoteId);
                if (remoteObj && remoteObj->get<bool>("active", false)) {
                    auto pos = remoteObj->get<Engine::Vec2>("pos", {0.f, 0.f});
                    
                    auto it = remoteEntities.find(remoteId);
                    if (it != remoteEntities.end() && it->second.entity) {
                        it->second.entity->update();
                        it->second.entity->setPosition(pos.x, pos.y);
                        it->second.entity->setSize(playerW, playerH);
                        it->second.entity->render(renderer, window);
                    }
                }
            }
        }

        renderSimpleText(renderer, "A/D: Move  W/SPACE: Jump", 20, 20, 255, 255, 255);
        renderSimpleText(renderer, "P: Pause  1/2/3: Speed  R: Restart  S: Scaling", 20, 40, 255, 255, 255);
        renderSimpleText(renderer, "[Part 1A] Using GameObject Registry", 20, 60, 0, 255, 255);

        std::string speedText = "Speed: " + std::to_string(int(gameTimeline.scale() * 100)) + "%";
        renderSimpleText(renderer, speedText, DESIGN_WIDTH - 200, 20, 0, 255, 0);

        if (gameTimeline.isPaused()) {
            renderSimpleText(renderer, "PAUSED", DESIGN_WIDTH - 200, 40, 255, 0, 0);
        }

        int remotePlayers = 0;
        {
            std::lock_guard<std::mutex> lock(remotePlayersMutex);
            remotePlayers = remotePlayersRegistry.size();
        }
        std::string playerCountText = "Remote Players: " + std::to_string(remotePlayers);
        renderSimpleText(renderer, playerCountText, DESIGN_WIDTH - 250, 60, 255, 255, 0);

        if (statusMessageTimer > 0) {
            renderSimpleText(renderer, statusMessage, DESIGN_WIDTH / 2 - 100, 100, 255, 255, 0);
            statusMessageTimer--;
        }

        if (gameWon) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
            SDL_FRect overlay = {0, 0, (float)DESIGN_WIDTH, (float)DESIGN_HEIGHT};
            SDL_RenderFillRect(renderer, &overlay);

            SDL_SetRenderDrawColor(renderer, 255, 215, 0, 255);
            SDL_FRect winBox = {DESIGN_WIDTH / 2 - 300, DESIGN_HEIGHT / 2 - 80, 600, 160};
            SDL_RenderFillRect(renderer, &winBox);

            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderDebugText(renderer, DESIGN_WIDTH / 2 - 80, DESIGN_HEIGHT / 2 - 40, "YOU WIN!");
            SDL_RenderDebugText(renderer, DESIGN_WIDTH / 2 - 120, DESIGN_HEIGHT / 2 + 20, "Press R to Restart");
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    // Cleanup
    running = false;
    networkThread.join();
    
    {
        std::lock_guard<std::mutex> lock(remotePlayersMutex);
        for (auto& [id, entity] : remoteEntities) {
            if (entity.entity) delete entity.entity;
        }
        remotePlayersRegistry.clear();
    }
    
    zmq_close(sock); 
    zmq_ctx_destroy(ctx);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    
    SDL_Log("[Part 1B] Client shut down cleanly");
    return 0;
}