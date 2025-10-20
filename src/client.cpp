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
#include "object_model.h"
#include "registry.h"

const int DESIGN_WIDTH  = 1720;
const int DESIGN_HEIGHT = 1080;
const int WORLD_WIDTH = 3440;

const float PLAYER_SPEED   = 300.f;
const float JUMP_VELOCITY  = -750.f;

// REQUIREMENT 2: 5 unique platform combinations (position + size + texture)
const float leftWidth = 180.f;
const float leftHeight = 300.f;

const float middleWidth = 200.f;
const float middleHeight = 350.f;

const float platform3Width = 220.f;
const float platform3Height = 550.f;

const float bridgeWidth = 135.f;
const float bridgeHeight = 54.f;

const float finalWidth = 240.f;
const float finalHeight = 380.f;

// Platform positions in WORLD SPACE
const float leftX = 0.f;
const float leftY = DESIGN_HEIGHT - leftHeight - 70.f;
const float middleX = DESIGN_WIDTH / 2.f - middleWidth / 2.f - 200.f;
const float middleY = DESIGN_HEIGHT - middleHeight;
const float platform3X = DESIGN_WIDTH - platform3Width - 470.f;
const float platform3TopY = DESIGN_HEIGHT - platform3Height - 200.f;
const float platform3FullHeight = DESIGN_HEIGHT - platform3TopY;
const float bridge1X = platform3X + platform3Width + 70.f;
const float bridge1Y = platform3TopY + 100.f;
const float finalX = DESIGN_WIDTH - finalWidth;
const float finalY = DESIGN_HEIGHT - finalHeight;

// PART 1A: Remote player entities
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
    if (playerX + w > WORLD_WIDTH) playerX = WORLD_WIDTH - w;
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
    std::random_device rd; 
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    return "client_" + std::to_string(dis(gen));
}

