#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <vector>
#include <iostream>
#include <cstdlib>
#include <ctime>

#include "entity.h"
#include "input.h"
#include "scaling.h"
#include "physics.h"
#include "collision.h"

const int WINDOW_WIDTH = 1920;
const int WINDOW_HEIGHT = 1080;

// Assets
const char* PLAYER_ASSET   = "../assets/player.png";
const char* GHOST_ASSET    = "../assets/ghost.png";
const char* PLATFORM_ASSET = "../assets/platform.png";
const char* GRAVE_ASSET    = "../assets/grave.png";

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Dino Runner", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_Log("Window creation failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    Scaling::setMode(ScaleMode::Pixel);
    Physics::setGravity(2000.f);
    srand(static_cast<unsigned>(time(nullptr))); // RNG seed

    // === Entities ===
    // Entity player(renderer, PLAYER_ASSET, 100, 758, 256, 256, 1, 0);
    int winW, winH;
    SDL_GetWindowSize(window, &winW, &winH);

    float playerX = 100.f;
    float playerY = winH - 322.f; // 758 for 1080 height → scale proportionally
    float playerW = 256.f;
    float playerH = 256.f;

    Entity player(renderer, PLAYER_ASSET, playerX, playerY, playerW, playerH, 1, 0);

    Body playerBody = { 0.f, 0.f, true };

    Entity platform(renderer, PLATFORM_ASSET, 0, 950, 1920, 130, 1, 0);
    Entity grave(renderer, GRAVE_ASSET, 700, 700, 256, 256, 1, 0);
    Entity ghost(renderer, GHOST_ASSET, 1500, 600, 256, 256, 1, 0);
    Body ghostBody = { -200.f, 0.f, false };

    float ghostTimer = 0.0f;
    float ghostChangeInterval = 2.0f;

    SDL_Event ev;
    bool running = true;
    bool prevT = false;
    Uint32 lastTick = SDL_GetTicks();
    bool graveTouched = false;

    while (running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT)
                running = false;
        }

        Input::poll();

        // Toggle Scaling
        bool tNow = Input::isKeyPressed(SDL_SCANCODE_T);
        if (tNow && !prevT) {
            ScaleMode current = Scaling::mode();
            Scaling::setMode(current == ScaleMode::Pixel ? ScaleMode::Proportional : ScaleMode::Pixel);
            SDL_Log("Scaling mode changed to: %s",
                (Scaling::mode() == ScaleMode::Pixel ? "Pixel" : "Proportional"));
        }
        prevT = tNow;

        // Delta time
        Uint32 now = SDL_GetTicks();
        float delta = (now - lastTick) / 1000.0f;
        lastTick = now;

        // === Player Controls ===
        float speed = 400.f;
        if (Input::isKeyPressed(SDL_SCANCODE_A)) {
            playerBody.vx = -speed;
        } else if (Input::isKeyPressed(SDL_SCANCODE_D)) {
            playerBody.vx = speed;
        } else {
            playerBody.vx = 0;
        }

        // Flappy-style Jump
        if (Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
            playerBody.vy = -900.f;
        }

        // === Player Physics ===
        float px, py;
        player.getPosition(px, py);
        Physics::step(delta * 1000, px, py, playerBody);

        // Clamp player to screen boundaries
        if (py < 0) {
            py = 0;
            playerBody.vy = 0;
        }
        if (px < 0) {
            px = 0;
            playerBody.vx = 0;
        }
        if (px > WINDOW_WIDTH - player.getRect().w) {
            px = WINDOW_WIDTH - player.getRect().w;
            playerBody.vx = 0;
        }

        player.setPosition(px, py);
        player.update();

        // === Ghost Physics ===
        float gx, gy;
        ghost.getPosition(gx, gy);
        Physics::step(delta * 1000, gx, gy, ghostBody);

        if (gx < -150) {
            gx = WINDOW_WIDTH;
            gy = static_cast<float>(rand() % static_cast<int>(WINDOW_HEIGHT - ghost.getRect().h));
        }

        if (gy < 0) gy = 0;
        if (gy > WINDOW_HEIGHT - ghost.getRect().h)
            gy = WINDOW_HEIGHT - ghost.getRect().h;

        ghost.setPosition(gx, gy);

        ghostTimer += delta;
        if (ghostTimer >= ghostChangeInterval) {
            ghostTimer = 0.0f;
            ghostBody.vy = static_cast<float>((rand() % 301) - 150);
        }

        ghost.update();

        // === Collisions ===

        // Player + Platform
        if (aabbIntersect(player.getRect(), platform.getRect())) {
            playerBody.vy = 0;
            float newY = platform.getRect().y - player.getRect().h;
            player.setPosition(px, newY);
        }

        // Player + Ghost
        if (aabbIntersect(player.getRect(), ghost.getRect())) {
            std::cout << "👻 You hit the ghost! Resetting...\n";
            player.setPosition(100, 758);
            playerBody.vx = 0;
            playerBody.vy = 0;
        }

        // Player + Grave
        if (aabbIntersect(player.getRect(), grave.getRect())) {
            if (!graveTouched) {
                std::cout << "🪦 You touched the grave! Resetting...\n";
                graveTouched = true;
            }
            player.setPosition(100, 758);
            playerBody.vx = 0;
            playerBody.vy = 0;
        } else {
            graveTouched = false;
        }

        // === Rendering ===
        SDL_SetRenderDrawColor(renderer, 50, 100, 255, 255);
        SDL_RenderClear(renderer);

        platform.render(renderer, window);
        grave.render(renderer, window);
        ghost.render(renderer, window);
        player.render(renderer, window);

        SDL_RenderPresent(renderer);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
