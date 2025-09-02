#pragma once
#include <SDL3/SDL.h>

class Entity {
protected:
    float x, y;
    SDL_Texture* texture;
    SDL_FRect dstRect;

public:
    Entity(float x, float y, SDL_Texture* texture);
    virtual ~Entity();

    virtual void update();
    virtual void draw(SDL_Renderer* renderer);
    void setPosition(float newX, float newY);
};
