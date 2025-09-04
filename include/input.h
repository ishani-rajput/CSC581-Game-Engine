#pragma once
#include <SDL3/SDL.h>

class Input {
public:
    static void poll(); // call once per frame (after SDL_PollEvent loop)
    static bool isKeyPressed(SDL_Scancode sc);

private:
    static const bool* s_state; // SDL3: bools, not Uint8s
    static int s_len;
};
