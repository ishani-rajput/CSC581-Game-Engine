#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include "input.h"
#include "physics.h"
#include "collision.h"
#include "scaling.h"
#include "timeline.h"
#include "object_model.h"
#include "registry.h"

// MILESTONE 4: Event System
#include "event_manager.h"

#include <vector>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <mutex>

struct PipePair { 
    SDL_FRect top, bottom; 
};

struct GameState {
    SDL_FRect skully;
    Body skBody;
    std::vector<PipePair> pipes;
    int currentFrame;
    float cameraX = 0.f;
};

// Global states 
GameState renderState;
GameState logicState;

std::mutex stateMutex;
bool running = true;

Timeline gameTime;
std::mutex jumpMutex;
bool jumpRequested = false;

std::mutex moveMutex;
bool moveLeft = false;
bool moveRight = false;

struct ReplayFrame {
    double timestamp;
    GameState state;
    std::vector<SDL_FRect> platforms;
};

std::vector<ReplayFrame> gReplayFrames;
size_t gReplayPlaybackIndex = 0;
double gReplayPlaybackElapsed = 0.0;
double gReplayRecordingStartTime = 0.0;
bool gReplayHasSavedState = false;
GameState gReplaySavedState;
std::vector<SDL_FRect> gReplaySavedPlatforms;

// MILESTONE 4: Global Event Manager
static Engine::EventManager* gEventManager = nullptr;

static float floatRand(float a, float b) {
    return a + (b - a) * (float)rand() / (float)RAND_MAX;
}

static SDL_FRect applyCamera(const SDL_FRect& worldRect, float cameraX) {
    return {worldRect.x - cameraX, worldRect.y, worldRect.w, worldRect.h};
}

static SDL_Texture* tryLoadTexture(SDL_Renderer* r, const char* const* paths, int n) {
    for (int i = 0; i < n; ++i) {
        if (!paths[i]) continue;
        if (SDL_Texture* t = IMG_LoadTexture(r, paths[i])) return t;
    }
    SDL_Log("Failed to load texture: %s", SDL_GetError());
    return nullptr;
}

static void drawCircle(SDL_Renderer* renderer, int32_t centreX, int32_t centreY,
                       int32_t radius, Uint8 r, Uint8 g, Uint8 b) {
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy <= radius * radius) {
                SDL_RenderPoint(renderer, centreX + dx, centreY + dy);
            }
        }
    }
}

static Engine::Registry gRegistry;

void initializeSpawnPoints() {
    auto& spawn1 = gRegistry.upsert("spawn_point_1");
    spawn1.set<Engine::Vec2>("pos", {100.f, 840.f});
    spawn1.set<bool>("active", true);

    auto& spawn2 = gRegistry.upsert("spawn_point_2");
    spawn2.set<Engine::Vec2>("pos", {480.f, 840.f});
    spawn2.set<bool>("active", true);

    auto& spawn3 = gRegistry.upsert("spawn_point_3");
    spawn3.set<Engine::Vec2>("pos", {1400.f, 840.f});
    spawn3.set<bool>("active", true);

    auto& spawn4 = gRegistry.upsert("spawn_point_4");
    spawn4.set<Engine::Vec2>("pos", {870.f, 392.f}); 
    spawn4.set<bool>("active", true);

    auto& spawn5 = gRegistry.upsert("spawn_point_5");
    spawn5.set<Engine::Vec2>("pos", {1620.f, 542.f}); 
    spawn5.set<bool>("active", true);

    auto& spawn6 = gRegistry.upsert("spawn_point_6");
    spawn6.set<Engine::Vec2>("pos", {1100.f, 840.f});
    spawn6.set<bool>("active", true);
}

Engine::Vec2 getSpawnPointPosition(int spawnId) {
    if (spawnId < 1) spawnId = 1;
    if (spawnId > 6) spawnId = ((spawnId - 1) % 6) + 1;

    std::string spawnName = "spawn_point_" + std::to_string(spawnId);
    auto* spawn = gRegistry.get(spawnName);
    if (spawn) {
        return spawn->get<Engine::Vec2>("pos", {480.f, 840.f});
    }
    return {480.f, 840.f};
}