int main(int, char**) {
    srand(time(nullptr));
    
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("Feeling Spikey",
                                          DESIGN_WIDTH, DESIGN_HEIGHT, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    Scaling::setMode(ScaleMode::Proportional);

    Physics::setGravity(2000.f);
    Timeline gameTimeline; 
    gameTimeline.anchorToRealTime();

    void* ctx = zmq_ctx_new();
    void* sock = zmq_socket(ctx, ZMQ_REQ);
    if (zmq_connect(sock, "tcp://localhost:5555") != 0) {
        SDL_Log("Failed to connect: %s", zmq_strerror(errno));
        return 1;
    }
    std::string clientId = generateClientId();
    SDL_Log("[Part 1B] Client ID: %s", clientId.c_str());

    Entity groundBottom(renderer, "../assets/ground_bottom.png", 0, 0, 64, 64, 1, 0);
    Entity groundTop(renderer, "../assets/ground.png", 0, 0, 64, 64, 1, 0);
    Entity platformTex(renderer, "../assets/platform.png", 0, 0, 384, 128, 1, 0);
    Entity spikeTex(renderer, "../assets/spikes.png", 0, 0, 256, 256, 1, 0);
    Entity flagTex(renderer, "../assets/flag.png", 0, 0, 256, 256, 1, 0);

    const int frameCount = 8, frameWidth = 128, frameHeight = 128;
    const float playerScale = 1.1f;
    const float playerW = frameWidth * playerScale;
    const float playerH = frameHeight * playerScale;

    Entity localPlayer(renderer, "../assets/player.png", 0, 0, frameWidth, frameHeight, frameCount, 150);

    // PART 1A: Game object registry for EVERYTHING
    Engine::Registry gameObjectRegistry;
    std::mutex gameObjectMutex;

    // REQUIREMENT 4: SPAWN POINTS (3 GameObjects)
    SDL_Log("[Req 4] Creating spawn points...");
    auto& spawn1 = gameObjectRegistry.upsert("spawn_left");
    spawn1.set<Engine::Vec2>("pos", {leftX + 80.f, leftY - playerH});
    spawn1.set<bool>("visible", false);

    auto& spawn2 = gameObjectRegistry.upsert("spawn_middle");
    spawn2.set<Engine::Vec2>("pos", {middleX + 80.f, middleY - playerH});
    spawn2.set<bool>("visible", false);

    auto& spawn3 = gameObjectRegistry.upsert("spawn_platform3");
    spawn3.set<Engine::Vec2>("pos", {platform3X + 80.f, platform3TopY - playerH});
    spawn3.set<bool>("visible", false);

    std::vector<std::string> spawnIds = {"spawn_left", "spawn_middle", "spawn_platform3"};
    SDL_Log("[Req 4] Created 3 spawn points");

    // Track current checkpoint spawn
    std::string currentSpawnId = "spawn_left";  // Start at first spawn

    // REQUIREMENT 5: DEATH ZONES (4 GameObjects)
    SDL_Log("[Req 5] Creating death zones...");
    auto& dz1 = gameObjectRegistry.upsert("death_zone_gap1");
    dz1.set<Engine::Vec2>("pos", {leftX + leftWidth, DESIGN_HEIGHT - 280});
    dz1.set<Engine::Vec2>("size", {middleX - (leftX + leftWidth), 280});
    dz1.set<bool>("visible", false);

    auto& dz2 = gameObjectRegistry.upsert("death_zone_gap2");
    dz2.set<Engine::Vec2>("pos", {middleX + middleWidth, DESIGN_HEIGHT - 280});
    dz2.set<Engine::Vec2>("size", {platform3X - (middleX + middleWidth), 280});
    dz2.set<bool>("visible", false);

    auto& dz3 = gameObjectRegistry.upsert("death_zone_gap3");
    dz3.set<Engine::Vec2>("pos", {platform3X + platform3Width, DESIGN_HEIGHT - 280});
    dz3.set<Engine::Vec2>("size", {finalX - (platform3X + platform3Width), 280});
    dz3.set<bool>("visible", false);

    auto& dz4 = gameObjectRegistry.upsert("death_zone_fall");
    dz4.set<Engine::Vec2>("pos", {0, DESIGN_HEIGHT});
    dz4.set<Engine::Vec2>("size", {WORLD_WIDTH, 200});
    dz4.set<bool>("visible", false);

    std::vector<std::string> deathZoneIds = {"death_zone_gap1", "death_zone_gap2", "death_zone_gap3", "death_zone_fall"};
    SDL_Log("[Req 5] Created 4 death zones");

    // REQUIREMENT 6: CAMERA SYSTEM
    SDL_Log("[Req 6] Camera system initialized");
    float cameraX = 0.f;

    // PART 1A: Remote players registry
    Engine::Registry remotePlayersRegistry;
    std::unordered_map<std::string, RemotePlayerEntity> remoteEntities;
    std::mutex remotePlayersMutex;

    // Track last message time for disconnect detection
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastHeardFrom;
    std::mutex lastHeardMutex;
    static constexpr int CLIENT_TIMEOUT_SECONDS = 3;  // 3 seconds without message = disconnect

    ServerState serverState;
    serverState.horizontalPlatform = {leftX + leftWidth + 20.f, DESIGN_HEIGHT - middleHeight - 150.f, 144.f, 72.f};
    serverState.horizontalDir = 1;
    serverState.verticalPlatform = {middleX + middleWidth + 40.f, middleY - 180.f, 144.f, 68.f};
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

    // PART 1B: Network thread
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

                    // Mark all remote players inactive
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
                                    std::lock_guard<std::mutex> lock(remotePlayersMutex);
                                    auto& remoteObj = remotePlayersRegistry.upsert(remoteId);
                                    remoteObj.set<Engine::Vec2>("pos", {rx_, ry_});
                                    remoteObj.set<bool>("active", true);
                                    
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
                                remotePlayersRegistry.erase(id);
                                auto it = remoteEntities.find(id);
                                if (it != remoteEntities.end() && it->second.entity) {
                                    delete it->second.entity;
                                    remoteEntities.erase(it);
                                }
                                
                                // Remove from lastHeard
                                {
                                    std::lock_guard<std::mutex> timeLock(lastHeardMutex);
                                    lastHeardFrom.erase(id);
                                }
                                
                                SDL_Log("[DISCONNECT] Remote player removed (inactive): %s", id.c_str());
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

    // Timeout-based disconnect detection thread
    std::thread timeoutThread([&]() {
        SDL_Log("[DISCONNECT MONITOR] Timeout detection thread started");
        
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));  // Check every 500ms
            
            auto now = std::chrono::steady_clock::now();
            std::vector<std::string> timedOutPlayers;
            
            // Find timed out players
            {
                std::lock_guard<std::mutex> timeLock(lastHeardMutex);
                for (const auto& [playerId, lastTime] : lastHeardFrom) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                    if (elapsed.count() >= CLIENT_TIMEOUT_SECONDS) {
                        timedOutPlayers.push_back(playerId);
                    }
                }
            }
            
            // Remove timed out players
            if (!timedOutPlayers.empty()) {
                std::lock_guard<std::mutex> lock(remotePlayersMutex);
                for (const auto& playerId : timedOutPlayers) {
                    // Remove from registry
                    remotePlayersRegistry.erase(playerId);
                    
                    // Delete entity
                    auto it = remoteEntities.find(playerId);
                    if (it != remoteEntities.end() && it->second.entity) {
                        delete it->second.entity;
                        remoteEntities.erase(it);
                    }
                    
                    // Remove from lastHeard
                    {
                        std::lock_guard<std::mutex> timeLock(lastHeardMutex);
                        lastHeardFrom.erase(playerId);
                    }
                    
                    SDL_Log("[DISCONNECT] Remote player timed out (>%ds): %s", CLIENT_TIMEOUT_SECONDS, playerId.c_str());
                }
            }
        }
        
        SDL_Log("[DISCONNECT MONITOR] Timeout detection thread stopped");
    });

    float prevHorizX = serverState.horizontalPlatform.x;
    float prevVertY = serverState.verticalPlatform.y;

    // Main loop
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
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
                        statusMessage = "Speed: 0.5x";
                        statusMessageTimer = 120;
                        break;
                    case SDL_SCANCODE_2:
                        gameTimeline.setScale(1.0);
                        statusMessage = "Speed: 1.0x";
                        statusMessageTimer = 120;
                        break;
                    case SDL_SCANCODE_3:
                        gameTimeline.setScale(2.0);
                        statusMessage = "Speed: 2.0x";
                        statusMessageTimer = 120;
                        break;
                    case SDL_SCANCODE_R:
                        if (gameWon) {
                            gameWon = false;
                            currentSpawnId = "spawn_left";  // Reset to first checkpoint
                            std::lock_guard<std::mutex> lock(localPlayerState.mutex);
                            localPlayerState.x = leftX + 80.f;
                            localPlayerState.y = leftY - playerH;
                            localPlayerState.vx = localPlayerState.vy = 0.f;
                            statusMessage = "Restarted";
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
        if (Input::isKeyPressed(SDL_SCANCODE_D)) playerBody.vx = PLAYER_SPEED;
        if ((Input::isKeyPressed(SDL_SCANCODE_W) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) && wasGrounded) {
            playerBody.vy = JUMP_VELOCITY;
        }

        Physics::step(dt * 1000, px, py, playerBody);
        clampPlayerPosition(px, py, playerW, playerH);

        SDL_FRect playerRect {px, py, playerW, playerH};
        bool grounded = false;

        SDL_FRect leftRect {leftX, leftY, leftWidth, leftHeight};
        SDL_FRect middleRect {middleX, middleY, middleWidth, middleHeight};
        SDL_FRect plat3Rect {platform3X, platform3TopY, platform3Width, platform3FullHeight};
        SDL_FRect bridge1Rect {bridge1X, bridge1Y, bridgeWidth, bridgeHeight};
        SDL_FRect finalRect {finalX, finalY, finalWidth, finalHeight};

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

        SDL_FRect mpRect {horizontalPlat.x, horizontalPlat.y, horizontalPlat.w, horizontalPlat.h};
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

        SDL_FRect vpRect {verticalPlat.x, verticalPlat.y, verticalPlat.w, verticalPlat.h};
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

        // Check if player crosses spawn points (checkpoints)
        playerRect = {px, py, playerW, playerH};
        {
            std::lock_guard<std::mutex> lock(gameObjectMutex);
            for (const auto& spawnId : spawnIds) {
                const auto* spawn = gameObjectRegistry.get(spawnId);
                if (!spawn) continue;
                
                auto spawnPos = spawn->get<Engine::Vec2>("pos", {0, 0});
                // Create checkpoint trigger area (wider than spawn point)
                SDL_FRect checkpointRect = {spawnPos.x - 50.f, spawnPos.y - 50.f, 100.f, 100.f};
                
                if (checkCollision(playerRect, checkpointRect)) {
                    if (currentSpawnId != spawnId) {
                        currentSpawnId = spawnId;
                        statusMessage = "Checkpoint: " + spawnId;
                        statusMessageTimer = 90;
                        SDL_Log("Checkpoint reached: %s", spawnId.c_str());
                    }
                }
            }
        }

        // Spike collision (explicit backup safety check) - respawn at current checkpoint
        for (float x = leftX + leftWidth; x < middleX; x += 220.f) {
            SDL_FRect spikeRect{ x, DESIGN_HEIGHT - 280.f, 220.f, 280.f };
            if (checkCollision(playerRect, spikeRect)) {
                const auto* spawn = gameObjectRegistry.get(currentSpawnId);
                if (spawn) {
                    auto spawnPos = spawn->get<Engine::Vec2>("pos", {leftX + 80.f, leftY - playerH});
                    px = spawnPos.x;
                    py = spawnPos.y;
                } else {
                    px = leftX + 80.f;
                    py = leftY - playerH;
                }
                playerBody.vx = playerBody.vy = 0;
                statusMessage = "OUCH! Respawned at " + currentSpawnId;
                statusMessageTimer = 120;
            }
        }
        for (float x = middleX + middleWidth; x < platform3X; x += 220.f) {
            SDL_FRect spikeRect{ x, DESIGN_HEIGHT - 280.f, 220.f, 280.f };
            if (checkCollision(playerRect, spikeRect)) {
                const auto* spawn = gameObjectRegistry.get(currentSpawnId);
                if (spawn) {
                    auto spawnPos = spawn->get<Engine::Vec2>("pos", {leftX + 80.f, leftY - playerH});
                    px = spawnPos.x;
                    py = spawnPos.y;
                } else {
                    px = leftX + 80.f;
                    py = leftY - playerH;
                }
                playerBody.vx = playerBody.vy = 0;
                statusMessage = "OUCH! Respawned at " + currentSpawnId;
                statusMessageTimer = 120;
            }
        }
        for (float x = platform3X + platform3Width; x < finalX; x += 220.f) {
            SDL_FRect spikeRect{ x, DESIGN_HEIGHT - 280.f, 220.f, 280.f };
            if (checkCollision(playerRect, spikeRect)) {
                const auto* spawn = gameObjectRegistry.get(currentSpawnId);
                if (spawn) {
                    auto spawnPos = spawn->get<Engine::Vec2>("pos", {leftX + 80.f, leftY - playerH});
                    px = spawnPos.x;
                    py = spawnPos.y;
                } else {
                    px = leftX + 80.f;
                    py = leftY - playerH;
                }
                playerBody.vx = playerBody.vy = 0;
                statusMessage = "OUCH! Respawned at " + currentSpawnId;
                statusMessageTimer = 120;
            }
        }

        // Fall detection (explicit backup safety check) - respawn at current checkpoint
        if (py > DESIGN_HEIGHT + 100) {
            const auto* spawn = gameObjectRegistry.get(currentSpawnId);
            if (spawn) {
                auto spawnPos = spawn->get<Engine::Vec2>("pos", {leftX + 80.f, leftY - playerH});
                px = spawnPos.x;
                py = spawnPos.y;
            } else {
                px = leftX + 80.f;
                py = leftY - playerH;
            }
            playerBody.vx = playerBody.vy = 0;
            statusMessage = "Fell! Respawned at " + currentSpawnId;
            statusMessageTimer = 120;
        }

        // REQUIREMENT 5: Check death zone collisions (GameObject-based) - respawn at current checkpoint
        {
            std::lock_guard<std::mutex> lock(gameObjectMutex);
            for (const auto& id : deathZoneIds) {
                const auto* zone = gameObjectRegistry.get(id);
                if (!zone) continue;
                
                auto zonePos = zone->get<Engine::Vec2>("pos", {0, 0});
                auto zoneSize = zone->get<Engine::Vec2>("size", {0, 0});
                SDL_FRect zoneRect = {zonePos.x, zonePos.y, zoneSize.x, zoneSize.y};
                
                if (checkCollision(playerRect, zoneRect)) {
                    // Respawn at current checkpoint
                    const auto* spawn = gameObjectRegistry.get(currentSpawnId);
                    if (spawn) {
                        auto spawnPos = spawn->get<Engine::Vec2>("pos", {leftX + 80.f, leftY - playerH});
                        px = spawnPos.x;
                        py = spawnPos.y;
                        playerBody.vx = 0.f;
                        playerBody.vy = 0.f;
                        statusMessage = "Death! Respawned at " + currentSpawnId;
                        statusMessageTimer = 120;
                    }
                    break;
                }
            }
        }

        // REQUIREMENT 6: Update camera (smooth interpolation)
        float targetCameraX = px - DESIGN_WIDTH / 2.f;
        if (targetCameraX < 0) targetCameraX = 0;
        if (targetCameraX > WORLD_WIDTH - DESIGN_WIDTH) targetCameraX = WORLD_WIDTH - DESIGN_WIDTH;
        cameraX += (targetCameraX - cameraX) * 0.1f;

        {
            std::lock_guard<std::mutex> lock(localPlayerState.mutex);
            localPlayerState.x = px;
            localPlayerState.y = py;
            localPlayerState.vx = playerBody.vx;
            localPlayerState.vy = playerBody.vy;
            localPlayerState.grounded = grounded;
        }

        localPlayer.update();

        // RENDERING WITH CAMERA OFFSET
        SDL_SetRenderDrawColor(renderer, 135, 206, 235, 255);
        SDL_RenderClear(renderer);

        float spikeW = 220.f, spikeH = 280.f, spikeY = DESIGN_HEIGHT - spikeH;
        
        for (float x = leftX + leftWidth; x < middleX; x += spikeW) {
            spikeTex.setPosition(x - cameraX, spikeY);
            spikeTex.setSize(spikeW, spikeH);
            spikeTex.render(renderer, window);
        }
        for (float x = middleX + middleWidth; x < platform3X; x += spikeW) {
            spikeTex.setPosition(x - cameraX, spikeY);
            spikeTex.setSize(spikeW, spikeH);
            spikeTex.render(renderer, window);
        }
        for (float x = platform3X + platform3Width; x < finalX; x += spikeW) {
            spikeTex.setPosition(x - cameraX, spikeY);
            spikeTex.setSize(spikeW, spikeH);
            spikeTex.render(renderer, window);
        }

        groundBottom.setPosition(leftX - cameraX, leftY);
        groundBottom.setSize(leftWidth, leftHeight);
        groundBottom.render(renderer, window);
        groundTop.setPosition(leftX - cameraX, leftY - 48.f);
        groundTop.setSize(leftWidth, 48.f);
        groundTop.render(renderer, window);

        groundBottom.setPosition(middleX - cameraX, middleY);
        groundBottom.setSize(middleWidth, middleHeight);
        groundBottom.render(renderer, window);
        groundTop.setPosition(middleX - cameraX, middleY - 48.f);
        groundTop.setSize(middleWidth, 48.f);
        groundTop.render(renderer, window);

        groundBottom.setPosition(platform3X - cameraX, platform3TopY);
        groundBottom.setSize(platform3Width, platform3FullHeight);
        groundBottom.render(renderer, window);
        groundTop.setPosition(platform3X - cameraX, platform3TopY - 48.f);
        groundTop.setSize(platform3Width, 48.f);
        groundTop.render(renderer, window);

        if (bridge1Vis) {
            platformTex.setPosition(bridge1X - cameraX, bridge1Y);
            platformTex.setSize(bridgeWidth, bridgeHeight);
            platformTex.render(renderer, window);
        }

        groundBottom.setPosition(finalX - cameraX, finalY);
        groundBottom.setSize(finalWidth, finalHeight);
        groundBottom.render(renderer, window);
        groundTop.setPosition(finalX - cameraX, finalY - 48.f);
        groundTop.setSize(finalWidth, 48.f);
        groundTop.render(renderer, window);

        float flagW = 150.f, flagH = 150.f;
        flagTex.setPosition(finalX - cameraX + finalWidth - flagW - 30.f, finalY - flagH - 48.f);
        flagTex.setSize(flagW, flagH);
        flagTex.render(renderer, window);

        platformTex.setPosition(horizontalPlat.x - cameraX, horizontalPlat.y);
        platformTex.setSize(horizontalPlat.w, horizontalPlat.h);
        platformTex.render(renderer, window);

        platformTex.setPosition(verticalPlat.x - cameraX, verticalPlat.y);
        platformTex.setSize(verticalPlat.w, verticalPlat.h);
        platformTex.render(renderer, window);

        localPlayer.setPosition(px - cameraX, py);
        localPlayer.setSize(playerW, playerH);
        localPlayer.render(renderer, window);

        // PART 1A: Render all remote players with camera offset
        {
            std::lock_guard<std::mutex> lock(remotePlayersMutex);
            for (const auto& remoteId : remotePlayersRegistry.getAllIds()) {
                const auto* remoteObj = remotePlayersRegistry.get(remoteId);
                if (remoteObj && remoteObj->get<bool>("active", false)) {
                    auto pos = remoteObj->get<Engine::Vec2>("pos", {0.f, 0.f});
                    auto it = remoteEntities.find(remoteId);
                    if (it != remoteEntities.end() && it->second.entity) {
                        it->second.entity->update();
                        it->second.entity->setPosition(pos.x - cameraX, pos.y);
                        it->second.entity->setSize(playerW, playerH);
                        it->second.entity->render(renderer, window);
                    }
                }
            }
        }

        // UI
        renderSimpleText(renderer, "A/D: Move  W/Space: Jump", 20, 20, 255, 255, 255);
        renderSimpleText(renderer, "P: Pause  1/2/3: Speed  R: Restart  S: Scaling", 20, 40, 255, 255, 255);
        renderSimpleText(renderer, "[Part 1A] GameObject Registry", 20, 60, 0, 255, 255);

        std::string cameraText = "Camera: " + std::to_string(int(cameraX));
        renderSimpleText(renderer, cameraText, DESIGN_WIDTH - 180, 20, 100, 200, 255);

        std::string speedText = "Speed: " + std::to_string(int(gameTimeline.scale() * 100)) + "%";
        renderSimpleText(renderer, speedText, DESIGN_WIDTH - 200, 40, 0, 255, 0);

        if (gameTimeline.isPaused()) {
            renderSimpleText(renderer, "PAUSED", DESIGN_WIDTH - 200, 60, 255, 0, 0);
        }

        int remotePlayers = 0;
        {
            std::lock_guard<std::mutex> lock(remotePlayersMutex);
            remotePlayers = remotePlayersRegistry.size();
        }
        std::string playerCountText = "Remote Players: " + std::to_string(remotePlayers);
        renderSimpleText(renderer, playerCountText, DESIGN_WIDTH - 250, 80, 255, 255, 0);

        if (statusMessageTimer > 0) {
            renderSimpleText(renderer, statusMessage, DESIGN_WIDTH / 2 - 150, 100, 255, 255, 0);
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
    timeoutThread.join();  // Wait for timeout detection thread
    
    {
        std::lock_guard<std::mutex> lock(remotePlayersMutex);
        for (auto& [id, entity] : remoteEntities) {
            if (entity.entity) delete entity.entity;
        }
        remotePlayersRegistry.clear();
    }
    
    lastHeardFrom.clear();
    
    gameObjectRegistry.clear();
    zmq_close(sock); 
    zmq_ctx_destroy(ctx);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    
    return 0;
}