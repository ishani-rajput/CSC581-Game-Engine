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
    Entity(Entity&&) noexcept = default;
    Entity& operator=(Entity&&) noexcept = default;

    void update();                          
    void render(SDL_Renderer* renderer);     
    void render(SDL_Renderer* renderer, SDL_Window* win); 

    void setPosition(float nx, float ny);
    void setSize(float w, float h);

    void getPosition(float& outX, float& outY) const;
    SDL_FRect getRect() const;

private:
    float x, y;
    SDL_Texture* texture = nullptr;
    SDL_FRect dstRect;

    int frameCount;
    int frameWidth, frameHeight;
    int currentFrame = 0;
    Uint32 lastFrameTime = 0;
    int frameDelay = 0;
};