void respawnPlayer(GameState& state) {
    int randomSpawn = 1 + (rand() % 6);
    Engine::Vec2 spawnPos = getSpawnPointPosition(randomSpawn);

    state.skully.x = spawnPos.x;
    state.skully.y = spawnPos.y;

    state.skBody.vx = 0.f;
    state.skBody.vy = 0.f;

    SDL_Log("Player respawned at spawn point %d (%.1f, %.1f)", randomSpawn, spawnPos.x, spawnPos.y);

    // MILESTONE 4: Raise Spawn Event
    if (gEventManager) {
        Engine::Event spawnEvent = Engine::Events::Spawn(
            "player", spawnPos.x, spawnPos.y, &gameTime
        );
        gEventManager->raiseEvent(spawnEvent);
    }
}

void initializeDeathZones() {
    auto& dz = gRegistry.upsert("death_zone_left");
    dz.set<Engine::Vec2>("pos", {-200.f, 0.f});
    dz.set<Engine::Vec2>("size", {200.f, 1080.f});
    dz.set<bool>("active", true);
}

bool checkDeathZones(const SDL_FRect& player) {
    auto* dz = gRegistry.get("death_zone_left");
    if (dz && dz->get<bool>("active", true)) {
        Engine::Vec2 pos = dz->get<Engine::Vec2>("pos", {0.f, 0.f});
        Engine::Vec2 size = dz->get<Engine::Vec2>("size", {0.f, 0.f});
        SDL_FRect dzRect = {pos.x, pos.y, size.x, size.y};
        if (aabbIntersect(player, dzRect)) {
            SDL_Log("Player entered death zone (left boundary)");
            return true;
        }
    }

    return false;
}

