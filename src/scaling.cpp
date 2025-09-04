#include "scaling.h"

ScaleMode Scaling::s_mode = ScaleMode::Pixel;

void Scaling::setMode(ScaleMode m) { s_mode = m; }
ScaleMode Scaling::mode() { return s_mode; }

SDL_FRect Scaling::compute(const SDL_FRect& logical, SDL_Window* win) {
    if (s_mode == ScaleMode::Pixel) return logical;

    int ww=0, wh=0;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_FRect out = logical;
    // interpret w/h as 0..1 fractions of window size in proportional mode
    out.w = logical.w * ww;
    out.h = logical.h * wh;
    // position also as 0..1? choice: here we keep x,y as pixels; or switch to fractions too.
    // If you want full proportional layout, treat x,y as fractions as well:
    // out.x = logical.x * ww; out.y = logical.y * wh;
    return out;
}
