#include "entity.h"
#include <SDL3/SDL_log.h>
#include <SDL3_image/SDL_image.h>

Entity::Entity(SDL_Renderer* renderer, const char* path,
               float x, float y,
               int frameWidth, int frameHeight,
               int frameCount, int frameDelay)
    : x(x), y(y),
      frameCount(frameCount),
      frameWidth(frameWidth), frameHeight(frameHeight),
      currentFrame(0),
      lastFrameTime(SDL_GetTicks()),
      frameDelay(frameDelay)
{
    texture = IMG_LoadTexture(renderer, path);
    if (texture) {
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
}

    if (!texture) {
        SDL_Log("IMG_LoadTexture('%s') failed: %s", path, SDL_GetError());
    }

    dstRect = { x, y, (float)frameWidth, (float)frameHeight };
}

Entity::~Entity() {
    if (texture) {
        SDL_DestroyTexture(texture);
        texture = nullptr;
    }
}

void Entity::update() {
    Uint32 now = SDL_GetTicks();
    if (now - lastFrameTime >= (Uint32)frameDelay) {
        currentFrame = (currentFrame + 1) % frameCount;
        lastFrameTime = now;
    }
}

void Entity::render(SDL_Renderer* renderer) {
    if (!texture) return;

    SDL_FRect src = {
        (float)(currentFrame * frameWidth), 0.0f,
        (float)frameWidth, (float)frameHeight
    };

    SDL_RenderTexture(renderer, texture, &src, &dstRect);
}

void Entity::setPosition(float nx, float ny) {
    x = nx;
    y = ny;
    dstRect.x = x;
    dstRect.y = y;
}

void Entity::setSize(float w, float h) {
    dstRect.w = w;
    dstRect.h = h;
}

SDL_FRect Entity::getSrcRect() const {
    SDL_FRect src = {
        (float)(currentFrame * frameWidth), 0.0f,
        (float)frameWidth, (float)frameHeight
    };
    return src;
}
