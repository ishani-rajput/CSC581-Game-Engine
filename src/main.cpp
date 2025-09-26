#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <ctime>

#include "entity.h"
#include "input.h"
#include "scaling.h"
#include "physics.h"
#include "collision.h"
#include "timeline.h"

const int WINDOW_WIDTH = 1920;
const int WINDOW_HEIGHT = 1080;

// Assets
const char* PLAYER_ASSET   = "../assets/player.png";
const char* GHOST_ASSET    = "../assets/ghost.png";
const char* PLATFORM_ASSET = "../assets/platform.png";
const char* GRAVE_ASSET    = "../assets/grave.png";
const char* BG_SKY_ASSET   = "../assets/background_sky.png";

struct Object2D { float x{0}, y{0}, w{256}, h{256}; };

struct Snapshot {
    Object2D player, ghost, platform, grave;
    bool graveTouched{false};
};

class GameState {
public:
    GameState() {
        // Init positions
        player_ = {100.f, WINDOW_HEIGHT - 322.f, 256.f, 256.f};
        playerBody_ = {0.f, 0.f, true};

        ghost_ = {1500.f, 600.f, 256.f, 256.f};
        ghostBody_ = {-200.f, 0.f, false};

        platform_ = {0.f, 950.f, 1920.f, 130.f};
        grave_    = {700.f, 700.f, 256.f, 256.f};
    }

    void setInput(float vx, bool jump) {
        std::lock_guard<std::mutex> lk(m_);
        desiredVx_ = vx;
        if (jump) wantJump_ = true;
    }

    void getSnapshot(Snapshot& s) const {
        std::lock_guard<std::mutex> lk(m_);
        s.player = player_;
        s.ghost = ghost_;
        s.platform = platform_;
        s.grave = grave_;
        s.graveTouched = graveTouched_;
    }

    void stepPlayer(float dt) {
        std::lock_guard<std::mutex> lk(m_);

        playerBody_.vx = desiredVx_;
        if (wantJump_ && onGround_) {
            playerBody_.vy = -900.f;
            onGround_ = false;
        }
        wantJump_ = false;

        float px = player_.x, py = player_.y;
        Physics::step(dt * 1000, px, py, playerBody_);
        player_.x = px; player_.y = py;

        if (player_.y < 0) { player_.y = 0; playerBody_.vy = 0; }
        if (player_.x < 0) { player_.x = 0; playerBody_.vx = 0; }
        if (player_.x > WINDOW_WIDTH - player_.w) {
            player_.x = WINDOW_WIDTH - player_.w; playerBody_.vx = 0;
        }

        if (aabbIntersect(toRect(player_), toRect(platform_))) {
            playerBody_.vy = 0;
            player_.y = platform_.y - player_.h;
            onGround_ = true;
        } else onGround_ = false;

        if (aabbIntersect(toRect(player_), toRect(grave_))) {
            graveTouched_ = true;
            resetPlayer_nolock();
        } else graveTouched_ = false;

        if (aabbIntersect(toRect(player_), toRect(ghost_))) {
            resetPlayer_nolock();
        }
    }

    void stepGhost(float dt) {
        std::lock_guard<std::mutex> lk(m_);

        float gx = ghost_.x, gy = ghost_.y;
        Physics::step(dt * 1000, gx, gy, ghostBody_);
        ghost_.x = gx; ghost_.y = gy;

        if (ghost_.x < -150) {
            ghost_.x = WINDOW_WIDTH;
            ghost_.y = (float)(rand() % (WINDOW_HEIGHT - (int)ghost_.h));
        }
        if (ghost_.y < 0) ghost_.y = 0;
        if (ghost_.y > WINDOW_HEIGHT - ghost_.h) ghost_.y = WINDOW_HEIGHT - ghost_.h;

        ghostTimer_ += dt;
        if (ghostTimer_ >= ghostInterval_) {
            ghostTimer_ = 0;
            ghostBody_.vy = (float)((rand() % 301) - 150);
        }
    }

private:
    void resetPlayer_nolock() {
        player_.x = 100.f; player_.y = WINDOW_HEIGHT - 322.f;
        playerBody_ = {0.f, 0.f, true};
        onGround_ = false;
    }

    static SDL_FRect toRect(const Object2D& o) {
        return SDL_FRect{o.x, o.y, o.w, o.h};
    }

