#pragma once
#include <SDL3/SDL.h>

class Input {
public:
    static void poll();
    static bool isKeyPressed(SDL_Scancode sc);

private:
    static const bool* s_state;
    static int s_len;
};
