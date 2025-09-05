#include "scaling.h"
#include <SDL3/SDL.h>

ScaleMode Scaling::s_mode = ScaleMode::Pixel;

void Scaling::setMode(ScaleMode m) {
    s_mode = m;
}

ScaleMode Scaling::mode() {
    return s_mode;
}

SDL_FRect Scaling::compute(const SDL_FRect& logical, SDL_Window* win) {
    if (s_mode == ScaleMode::Pixel)
        return logical;

    int ww = 0, wh = 0;
    SDL_GetWindowSize(win, &ww, &wh);

    // Base (design) resolution
    constexpr float baseW = 1920.0f;
    constexpr float baseH = 1080.0f;

    float scaleX = ww / baseW;
    float scaleY = wh / baseH;

    SDL_FRect out;
    out.x = logical.x * scaleX;
    out.y = logical.y * scaleY;
    out.w = logical.w * scaleX;
    out.h = logical.h * scaleY;

    return out;
}