    mutable std::mutex m_;
    Object2D player_, ghost_, platform_, grave_;
    Body playerBody_, ghostBody_;   // ✅ use Body from physics.h
    bool onGround_{false}, graveTouched_{false};
    float ghostTimer_{0}, ghostInterval_{2};
    float desiredVx_{0}; bool wantJump_{false};
};

// Worker threads
static void playerPhysicsLoop(std::atomic<bool>& running, GameState& gs) {
    using clk = std::chrono::steady_clock;
    auto prev = clk::now();
    while (running) {
        auto now = clk::now();
        float dt = std::chrono::duration<float>(now - prev).count();
        prev = now;
        gs.stepPlayer(dt);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

static void ghostAILoop(std::atomic<bool>& running, GameState& gs) {
    using clk = std::chrono::steady_clock;
    auto prev = clk::now();
    while (running) {
        auto now = clk::now();
        float dt = std::chrono::duration<float>(now - prev).count();
        prev = now;
        gs.stepGhost(dt);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Main
int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Witch Runner (Multithreaded)", WINDOW_WIDTH, WINDOW_HEIGHT,
                                     SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_Log("Window creation failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    Scaling::setMode(ScaleMode::Pixel);
    Physics::setGravity(2000.f);
    srand((unsigned)time(nullptr));

    SDL_Texture* bgSky = IMG_LoadTexture(renderer, BG_SKY_ASSET);

    Entity platformE(renderer, PLATFORM_ASSET, 0, 950, 1920, 130, 1, 0);
    Entity graveE(renderer, GRAVE_ASSET, 700, 700, 256, 256, 1, 0);
    Entity ghostE(renderer, GHOST_ASSET, 1500, 600, 256, 256, 1, 0);
    Entity playerE(renderer, PLAYER_ASSET, 100, WINDOW_HEIGHT-322.f, 256, 256, 1, 0);

    GameState gs;
    std::atomic<bool> running(true);

    std::thread tPlayer(playerPhysicsLoop, std::ref(running), std::ref(gs));
    std::thread tGhost(ghostAILoop,      std::ref(running), std::ref(gs));

    Timeline renderTime; renderTime.anchorToRealTime();
    bool prevT = false;

    SDL_Event ev;
    while (running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }

        Input::poll();

        // Scaling toggle
        bool tNow = Input::isKeyPressed(SDL_SCANCODE_T);
        if (tNow && !prevT) {
            ScaleMode current = Scaling::mode();
            Scaling::setMode(current == ScaleMode::Pixel ? ScaleMode::Proportional : ScaleMode::Pixel);
            SDL_Log("Scaling mode changed to: %s",
                    (Scaling::mode() == ScaleMode::Pixel ? "Pixel" : "Proportional"));
        }
        prevT = tNow;

        // Pause / speed scaling (local render only)
        if (Input::isKeyPressed(SDL_SCANCODE_P)) {
            renderTime.togglePause();
            SDL_Log("Render %s", renderTime.isPaused() ? "paused" : "resumed");
        }
        if (Input::isKeyPressed(SDL_SCANCODE_1)) { renderTime.setScale(0.5); SDL_Log("Render speed 0.5x"); }
        if (Input::isKeyPressed(SDL_SCANCODE_2)) { renderTime.setScale(1.0); SDL_Log("Render speed 1.0x"); }
        if (Input::isKeyPressed(SDL_SCANCODE_3)) { renderTime.setScale(2.0); SDL_Log("Render speed 2.0x"); }

        renderTime.tick();

        // Input -> GameState
        float vx = 0; float moveSpeed = 400.f;
        if (Input::isKeyPressed(SDL_SCANCODE_A)) vx = -moveSpeed;
        else if (Input::isKeyPressed(SDL_SCANCODE_D)) vx = moveSpeed;
        bool jump = Input::isKeyPressed(SDL_SCANCODE_SPACE);
        gs.setInput(vx, jump);

        // Snapshot for rendering
        Snapshot snap;
        gs.getSnapshot(snap);

        // Render
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        if (bgSky) SDL_RenderTexture(renderer, bgSky, nullptr, nullptr);

        platformE.render(renderer, window);
        graveE.render(renderer, window);

        playerE.setPosition(snap.player.x, snap.player.y);
        ghostE.setPosition(snap.ghost.x, snap.ghost.y);

        playerE.update(); playerE.render(renderer, window);
        ghostE.update();  ghostE.render(renderer, window);

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    tPlayer.join();
    tGhost.join();

    if (bgSky) SDL_DestroyTexture(bgSky);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
