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

    void update();                          // advances animation by time
    void render(SDL_Renderer* renderer);    // draws current frame

    // Public API
    void setPosition(float nx, float ny); // declared, defined in .cpp
    void setSize(float w, float h);

    // ✅ Getters for private members
    SDL_FRect getRect() const { return dstRect; }
    SDL_Texture* getTexture() const { return texture; }
    SDL_FRect getSrcRect() const;

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
