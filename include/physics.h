#pragma once
#include <SDL3/SDL.h>

struct Body {
    float vx = 0.f, vy = 0.f;   // velocity
    bool affectedByGravity = false;
};

class Physics {
public:
    static void setGravity(float g);              // pixels/s^2, downward
    static float gravity();                       // current gravity
    static void step(float dtMs, float& x, float& y, Body& body); // integrate
private:
    static float s_gravity;
};
