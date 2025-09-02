#include "entity.h"

Entity::Entity(float x, float y, SDL_Texture* texture)
    : x(x), y(y), texture(texture)
{
    dstRect = { x, y, 64, 64 };  // default size
}

Entity::~Entity() {}

void Entity::update() {
    // default: no-op
}

void Entity::draw(SDL_Renderer* renderer) {
    dstRect.x = x;
    dstRect.y = y;
    SDL_RenderTexture(renderer, texture, nullptr, &dstRect);
}

void Entity::setPosition(float newX, float newY) {
    x = newX;
    y = newY;
}