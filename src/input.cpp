#include "input.h"

const bool* Input::s_state = nullptr;
int Input::s_len = 0;

void Input::poll() {
    s_state = SDL_GetKeyboardState(&s_len); // SDL3 returns const bool*
}

bool Input::isKeyPressed(SDL_Scancode sc) {
    return (s_state && sc < s_len) ? s_state[sc] : false;
}
