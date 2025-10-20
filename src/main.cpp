#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include "input.h"
#include "physics.h"
#include "collision.h"
#include "scaling.h"
#include "timeline.h"
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <condition_variable>

struct PipePair { 
    SDL_FRect top, bottom; 
    bool scored = false; 
};

struct GameState {
    SDL_FRect skully;
    Body skBody;
    std::vector<PipePair> pipes;
    int currentFrame;
};

// Global states for render/logic threads
GameState renderState;
GameState logicState;

std::mutex stateMutex;
bool running = true;

Timeline gameTime;
std::mutex jumpMutex;
std::condition_variable jumpCV;
bool jumpRequested = false;

// Movement input
std::mutex moveMutex;
bool moveLeft = false;
bool moveRight = false;

// Random helper
static float floatRand(float a, float b) {
    return a + (b - a) * (float)rand() / (float)RAND_MAX;
}

// Texture loader
static SDL_Texture* tryLoadTexture(SDL_Renderer* r, const char* const* paths, int n) {
    for (int i = 0; i < n; ++i) {
        if (!paths[i]) continue;
        if (SDL_Texture* t = IMG_LoadTexture(r, paths[i])) return t;
    }
    SDL_Log("Failed to load texture: %s", SDL_GetError());
    return nullptr;
}

// ----------------------------------------------------------
// Logic Thread
// ----------------------------------------------------------
void logicLoop(std::vector<SDL_FRect>& staticPlatforms) {
    const float JUMP_VELOCITY = -900.f;
    const float MOVE_SPEED = 400.f; 
    const float PIPE_SPEED = -450.f;
    const float PIPE_W = 140.f, PIPE_GAP = 280.f;
    const double PIPE_SPAWN_EVERY = 1.4;
    const double ANIM_FRAME_SEC = 0.100;
    
    // Moving platform (Task 3)
    const float MOVING_PLAT_SPEED = 120.f;
    float movingPlatDir = 1.f;  // 1 = down, -1 = up
    const float MOVING_PLAT_MIN_Y = 400.f, MOVING_PLAT_MAX_Y = 700.f;

    GameState local = logicState;
    double spawnTimer = 0.0;
    double animationAccum = 0.0;

    while (running) {
        double dtSec = gameTime.tick();
        if (dtSec <= 0.0) { SDL_Delay(1); continue; }

        // Jump input
        {
            std::unique_lock<std::mutex> lock(jumpMutex);
            if (jumpRequested) {
                local.skBody.vy = JUMP_VELOCITY;
                jumpRequested = false;
            }
        }

        // Horizontal movement
        {
            std::lock_guard<std::mutex> lock(moveMutex);
            if (moveLeft && !moveRight) local.skBody.vx = -MOVE_SPEED;
            else if (moveRight && !moveLeft) local.skBody.vx = MOVE_SPEED;
            else local.skBody.vx = 0.f;
        }

        // Physics integration
        Physics::step(static_cast<float>(dtSec * 1000.0), local.skully.x, local.skully.y, local.skBody);

        // Screen edges
        if (local.skully.x < 0.f) { local.skully.x = 0.f; local.skBody.vx = 0.f; }
        if (local.skully.x + local.skully.w > 1920.f) { local.skully.x = 1920.f - local.skully.w; local.skBody.vx = 0.f; }
        
        // Moving platform - vertical movement (Task 3)
        staticPlatforms[3].y += MOVING_PLAT_SPEED * movingPlatDir * dtSec;
        if (staticPlatforms[3].y <= MOVING_PLAT_MIN_Y) {
            staticPlatforms[3].y = MOVING_PLAT_MIN_Y;
            movingPlatDir = 1.f;
        } else if (staticPlatforms[3].y >= MOVING_PLAT_MAX_Y) {
            staticPlatforms[3].y = MOVING_PLAT_MAX_Y;
            movingPlatDir = -1.f;
        }

        // Platform collisions
        for (const auto& platform : staticPlatforms) {
            if (aabbIntersect(local.skully, platform) && local.skBody.vy > 0.f) {
                local.skully.y = platform.y - local.skully.h;
                local.skBody.vy = 0.f;
            }
        }
        if (local.skully.y < 0.f) { local.skully.y = 0.f; local.skBody.vy = 0.f; }

        // Pipe spawning
        spawnTimer += dtSec;
        while (spawnTimer >= PIPE_SPAWN_EVERY) {
            spawnTimer -= PIPE_SPAWN_EVERY;
            float center = floatRand(1080.f * 0.30f, 1080.f * 0.70f);
            float topH = center - PIPE_GAP * 0.5f;
            float bottomY = center + PIPE_GAP * 0.5f;
            local.pipes.push_back({
                SDL_FRect{1920.f + PIPE_W, 0.f, PIPE_W, topH},
                SDL_FRect{1920.f + PIPE_W, bottomY, PIPE_W, 1080.f - bottomY - 120.f}
            });
        }
        for (auto& p : local.pipes) {
            p.top.x += PIPE_SPEED * dtSec;
            p.bottom.x += PIPE_SPEED * dtSec;
        }
        local.pipes.erase(std::remove_if(local.pipes.begin(), local.pipes.end(),
            [&](PipePair& pp) { return (pp.top.x + pp.top.w) < -50.f; }), local.pipes.end());

        // Pipe collisions
        bool hit = false;
        for (auto& p : local.pipes) {
            if (aabbIntersect(local.skully, p.top) || aabbIntersect(local.skully, p.bottom)) { hit = true; break; }
        }
        if (hit) { local.pipes.clear(); spawnTimer = 0.0; }

        // Animation
        animationAccum += dtSec;
        while (animationAccum >= ANIM_FRAME_SEC) {
            animationAccum -= ANIM_FRAME_SEC;
            local.currentFrame = (local.currentFrame + 1) % 6;
        }

        // Publish new state
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            renderState = local;
        }

        SDL_Delay(1);
    }
}

