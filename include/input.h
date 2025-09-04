#pragma once
#include <SDL3/SDL.h>

class Input {
public:
    static void poll(); // call once per frame (after SDL_PollEvent loop)
    static bool isKeyPressed(SDL_Scancode sc);

private:
    static const Uint8* s_state;
    static int s_len;
};
