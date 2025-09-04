#pragma once
#include <SDL3/SDL.h>

class Entity {
protected:
    float x, y;
    SDL_Texture* texture;
    SDL_FRect dstRect;

    int frameCount;
    int frameWidth, frameHeight;
    int currentFrame;
    Uint32 lastFrameTime;   // track when we last advanced
    int frameDelay;         // how long to wait before advancing

public:
    Entity(SDL_Renderer* renderer, const char* path,
           float x, float y,
           int frameWidth, int frameHeight,
           int frameCount, int frameDelay);

    void update();
    void render(SDL_Renderer* renderer);
};