// ----------------------------------------------------------
// Rendering (Main Thread)
// ----------------------------------------------------------
int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = nullptr; SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Skully Bird - Static Platforms", 1500, 900, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_Log("CreateWindowAndRenderer failed: %s", SDL_GetError()); SDL_Quit(); return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    // --- TEXTURES ---
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

    // --- STATIC PLATFORMS (Task 2 & 3) ---
    std::vector<SDL_FRect> staticPlatforms = {
        {0.f, 1080.f - 120.f, 1920.f, 120.f},  // Ground (brick texture)
        {800.f, 500.f, 250.f, 50.f},           // Skully platform (with texture)
        {1550.f, 650.f, 300.f, 40.f},          // Grey platform (extreme right)
        {1200.f, 550.f, 180.f, 35.f}           // Red moving platform (Task 3 - vertical movement)
    };

    // --- PLAYER INIT ---
    logicState.skully = {480.f, 540.f, 108.f, 108.f};
    logicState.skBody.affectedByGravity = true;
    Physics::setGravity(2400.f);
    logicState.currentFrame = 0;
    renderState = logicState;

    Scaling::setMode(ScaleMode::Pixel);
    gameTime.anchorToRealTime();
    gameTime.setScale(1.0);

    std::thread logicThread(logicLoop, std::ref(staticPlatforms));

    bool prevSpace = false, prevToggle = false;
    bool prevP = false, prev1 = false, prev2 = false, prev3 = false;

    SDL_Event ev;
    while (running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
        }
        Input::poll();
        if (Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) running = false;

        // --- TOGGLES ---
        bool toggleNow = Input::isKeyPressed(SDL_SCANCODE_T);
        if (toggleNow && !prevToggle) {
            Scaling::setMode(Scaling::mode() == ScaleMode::Pixel ? ScaleMode::Proportional : ScaleMode::Pixel);
            SDL_Log("Scaling mode: %s", Scaling::mode() == ScaleMode::Pixel ? "Pixel" : "Proportional");
        }
        prevToggle = toggleNow;

        bool pNow = Input::isKeyPressed(SDL_SCANCODE_P);
        if (pNow && !prevP) { gameTime.togglePause(); SDL_Log("Timeline %s", gameTime.isPaused() ? "PAUSED" : "UNPAUSED"); }
        prevP = pNow;

        bool k1Now = Input::isKeyPressed(SDL_SCANCODE_1);
        bool k2Now = Input::isKeyPressed(SDL_SCANCODE_2);
        bool k3Now = Input::isKeyPressed(SDL_SCANCODE_3);
        if (k1Now && !prev1) { gameTime.setScale(0.5); SDL_Log("Speed set: 0.5x"); }
        if (k2Now && !prev2) { gameTime.setScale(1.0); SDL_Log("Speed set: 1.0x"); }
        if (k3Now && !prev3) { gameTime.setScale(2.0); SDL_Log("Speed set: 2.0x"); }
        prev1 = k1Now; prev2 = k2Now; prev3 = k3Now;

        bool spaceNow = Input::isKeyPressed(SDL_SCANCODE_SPACE);
        if (spaceNow && !prevSpace) {
            std::lock_guard<std::mutex> lock(jumpMutex);
            jumpRequested = true;
            jumpCV.notify_one();
        }
        prevSpace = spaceNow;

        // Continuous movement
        {
            std::lock_guard<std::mutex> lock(moveMutex);
            moveLeft = Input::isKeyPressed(SDL_SCANCODE_A);
            moveRight = Input::isKeyPressed(SDL_SCANCODE_D);
        }

        // --- RENDER ---
        SDL_SetRenderDrawColor(renderer, 100, 150, 255, 255);
        SDL_RenderClear(renderer);

        GameState snapshot;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            snapshot = renderState;
        }

        // --- STATIC PLATFORMS ---
        for (size_t i = 0; i < staticPlatforms.size(); ++i) {
            SDL_FRect plat = Scaling::compute(staticPlatforms[i], window);
            if (brickTex && i == 0) {
                // Ground - tiled brick texture
                float tw = 0, th = 0; SDL_GetTextureSize(brickTex, &tw, &th);
                if (tw < 1) tw = 64; if (th < 1) th = 64;
                float scaleY = staticPlatforms[i].h / th;
                float tileW = tw * scaleY, tileH = th * scaleY;
                for (float x = 0; x < 1920.0f + tileW; x += tileW) {
                    SDL_FRect tilePx{ x, staticPlatforms[i].y, tileW, tileH };
                    SDL_FRect tiledest = Scaling::compute(tilePx, window);
                    SDL_RenderTexture(renderer, brickTex, nullptr, &tiledest);
                }
            } else if (i == 1 && skullyPlatformTex) {
                SDL_RenderTexture(renderer, skullyPlatformTex, nullptr, &plat);
            } else if (i == 1) {
                SDL_SetRenderDrawColor(renderer, 200, 100, 200, 255); // Purple fallback
                SDL_RenderFillRect(renderer, &plat);
            } else if (i == 2) {
                SDL_SetRenderDrawColor(renderer, 120, 120, 130, 255); // Grey
                SDL_RenderFillRect(renderer, &plat);
            } else if (i == 3) {
                SDL_SetRenderDrawColor(renderer, 220, 50, 50, 255); // Red moving platform
                SDL_RenderFillRect(renderer, &plat);
            }
        }

        // --- PIPES ---
        SDL_SetRenderDrawColor(renderer, 20, 120, 50, 255);
        for (auto& p : snapshot.pipes) {
            SDL_FRect t = Scaling::compute(p.top, window), b = Scaling::compute(p.bottom, window);
            SDL_RenderFillRect(renderer, &t); SDL_RenderFillRect(renderer, &b);
        }

        // --- SKULLY ---
        SDL_FRect src{ FRAME_W * snapshot.currentFrame, 0.f, FRAME_W, FRAME_H };
        SDL_FRect dest = Scaling::compute(snapshot.skully, window);
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
