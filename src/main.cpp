// src/main.cpp
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>

#include "input.h"
#include "scaling.h"

const int WINDOW_WIDTH  = 1920;
const int WINDOW_HEIGHT = 1080;

const int FRAME_COUNT   = 8;
const int FRAME_WIDTH   = 512;
const int FRAME_HEIGHT  = 512;
const int FRAME_DELAY_MS = 100; // animation speed

struct AppState {
    SDL_Texture* texture = nullptr;
    int currentFrame = 0;
    Uint32 lastFrameTick = 0;
};

static SDL_Texture* loadSprite(SDL_Renderer* r) {
    const char* pngs[] = {"../assets/skelly.png","assets/skelly.png"};
    for (auto p : pngs) {
        if (auto* t = IMG_LoadTexture(r, p)) return t;
    }
    const char* bmps[] = {
        "../assets/spritesheet.bmp","assets/spritesheet.bmp",
        "../assets/skelly.bmp","assets/skelly.bmp"
    };
    for (auto p : bmps) {
        if (SDL_Surface* s = SDL_LoadBMP(p)) {
            SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
            SDL_DestroySurface(s);
            if (t) return t;
        }
    }
    SDL_Log("Failed to load texture. Last error: %s", SDL_GetError());
    return nullptr;
}

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window*   window   = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Feeling Loopy", WINDOW_WIDTH, WINDOW_HEIGHT, 0, &window, &renderer)) {
        SDL_Log("CreateWindowAndRenderer failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    AppState st;
    st.texture = loadSprite(renderer);
    if (!st.texture) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    st.lastFrameTick = SDL_GetTicks();
    st.currentFrame  = 0;

    // Start in Pixel mode; press T to toggle
    Scaling::setMode(ScaleMode::Pixel);
    bool running = true;
    SDL_Event ev;

    // Rising-edge detection for 'T' (toggle)
    bool prevT = false;

    while (running) {
        // Only handle window-close via events
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
        }

        // Poll keyboard state via Input system
        Input::poll();

        // Toggle scaling mode on T key press (edge)
        const bool tNow = Input::isKeyPressed(SDL_SCANCODE_T);
        if (tNow && !prevT) {
            Scaling::setMode(Scaling::mode() == ScaleMode::Pixel
                             ? ScaleMode::Proportional
                             : ScaleMode::Pixel);
            SDL_Log("Scaling mode: %s",
                    (Scaling::mode() == ScaleMode::Pixel ? "Pixel" : "Proportional"));
        }
        prevT = tNow;

        // Animate
        const Uint32 now = SDL_GetTicks();
        if (now - st.lastFrameTick >= (Uint32)FRAME_DELAY_MS) {
            st.currentFrame = (st.currentFrame + 1) % FRAME_COUNT;
            st.lastFrameTick = now;
        }

        // Clear (blue)
        SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
        SDL_RenderClear(renderer);

        // Build source rect from the sprite sheet
        SDL_FRect src = {
            (float)(st.currentFrame * FRAME_WIDTH),
            0.0f,
            (float)FRAME_WIDTH,
            (float)FRAME_HEIGHT
        };

        // Build a "logical" destination:
        // - Pixel mode: draw 512x512 at (100,100) px
        // - Proportional: draw at 10% from top-left, size = 20% of window
        SDL_FRect logicalDst;
        if (Scaling::mode() == ScaleMode::Pixel) {
            logicalDst = {100.f, 100.f, (float)FRAME_WIDTH, (float)FRAME_HEIGHT};
        } else {
            logicalDst = {0.10f, 0.10f, 0.20f, 0.20f}; // fractions of window
        }

        // Convert via the scaling system
        SDL_FRect dst = Scaling::compute(logicalDst, window);

        // Render
        SDL_RenderTexture(renderer, st.texture, &src, &dst);
        SDL_RenderPresent(renderer);
    }

    if (st.texture) SDL_DestroyTexture(st.texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
