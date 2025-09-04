#include "physics.h"

float Physics::s_gravity = 1200.f; // default ~gravity in px/s^2 (tune)

void Physics::setGravity(float g) { s_gravity = g; }
float Physics::gravity() { return s_gravity; }

void Physics::step(float dtMs, float& x, float& y, Body& body) {
    const float dt = dtMs * 0.001f; // ms -> seconds
    if (body.affectedByGravity) {
        body.vy += s_gravity * dt;
    }
    x += body.vx * dt;
    y += body.vy * dt;
}