// Logic Thread
void logicLoop(std::vector<SDL_FRect>& staticPlatforms) {
    const float JUMP_VELOCITY = -900.f;
    const float MOVE_SPEED = 400.f; 
    const float PIPE_SPEED = -450.f;
    const float PIPE_W = 140.f, PIPE_GAP = 280.f;
    const double PIPE_SPAWN_EVERY = 1.4;
    const double ANIM_FRAME_SEC = 0.100;

    const float MOVING_PLAT_SPEED = 120.f;
    float movingPlatDir = 1.f;  
    const float MOVING_PLAT_MIN_Y = 400.f, MOVING_PLAT_MAX_Y = 700.f;

    GameState local = logicState;
    double spawnTimer = 0.0;
    double animationAccum = 0.0;
    bool replayActiveLast = false;

    while (running) {
        double dtSec = gameTime.tick();
        if (dtSec <= 0.0) { SDL_Delay(1); continue; }

        bool replayActive = gEventManager && gEventManager->isReplaying();

        if (replayActive && !replayActiveLast) {
            gReplayPlaybackElapsed = 0.0;
            gReplayPlaybackIndex = 0;
            gReplayHasSavedState = true;
            gReplaySavedState = local;
            gReplaySavedPlatforms = staticPlatforms;
        }

        if (replayActive) {
            gReplayPlaybackElapsed += dtSec;

            if (gEventManager) {
                gEventManager->dispatchEvents();
            }

            while (gReplayPlaybackIndex < gReplayFrames.size() &&
                   gReplayFrames[gReplayPlaybackIndex].timestamp <= gReplayPlaybackElapsed) {
                const ReplayFrame& frame = gReplayFrames[gReplayPlaybackIndex];
                local = frame.state;
                staticPlatforms = frame.platforms;
                gReplayPlaybackIndex++;
            }

            {
                std::lock_guard<std::mutex> lock(stateMutex);
                renderState = local;
            }

            SDL_Delay(1);
            replayActiveLast = replayActive;
            continue;
        }

        if (!replayActive && replayActiveLast) {
            if (gReplayHasSavedState) {
                local = gReplaySavedState;
                staticPlatforms = gReplaySavedPlatforms;
                gReplayHasSavedState = false;
            }
        }

        {
            std::unique_lock<std::mutex> lock(jumpMutex);
            if (jumpRequested) {
                local.skBody.vy = JUMP_VELOCITY;
                jumpRequested = false;

                // MILESTONE 4: Raise Input Event for Jump
                if (gEventManager) {
                    Engine::Event jumpEvent = Engine::Events::Input("SPACE", true, &gameTime);
                    jumpEvent.payload["action"] = std::string("jump");
                    gEventManager->raiseEvent(jumpEvent);
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(moveMutex);
            if (moveLeft && !moveRight) local.skBody.vx = -MOVE_SPEED;
            else if (moveRight && !moveLeft) local.skBody.vx = MOVE_SPEED;
            else local.skBody.vx = 0.f;
        }

        Physics::step(static_cast<float>(dtSec * 1000.0), local.skully.x, local.skully.y, local.skBody);

        const float CAMERA_FOLLOW_THRESHOLD = 1920.f * 0.3f;
        const float CAMERA_BACK_THRESHOLD = 1920.f * 0.2f;
        
        if (local.skully.x - local.cameraX > CAMERA_FOLLOW_THRESHOLD) {
            local.cameraX = local.skully.x - CAMERA_FOLLOW_THRESHOLD;
        }
        if (local.skully.x - local.cameraX < CAMERA_BACK_THRESHOLD && local.cameraX > 0.f) {
            local.cameraX = local.skully.x - CAMERA_BACK_THRESHOLD;
            if (local.cameraX < 0.f) local.cameraX = 0.f;
        }

        staticPlatforms[3].y += MOVING_PLAT_SPEED * movingPlatDir * dtSec;
        if (staticPlatforms[3].y <= MOVING_PLAT_MIN_Y) {
            staticPlatforms[3].y = MOVING_PLAT_MIN_Y;
            movingPlatDir = 1.f;
        } else if (staticPlatforms[3].y >= MOVING_PLAT_MAX_Y) {
            staticPlatforms[3].y = MOVING_PLAT_MAX_Y;
            movingPlatDir = -1.f;
        }

        for (const auto& platform : staticPlatforms) {
            if (aabbIntersect(local.skully, platform) && local.skBody.vy > 0.f) {
                local.skully.y = platform.y - local.skully.h;
                local.skBody.vy = 0.f;
            }
        }
        if (local.skully.y < 0.f) { local.skully.y = 0.f; local.skBody.vy = 0.f; }

        if (checkDeathZones(local.skully)) {
            // MILESTONE 4: Raise Death Event
            if (gEventManager) {
                Engine::Event deathEvent = Engine::Events::Death("player", &gameTime);
                gEventManager->raiseEvent(deathEvent);
            }
            respawnPlayer(local);
        }

        spawnTimer += dtSec;
        while (spawnTimer >= PIPE_SPAWN_EVERY) {
            spawnTimer -= PIPE_SPAWN_EVERY;
            float center = floatRand(1080.f * 0.30f, 1080.f * 0.70f);
            float topH = center - PIPE_GAP * 0.5f;
            float bottomY = center + PIPE_GAP * 0.5f;
            float spawnX = local.cameraX + 1920.f + PIPE_W;
            local.pipes.push_back({
                SDL_FRect{spawnX, 0.f, PIPE_W, topH},
                SDL_FRect{spawnX, bottomY, PIPE_W, 1080.f - bottomY - 120.f}
            });
        }
        for (auto& p : local.pipes) {
            p.top.x += PIPE_SPEED * dtSec;
            p.bottom.x += PIPE_SPEED * dtSec;
        }
        local.pipes.erase(std::remove_if(local.pipes.begin(), local.pipes.end(),
            [](const PipePair& pp) { return (pp.top.x + pp.top.w) < -50.f; }), local.pipes.end());

        bool hit = false;
        for (auto& p : local.pipes) {
            if (aabbIntersect(local.skully, p.top) || aabbIntersect(local.skully, p.bottom)) { 
                // MILESTONE 4: Raise Collision Event
                if (gEventManager) {
                    Engine::Event collisionEvent = Engine::Events::Collision(
                        "player", "pipe", &gameTime
                    );
                    gEventManager->raiseEvent(collisionEvent);
                }
                
                // MILESTONE 4: Raise Death Event
                if (gEventManager) {
                    Engine::Event deathEvent = Engine::Events::Death("player", &gameTime);
                    gEventManager->raiseEvent(deathEvent);
                }
                
                hit = true; 
                break; 
            }
        }
        if (hit) { 
            local.pipes.clear(); 
            spawnTimer = 0.0;
            respawnPlayer(local);  
        }

        // Animation
        animationAccum += dtSec;
        while (animationAccum >= ANIM_FRAME_SEC) {
            animationAccum -= ANIM_FRAME_SEC;
            local.currentFrame = (local.currentFrame + 1) % 6;
        }

        if (gEventManager && gEventManager->isRecording()) {
            double relativeTime = gameTime.time() - gReplayRecordingStartTime;
            if (relativeTime < 0.0 || gReplayFrames.empty()) {
                relativeTime = 0.0;
            }
            gReplayFrames.push_back({relativeTime, local, staticPlatforms});
        }

        // MILESTONE 4: Process Events
        if (gEventManager) {
            gEventManager->dispatchEvents();
        }

        // Publish new state
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            renderState = local;
        }

        SDL_Delay(1);
        replayActiveLast = replayActive;
    }
}

// Rendering (Main Thread)
int main(int, char**) {
    initializeSpawnPoints();
    initializeDeathZones();

    srand(static_cast<unsigned int>(time(nullptr)));

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = nullptr; SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Skully Bird", 960,720, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_Log("CreateWindowAndRenderer failed: %s", SDL_GetError()); SDL_Quit(); return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    //TEXTURES
    const char* skullPaths[] = {"../assets/skullFace.png", "assets/skullFace.png"};
    SDL_Texture* skullTex = tryLoadTexture(renderer, skullPaths, 2);
    if (!skullTex) { SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit(); return 1; }

    const char* brickPaths[] = {"../assets/platform.png", "assets/platform.png"};
    SDL_Texture* brickTex = tryLoadTexture(renderer, brickPaths, 2);

    const char* skullyPlatformPaths[] = {"../assets/skullyPlatform.png", "assets/skullyPlatform.png"};
    SDL_Texture* skullyPlatformTex = tryLoadTexture(renderer, skullyPlatformPaths, 2);

    SDL_SetTextureScaleMode(skullTex, SDL_SCALEMODE_NEAREST);
    if (brickTex) SDL_SetTextureScaleMode(brickTex, SDL_SCALEMODE_NEAREST);
    if (skullyPlatformTex) SDL_SetTextureScaleMode(skullyPlatformTex, SDL_SCALEMODE_NEAREST);

    float texW = 0, texH = 0; SDL_GetTextureSize(skullTex, &texW, &texH);
    const int FRAME_COUNT = 6; const float FRAME_W = texW / FRAME_COUNT, FRAME_H = texH;

    std::vector<SDL_FRect> staticPlatforms = {
        {0.f, 1080.f - 120.f, 1920.f, 120.f},  
        {800.f, 500.f, 250.f, 50.f},           
        {1550.f, 650.f, 300.f, 40.f},          
        {1200.f, 550.f, 180.f, 35.f}           
    };

    Engine::Vec2 spawnPos = getSpawnPointPosition(2);
    logicState.skully = {spawnPos.x, spawnPos.y, 108.f, 108.f};
    logicState.skBody.affectedByGravity = true;
    Physics::setGravity(2400.f);
    logicState.currentFrame = 0;
    renderState = logicState;

    Scaling::setMode(ScaleMode::Pixel);
    gameTime.anchorToRealTime();
    gameTime.setScale(1.0);

    // MILESTONE 4: Initialize Event Manager
    Engine::EventManager eventManager(&gameTime);
    gEventManager = &eventManager;

    // MILESTONE 4: Register Event Listeners
    eventManager.registerListener(Engine::EventType::Collision, [&](const Engine::Event& ev) {
        auto it1 = ev.payload.find("A");
        auto it2 = ev.payload.find("B");
        if (it1 != ev.payload.end() && it2 != ev.payload.end()) {
            std::string objA = std::get<std::string>(it1->second);
            std::string objB = std::get<std::string>(it2->second);
            SDL_Log("[EVENT] Collision between %s and %s at time %.3f", 
                    objA.c_str(), objB.c_str(), ev.timestamp);
        }
    });

    eventManager.registerListener(Engine::EventType::Death, [&](const Engine::Event& ev) {
        auto it = ev.payload.find("entity");
        if (it != ev.payload.end()) {
            std::string entity = std::get<std::string>(it->second);
            SDL_Log("[EVENT] Death of %s at time %.3f", entity.c_str(), ev.timestamp);
        }
    });

    eventManager.registerListener(Engine::EventType::Spawn, [&](const Engine::Event& ev) {
        auto itEnt = ev.payload.find("entity");
        auto itX = ev.payload.find("x");
        auto itY = ev.payload.find("y");
        if (itEnt != ev.payload.end() && itX != ev.payload.end() && itY != ev.payload.end()) {
            std::string entity = std::get<std::string>(itEnt->second);
            float x = std::get<float>(itX->second);
            float y = std::get<float>(itY->second);
            SDL_Log("[EVENT] Spawn of %s at (%.1f, %.1f) at time %.3f", 
                    entity.c_str(), x, y, ev.timestamp);
        }
    });

    eventManager.registerListener(Engine::EventType::Input, [&](const Engine::Event& ev) {
        auto itKey = ev.payload.find("key");
        auto itPressed = ev.payload.find("pressed");
        if (itKey != ev.payload.end() && itPressed != ev.payload.end()) {
            std::string key = std::get<std::string>(itKey->second);
            bool pressed = std::get<bool>(itPressed->second);

            std::string actionSuffix;
            auto itAction = ev.payload.find("action");
            if (itAction != ev.payload.end() && std::holds_alternative<std::string>(itAction->second)) {
                actionSuffix = " (" + std::get<std::string>(itAction->second) + ")";
            }

            SDL_Log("[EVENT] Input %s%s %s at time %.3f", 
                    key.c_str(), actionSuffix.c_str(),
                    pressed ? "pressed" : "released", ev.timestamp);
        }
    });

    // REPLAY SYSTEM: Register Replay Listeners
    eventManager.registerListener(Engine::EventType::ReplayStart, [&](const Engine::Event&) {
        eventManager.startRecording();
        gReplayFrames.clear();
        gReplayPlaybackIndex = 0;
        gReplayPlaybackElapsed = 0.0;
        gReplayRecordingStartTime = gameTime.time();
        gReplayHasSavedState = false;
    });
    eventManager.registerListener(Engine::EventType::ReplayStop, [&](const Engine::Event&) {
        eventManager.stopRecording();
    });
    eventManager.registerListener(Engine::EventType::ReplayPlay, [&](const Engine::Event&) {
        if (gReplayFrames.empty()) {
            SDL_Log("[REPLAY] No recorded frames to play back");
            return;
        }
        gReplayPlaybackIndex = 0;
        gReplayPlaybackElapsed = 0.0;
        eventManager.playReplay();
    });

    // Raise initial spawn event
    Engine::Event initialSpawn = Engine::Events::Spawn("player", spawnPos.x, spawnPos.y, &gameTime);
    eventManager.raiseEvent(initialSpawn);

    std::thread logicThread(logicLoop, std::ref(staticPlatforms));

    bool prevSpace = false, prevToggle = false;
    bool prevP = false, prev1 = false, prev2 = false, prev3 = false;
    bool prevA = false, prevD = false;
    bool prevR = false, prevE = false, prevQ = false;

    SDL_Event ev;
    while (running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
        }
        Input::poll();
        if (Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) running = false;

        bool toggleNow = Input::isKeyPressed(SDL_SCANCODE_T);
        if (toggleNow && !prevToggle) {
            Scaling::setMode(Scaling::mode() == ScaleMode::Pixel ? ScaleMode::Proportional : ScaleMode::Pixel);
            SDL_Log("Scaling mode: %s", Scaling::mode() == ScaleMode::Pixel ? "Pixel" : "Proportional");

            // MILESTONE 4: Raise Input Event
            Engine::Event toggleEvent = Engine::Events::Input("T", true, &gameTime);
            toggleEvent.payload["action"] = std::string("toggle_scale");
            toggleEvent.payload["state"] = std::string(Scaling::mode() == ScaleMode::Pixel ? "Pixel" : "Proportional");
            eventManager.raiseEvent(toggleEvent);
        }
        prevToggle = toggleNow;

        bool pNow = Input::isKeyPressed(SDL_SCANCODE_P);
        if (pNow && !prevP) { 
            gameTime.togglePause();
            SDL_Log("Timeline %s", gameTime.isPaused() ? "PAUSED" : "UNPAUSED"); 

            // MILESTONE 4: Raise Input Event
            Engine::Event pauseEvent = Engine::Events::Input("P", true, &gameTime);
            pauseEvent.payload["action"] = std::string("pause_toggle");
            pauseEvent.payload["state"] = std::string(gameTime.isPaused() ? "paused" : "running");
            eventManager.raiseEvent(pauseEvent);
        }
        prevP = pNow;

        bool k1Now = Input::isKeyPressed(SDL_SCANCODE_1);
        bool k2Now = Input::isKeyPressed(SDL_SCANCODE_2);
        bool k3Now = Input::isKeyPressed(SDL_SCANCODE_3);
        if (k1Now && !prev1) { 
            gameTime.setScale(0.5); 
            SDL_Log("Speed set: 0.5x"); 
            Engine::Event slowEvent = Engine::Events::Input("1", true, &gameTime);
            slowEvent.payload["action"] = std::string("time_scale");
            slowEvent.payload["value"] = 0.5f;
            eventManager.raiseEvent(slowEvent);
        }
        if (k2Now && !prev2) { 
            gameTime.setScale(1.0); 
            SDL_Log("Speed set: 1.0x"); 
            Engine::Event normalEvent = Engine::Events::Input("2", true, &gameTime);
            normalEvent.payload["action"] = std::string("time_scale");
            normalEvent.payload["value"] = 1.0f;
            eventManager.raiseEvent(normalEvent);
        }
        if (k3Now && !prev3) { 
            gameTime.setScale(2.0); 
            SDL_Log("Speed set: 2.0x"); 
            Engine::Event fastEvent = Engine::Events::Input("3", true, &gameTime);
            fastEvent.payload["action"] = std::string("time_scale");
            fastEvent.payload["value"] = 2.0f;
            eventManager.raiseEvent(fastEvent);
        }
        prev1 = k1Now; prev2 = k2Now; prev3 = k3Now;

        bool spaceNow = Input::isKeyPressed(SDL_SCANCODE_SPACE);
        if (spaceNow && !prevSpace) {
            std::lock_guard<std::mutex> lock(jumpMutex);
            jumpRequested = true;
        }
        prevSpace = spaceNow;

        bool aNow = Input::isKeyPressed(SDL_SCANCODE_A);
        bool dNow = Input::isKeyPressed(SDL_SCANCODE_D);
        {
            std::lock_guard<std::mutex> lock(moveMutex);
            moveLeft = aNow;
            moveRight = dNow;
        }
        if (aNow && !prevA) {
            Engine::Event leftEvent = Engine::Events::Input("A", true, &gameTime);
            leftEvent.payload["action"] = std::string("move_left");
            eventManager.raiseEvent(leftEvent);
        }
        if (dNow && !prevD) {
            Engine::Event rightEvent = Engine::Events::Input("D", true, &gameTime);
            rightEvent.payload["action"] = std::string("move_right");
            eventManager.raiseEvent(rightEvent);
        }
        prevA = aNow;
        prevD = dNow;

        // REPLAY SYSTEM: Record/Stop/Play using R/E/P
        bool rNow = Input::isKeyPressed(SDL_SCANCODE_R);
        bool eNow = Input::isKeyPressed(SDL_SCANCODE_E);
        bool qNow = Input::isKeyPressed(SDL_SCANCODE_Q); // Q for quick play as alternate
        if (rNow && !prevR)
            eventManager.raiseEvent(Engine::Events::ReplayStart(&gameTime));
        if (eNow && !prevE)
            eventManager.raiseEvent(Engine::Events::ReplayStop(&gameTime));
        if (qNow && !prevQ)
            eventManager.raiseEvent(Engine::Events::ReplayPlay(&gameTime));
        prevR = rNow; prevE = eNow; prevQ = qNow;

        //RENDER
        SDL_SetRenderDrawColor(renderer, 100, 150, 255, 255);
        SDL_RenderClear(renderer);

        int winW = 0, winH = 0;
        SDL_GetWindowSize(window, &winW, &winH);
        int circleX = winW - 30;
        int circleY = 30;
        int circleRad = 8;
        if (eventManager.isRecording()) {
            drawCircle(renderer, circleX, circleY, circleRad, 255, 0, 0);
        } else if (eventManager.isReplaying()) {
            drawCircle(renderer, circleX, circleY, circleRad, 0, 255, 0);
        }

        GameState snapshot;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            snapshot = renderState;
        }

        for (size_t i = 0; i < staticPlatforms.size(); ++i) {
            SDL_FRect platWorld = applyCamera(staticPlatforms[i], snapshot.cameraX);
            SDL_FRect plat = Scaling::compute(platWorld, window);
            if (brickTex && i == 0) {
                float tw = 0, th = 0; SDL_GetTextureSize(brickTex, &tw, &th);
                if (tw < 1) tw = 64; if (th < 1) th = 64;
                float scaleY = staticPlatforms[i].h / th;
                float tileW = tw * scaleY, tileH = th * scaleY;
                for (float x = -snapshot.cameraX; x < 1920.0f + snapshot.cameraX + tileW; x += tileW) {
                    SDL_FRect tilePx{ x, staticPlatforms[i].y, tileW, tileH };
                    SDL_FRect tileWorld = applyCamera(tilePx, snapshot.cameraX);
                    SDL_FRect tiledest = Scaling::compute(tileWorld, window);
                    SDL_RenderTexture(renderer, brickTex, nullptr, &tiledest);
                }
            } else if (i == 1 && skullyPlatformTex) {
                SDL_RenderTexture(renderer, skullyPlatformTex, nullptr, &plat);
            } else if (i == 1) {
                SDL_SetRenderDrawColor(renderer, 200, 100, 200, 255); 
                SDL_RenderFillRect(renderer, &plat);
            } else if (i == 2) {
                SDL_SetRenderDrawColor(renderer, 120, 120, 130, 255); 
                SDL_RenderFillRect(renderer, &plat);
            } else if (i == 3) {
                SDL_SetRenderDrawColor(renderer, 220, 50, 50, 255); 
                SDL_RenderFillRect(renderer, &plat);
            }
        }

        SDL_SetRenderDrawColor(renderer, 20, 120, 50, 255);
        for (auto& p : snapshot.pipes) {
            SDL_FRect tWorld = applyCamera(p.top, snapshot.cameraX);
            SDL_FRect bWorld = applyCamera(p.bottom, snapshot.cameraX);
            SDL_FRect t = Scaling::compute(tWorld, window), b = Scaling::compute(bWorld, window);
            SDL_RenderFillRect(renderer, &t); SDL_RenderFillRect(renderer, &b);
        }

        SDL_FRect src{ FRAME_W * snapshot.currentFrame, 0.f, FRAME_W, FRAME_H };
        SDL_FRect skullyWorld = applyCamera(snapshot.skully, snapshot.cameraX);
        SDL_FRect dest = Scaling::compute(skullyWorld, window);
        SDL_RenderTexture(renderer, skullTex, &src, &dest);

        SDL_RenderPresent(renderer);
    }

    logicThread.join();

    if (brickTex) SDL_DestroyTexture(brickTex);
    if (skullyPlatformTex) SDL_DestroyTexture(skullyPlatformTex);
    SDL_DestroyTexture(skullTex);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
