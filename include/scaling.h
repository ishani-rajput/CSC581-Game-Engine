#pragma once
#include <SDL3/SDL.h>

enum class ScaleMode { Pixel, Proportional };

class Scaling {
public:
    static void setMode(ScaleMode m);
    static ScaleMode mode();

    // Compute destination rect given a "logical" rect
    // - Pixel: returns rect as-is
    // - Proportional: scales w,h as percentages of the window size
    static SDL_FRect compute(const SDL_FRect& logical, SDL_Window* win);

private:
    static ScaleMode s_mode;
};