#pragma once
#include <SDL3/SDL.h>

struct Body {
    float vx = 0.f, vy = 0.f;
    bool affectedByGravity = false;
};

class Physics {
public:
    static void setGravity(float g);
    static float gravity();
    static void step(float dtMs, float& x, float& y, Body& body);
private:
    static float s_gravity;
};
