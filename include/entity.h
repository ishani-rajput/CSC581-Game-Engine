#pragma once
#include <SDL3/SDL.h>

class Entity {
public:
    Entity(SDL_Renderer* renderer, const char* path,
           float x, float y,
           int frameWidth, int frameHeight,
           int frameCount, int frameDelayMs);
    ~Entity();

    // no copying (texture ownership)
    Entity(const Entity&) = delete;
    Entity& operator=(const Entity&) = delete;
    // allow moving if you like (optional)
    Entity(Entity&&) noexcept = default;
    Entity& operator=(Entity&&) noexcept = default;

    void update();                          // advances animation by time
    void render(SDL_Renderer* renderer);    // draws current frame

    // convenience
    void setPosition(float nx, float ny);
    void setSize(float w, float h);         // scale on screen

private:
    float x, y;
    SDL_Texture* texture = nullptr;
    SDL_FRect dstRect;

    int frameCount;
    int frameWidth, frameHeight;
    int currentFrame = 0;
    Uint32 lastFrameTime = 0; // ms
    int frameDelay = 0;       // ms per frame
};
