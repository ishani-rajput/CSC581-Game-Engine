#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <vector>
#include "entity.h"
#include "input.h"
#include "scaling.h"
#include "physics.h"
#include "collision.h"

// Original design resolution - everything is designed for this size
const int DESIGN_WIDTH = 1920;
const int DESIGN_HEIGHT = 1080;

const float PLAYER_SPEED = 300.f;
const float JUMP_VELOCITY = -1000.f;

// Global scaling variables
float g_scaleX = 1.0f;
float g_scaleY = 1.0f;

// Simple function to update scale based on window size
void updateScale(SDL_Window* window) {
    int currentWidth, currentHeight;
    SDL_GetWindowSize(window, &currentWidth, &currentHeight);
    
    g_scaleX = (float)currentWidth / (float)DESIGN_WIDTH;
    g_scaleY = (float)currentHeight / (float)DESIGN_HEIGHT;
}

// Helper to scale any rectangle from design size to current window size
SDL_FRect scaleRect(float x, float y, float w, float h) {
    return { x * g_scaleX, y * g_scaleY, w * g_scaleX, h * g_scaleY };
}

bool checkCollision(const SDL_FRect& a, const SDL_FRect& b) {
    return aabbIntersect(a, b);
}

int main(int, char**) {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("Feeling Spikey - Simple Scaling", 
                                         DESIGN_WIDTH, DESIGN_HEIGHT, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);

    Physics::setGravity(2000.f);

    // === Load assets ===
    Entity groundBottom(renderer, "../assets/ground_bottom.png", 0, 0, 64, 64, 1, 0);
    Entity groundTop(renderer, "../assets/ground.png", 0, 0, 64, 64, 1, 0);
    Entity platform(renderer, "../assets/platform.png", 0, 0, 384, 128, 1, 0);
    Entity spike(renderer, "../assets/spikes.png", 0, 0, 256, 256, 1, 0);

    // Player setup - these are the DESIGN dimensions
    int frameCount = 8;
    int frameWidth = 128;
    int frameHeight = 128;
    float playerScale = 2.0f;
    float playerWidth = frameWidth * playerScale;   // 256 design pixels
    float playerHeight = frameHeight * playerScale; // 256 design pixels
    
    Entity player(renderer, "../assets/player.png", 0, 0, frameWidth, frameHeight, frameCount, 150);

    // === Game layout in DESIGN coordinates (1920x1080) ===
    float platformWidth = 400.f;
    float platformHeight = 500.f;
    float leftX = 0.f;
    float leftY = DESIGN_HEIGHT - platformHeight;
    float rightX = DESIGN_WIDTH - platformWidth;
    float rightY = DESIGN_HEIGHT - platformHeight;

    // Moving platform in design coordinates
    SDL_FRect movingPlatform = { 
        DESIGN_WIDTH / 2.f - 192, 
        DESIGN_HEIGHT - platformHeight - 200, 
        384, 
        128 
    };
    float platformSpeed = 200.f;
    int platformDir = 1;

    // Player spawn in design coordinates
    float playerX = leftX + 100.f;
    float playerY = leftY - playerHeight;
    Body playerBody = { 0.f, 0.f, true };
    
    float prevPlayerX = playerX;
    float prevPlayerY = playerY;

    // Initialize scale
    updateScale(window);

    Uint32 lastTicks = SDL_GetTicks();
    bool running = true;
    bool wasGrounded = false;
    bool isGrounded = false;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) 
                running = false;
            
            // IMPORTANT: Update scaling when window is resized by mouse
            if (e.type == SDL_EVENT_WINDOW_RESIZED) {
                updateScale(window);
                SDL_Log("Window resized! New scale: %.2fx, %.2fy", g_scaleX, g_scaleY);
            }
        }

        Input::poll();

        Uint32 now = SDL_GetTicks();
        float dt = (now - lastTicks) / 1000.f;
        lastTicks = now;

        prevPlayerX = playerX;
        prevPlayerY = playerY;

        // === Input - keep in design space ===
        playerBody.vx = 0.f;
        
        if (Input::isKeyPressed(SDL_SCANCODE_A)) {
            playerBody.vx = -PLAYER_SPEED;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_D)) {
            playerBody.vx = PLAYER_SPEED;
        }
        if ((Input::isKeyPressed(SDL_SCANCODE_W) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) && wasGrounded) {
            playerBody.vy = JUMP_VELOCITY;
        }

        // === Physics - keep in design space ===
        Physics::step(dt * 1000, playerX, playerY, playerBody);

        // === Collision detection - all in design coordinates ===
        SDL_FRect playerRect = { playerX, playerY, playerWidth, playerHeight };
        isGrounded = false;

        // Left platform collision
        SDL_FRect leftPlatformRect = { leftX, leftY, platformWidth, platformHeight };
        if (checkCollision(playerRect, leftPlatformRect)) {
            if (playerBody.vy >= 0 && prevPlayerY + playerHeight <= leftY + 10) {
                playerY = leftY - playerHeight;
                playerBody.vy = 0;
                isGrounded = true;
            }
        }

        // Right platform collision
        SDL_FRect rightPlatformRect = { rightX, rightY, platformWidth, platformHeight };
        if (checkCollision(playerRect, rightPlatformRect)) {
            if (playerBody.vy >= 0 && prevPlayerY + playerHeight <= rightY + 10) {
                playerY = rightY - playerHeight;
                playerBody.vy = 0;
                isGrounded = true;
            }
        }

        // Moving platform collision
        SDL_FRect movingPlatformRect = { movingPlatform.x, movingPlatform.y, movingPlatform.w, movingPlatform.h };
        if (checkCollision(playerRect, movingPlatformRect)) {
            if (playerBody.vy >= 0 && prevPlayerY + playerHeight <= movingPlatform.y + 10) {
                playerY = movingPlatform.y - playerHeight;
                playerBody.vy = 0;
                playerX += platformSpeed * platformDir * dt; // ride along
                isGrounded = true;
            }
        }

        // === Spike collision ===
        float spikeW = 300.f;
        float spikeH = 389.f;
        float spikeY = DESIGN_HEIGHT - spikeH;
        for (float x = platformWidth; x < DESIGN_WIDTH - platformWidth; x += spikeW) {
            SDL_FRect spikeRect = { x, spikeY, spikeW, spikeH };
            if (checkCollision(playerRect, spikeRect)) {
                // Reset player to starting point
                playerX = leftX + 100.f;
                playerY = leftY - playerHeight;
                playerBody.vx = 0.f;
                playerBody.vy = 0.f;
                SDL_Log("You died! Resetting...");
                break;
            }
        }

        wasGrounded = isGrounded;

        // === Move platform in design space ===
        movingPlatform.x += platformSpeed * platformDir * dt;
        if (movingPlatform.x < platformWidth) {
            movingPlatform.x = platformWidth;
            platformDir = 1;
        } else if (movingPlatform.x + movingPlatform.w > DESIGN_WIDTH - platformWidth) {
            movingPlatform.x = DESIGN_WIDTH - platformWidth - movingPlatform.w;
            platformDir = -1;
        }

        // === RENDERING - Scale everything to current window size ===
        SDL_SetRenderDrawColor(renderer, 135, 206, 235, 255);
        SDL_RenderClear(renderer);

        // Render spikes - convert from design to screen coordinates
        for (float x = platformWidth; x < DESIGN_WIDTH - platformWidth; x += spikeW) {
            SDL_FRect screenSpike = scaleRect(x, spikeY, spikeW, spikeH);
            spike.setPosition(screenSpike.x, screenSpike.y);
            spike.setSize(screenSpike.w, screenSpike.h);
            spike.render(renderer);
        }

        // Left platform bottom
        SDL_FRect screenLeftBottom = scaleRect(leftX, leftY, platformWidth, platformHeight);
        groundBottom.setPosition(screenLeftBottom.x, screenLeftBottom.y);
        groundBottom.setSize(screenLeftBottom.w, screenLeftBottom.h);
        groundBottom.render(renderer);

        // Left platform top
        SDL_FRect screenLeftTop = scaleRect(leftX, leftY - 64, platformWidth, 64);
        groundTop.setPosition(screenLeftTop.x, screenLeftTop.y);
        groundTop.setSize(screenLeftTop.w, screenLeftTop.h);
        groundTop.render(renderer);

        // Right platform bottom
        SDL_FRect screenRightBottom = scaleRect(rightX, rightY, platformWidth, platformHeight);
        groundBottom.setPosition(screenRightBottom.x, screenRightBottom.y);
        groundBottom.setSize(screenRightBottom.w, screenRightBottom.h);
        groundBottom.render(renderer);

        // Right platform top
        SDL_FRect screenRightTop = scaleRect(rightX, rightY - 64, platformWidth, 64);
        groundTop.setPosition(screenRightTop.x, screenRightTop.y);
        groundTop.setSize(screenRightTop.w, screenRightTop.h);
        groundTop.render(renderer);

        // Moving platform
        SDL_FRect screenMoving = scaleRect(movingPlatform.x, movingPlatform.y, movingPlatform.w, movingPlatform.h);
        platform.setPosition(screenMoving.x, screenMoving.y);
        platform.setSize(screenMoving.w, screenMoving.h);
        platform.render(renderer);

        // Player - scale from design coordinates to screen
        SDL_FRect screenPlayer = scaleRect(playerX, playerY, playerWidth, playerHeight);
        player.setPosition(screenPlayer.x, screenPlayer.y);
        player.setSize(screenPlayer.w, screenPlayer.h);
        player.update(); // Update animation frame
        player.render(renderer);

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}