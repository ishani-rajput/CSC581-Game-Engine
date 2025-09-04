#include "entity.h"
#include <SDL3_image/SDL_image.h>

Entity::Entity(SDL_Renderer* renderer, const char* path,
               float x, float y,
               int frameWidth, int frameHeight,
               int frameCount, int frameDelay)
    : x(x), y(y), frameWidth(frameWidth), frameHeight(frameHeight),
      frameCount(frameCount), currentFrame(0),
      lastFrameTime(SDL_GetTicks()), frameDelay(frameDelay)
{
    texture = IMG_LoadTexture(renderer, path);
    dstRect = { x, y, (float)frameWidth, (float)frameHeight };
}

void Entity::update() {
    Uint32 now = SDL_GetTicks(); // ms since SDL init
    if (now - lastFrameTime >= (Uint32)frameDelay) {
        currentFrame = (currentFrame + 1) % frameCount;
        lastFrameTime = now;
    }
}

void Entity::render(SDL_Renderer* renderer) {
    SDL_FRect src = { (float)(currentFrame * frameWidth), 0,
                      (float)frameWidth, (float)frameHeight };
    SDL_RenderTexture(renderer, texture, &src, &dstRect);
}