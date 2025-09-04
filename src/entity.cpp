#include "entity.h"
#include "scaling.h"
#include <SDL3_image/SDL_image.h>
#include <iostream>

Entity::Entity(SDL_Renderer* renderer, const char* path,
               float x, float y,
               int frameWidth, int frameHeight,
               int frameCount, int frameDelayMs)
    : x(x), y(y), frameWidth(frameWidth), frameHeight(frameHeight),
      frameCount(frameCount), frameDelay(frameDelayMs) {

    SDL_Surface* surface = IMG_Load(path);
    if (!surface) {
        std::cerr << "Failed to load image: " << path << " Error: " << SDL_GetError() << "\n";
        return;
    }

    texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);

    if (!texture) {
        std::cerr << "Failed to create texture: " << SDL_GetError() << "\n";
    }

    dstRect = { x, y, static_cast<float>(frameWidth), static_cast<float>(frameHeight) };
}

Entity::~Entity() {
    if (texture) SDL_DestroyTexture(texture);
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

void Entity::getPosition(float& outX, float& outY) const {
    outX = dstRect.x;
    outY = dstRect.y;
}

SDL_FRect Entity::getRect() const {
    return dstRect;
}

void Entity::update() {
    if (frameCount <= 1) return;

    Uint32 now = SDL_GetTicks();
    if (now - lastFrameTime > static_cast<Uint32>(frameDelay)) {
        currentFrame = (currentFrame + 1) % frameCount;
        lastFrameTime = now;
    }
}

// This version uses scaling
void Entity::render(SDL_Renderer* renderer, SDL_Window* win) {
    SDL_FRect srcRect = {
        static_cast<float>(currentFrame * frameWidth),
        0.0f,
        static_cast<float>(frameWidth),
        static_cast<float>(frameHeight)
    };

    SDL_FRect scaledDst = Scaling::compute(dstRect, win);

    SDL_RenderTexture(renderer, texture, &srcRect, &scaledDst);
}

// Fallback version without scaling (not used anymore)
void Entity::render(SDL_Renderer* renderer) {
    SDL_FRect srcRect = {
        static_cast<float>(currentFrame * frameWidth),
        0.0f,
        static_cast<float>(frameWidth),
        static_cast<float>(frameHeight)
    };

    SDL_RenderTexture(renderer, texture, &srcRect, &dstRect);
}
