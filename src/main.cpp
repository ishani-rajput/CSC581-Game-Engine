#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <thread>
#include <mutex>
#include <vector>
#include "entity.h"
#include "input.h"
#include "scaling.h"
#include "physics.h"
#include "collision.h"
#include "timeline.h"  // Added Timeline

const int DESIGN_WIDTH = 1720;
const int DESIGN_HEIGHT = 1080;

const float PLAYER_SPEED = 300.f;
const float JUMP_VELOCITY = -1000.f;

bool checkCollision(const SDL_FRect& a, const SDL_FRect& b) {
    return aabbIntersect(a, b);
}

void clampPlayerPosition(float& playerX, float& playerY, float playerWidth, float playerHeight) {
    if (playerX < 0) playerX = 0;
    if (playerX + playerWidth > DESIGN_WIDTH) playerX = DESIGN_WIDTH - playerWidth;
    if (playerY > DESIGN_HEIGHT) {
        playerY = DESIGN_HEIGHT - playerHeight;
    }
}

// 🔹 Shared data for multithreading
std::mutex platformMutex;

int main(int, char**) {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("Feeling Spikey", 
                                         DESIGN_WIDTH, DESIGN_HEIGHT, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    Scaling::setMode(ScaleMode::Proportional);

    Physics::setGravity(2000.f);

    // Initialize Timeline System
    Timeline gameTimeline;
    gameTimeline.anchorToRealTime();
    
    // Optional: Create UI timeline that always runs at real time
    Timeline uiTimeline;
    uiTimeline.anchorToRealTime();

    Entity groundBottom(renderer, "../assets/ground_bottom.png", 0, 0, 64, 64, 1, 0);
    Entity groundTop(renderer, "../assets/ground.png", 0, 0, 64, 64, 1, 0);
    Entity platform(renderer, "../assets/platform.png", 0, 0, 384, 128, 1, 0);
    Entity spike(renderer, "../assets/spikes.png", 0, 0, 256, 256, 1, 0);

    int frameCount = 8;
    int frameWidth = 128;
    int frameHeight = 128;
    float playerScale = 2.0f;
    float playerWidth = frameWidth * playerScale;   // 256 design pixels
    float playerHeight = frameHeight * playerScale; // 256 design pixels
    
    Entity player(renderer, "../assets/player.png", 0, 0, frameWidth, frameHeight, frameCount, 150);

    float platformWidth = 400.f;
    float platformHeight = 500.f;
    float leftX = 0.f;
    float leftY = DESIGN_HEIGHT - platformHeight;
    float rightX = DESIGN_WIDTH - platformWidth;
    float rightY = DESIGN_HEIGHT - platformHeight;

    SDL_FRect movingPlatform = { 
        DESIGN_WIDTH / 2.f - 192, 
        DESIGN_HEIGHT - platformHeight - 200, 
        384, 
        128 
    };
    float platformSpeed = 200.f;
    int platformDir = 1;

    float playerX = leftX + 100.f;
    float playerY = leftY - playerHeight;
    Body playerBody = { 0.f, 0.f, true };
    
    float prevPlayerX = playerX;
    float prevPlayerY = playerY;

    bool running = true;
    bool wasGrounded = false;
    bool isGrounded = false;

    // Timeline-related variables for display
    bool showTimelineInfo = false;

    // 🔹 Start platform thread (uses same timeline for pause/speed)
    std::thread platformThread([&]() {
        while (running) {
            double dt = gameTimeline.tick();
            if (dt > 0.05f) dt = 0.05f;

            if (!gameTimeline.isPaused()) {
                std::lock_guard<std::mutex> lock(platformMutex);
                movingPlatform.x += platformSpeed * platformDir * dt;
                if (movingPlatform.x < platformWidth) {
                    movingPlatform.x = platformWidth;
                    platformDir = 1;
                } else if (movingPlatform.x + movingPlatform.w > DESIGN_WIDTH - platformWidth) {
                    movingPlatform.x = DESIGN_WIDTH - platformWidth - movingPlatform.w;
                    platformDir = -1;
                }
            }
            SDL_Delay(16); // ~60Hz update
        }
    });

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) 
                running = false;
            
            if (e.type == SDL_EVENT_WINDOW_RESIZED) {
                clampPlayerPosition(playerX, playerY, playerWidth, playerHeight);
                SDL_Log("Window resized!");
            }
            
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_S) {
                if (Scaling::mode() == ScaleMode::Pixel) {
                    Scaling::setMode(ScaleMode::Proportional);
                    SDL_Log("Switched to Proportional scaling mode");
                } else {
                    Scaling::setMode(ScaleMode::Pixel);
                    SDL_Log("Switched to Pixel scaling mode");
                }
            }

            // Timeline Controls
            if (e.type == SDL_EVENT_KEY_DOWN) {
                switch (e.key.scancode) {
                    case SDL_SCANCODE_P:
                        gameTimeline.togglePause();
                        SDL_Log(gameTimeline.isPaused() ? "Game PAUSED" : "Game RESUMED");
                        break;
                    case SDL_SCANCODE_1:
                        gameTimeline.setScale(0.5);
                        SDL_Log("Game speed: SLOW (0.5x)");
                        break;
                    case SDL_SCANCODE_2:
                        gameTimeline.setScale(1.0);
                        SDL_Log("Game speed: NORMAL (1.0x)");
                        break;
                    case SDL_SCANCODE_3:
                        gameTimeline.setScale(2.0);
                        SDL_Log("Game speed: FAST (2.0x)");
                        break;
                    case SDL_SCANCODE_T:
                        showTimelineInfo = !showTimelineInfo;
                        SDL_Log(showTimelineInfo ? "Timeline info: ON" : "Timeline info: OFF");
                        break;
                }
            }
        }

        Input::poll();

        // 🔹 Player physics update with timeline
        double gameDeltatime = gameTimeline.tick();
        float dt = static_cast<float>(gameDeltatime);
        if (dt > 0.05f) dt = 0.05f;

        if (!gameTimeline.isPaused()) {
            prevPlayerX = playerX;
            prevPlayerY = playerY;

            playerBody.vx = 0.f;
            if (Input::isKeyPressed(SDL_SCANCODE_A)) playerBody.vx = -PLAYER_SPEED;
            if (Input::isKeyPressed(SDL_SCANCODE_D)) playerBody.vx = PLAYER_SPEED;
            if ((Input::isKeyPressed(SDL_SCANCODE_W) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) && wasGrounded) {
                playerBody.vy = JUMP_VELOCITY;
            }

            Physics::step(dt * 1000, playerX, playerY, playerBody);
            clampPlayerPosition(playerX, playerY, playerWidth, playerHeight);

            SDL_FRect playerRect = { playerX, playerY, playerWidth, playerHeight };
            isGrounded = false;

            // Collisions with static platforms
            SDL_FRect leftPlatformRect = { leftX, leftY, platformWidth, platformHeight };
            SDL_FRect rightPlatformRect = { rightX, rightY, platformWidth, platformHeight };
            if (checkCollision(playerRect, leftPlatformRect) && playerBody.vy >= 0) {
                playerY = leftY - playerHeight; playerBody.vy = 0; isGrounded = true;
            }
            if (checkCollision(playerRect, rightPlatformRect) && playerBody.vy >= 0) {
                playerY = rightY - playerHeight; playerBody.vy = 0; isGrounded = true;
            }

            // Collision with moving platform (read protected)
            SDL_FRect mp;
            {
                std::lock_guard<std::mutex> lock(platformMutex);
                mp = movingPlatform;
            }
            if (checkCollision(playerRect, mp) && playerBody.vy >= 0 && prevPlayerY + playerHeight <= mp.y + 20) {
                playerY = mp.y - playerHeight;
                playerBody.vy = 0;
                playerX += platformSpeed * platformDir * dt; 
                isGrounded = true;
            }

            wasGrounded = isGrounded;
            player.update();
        }

        // Rendering
        SDL_SetRenderDrawColor(renderer, 135, 206, 235, 255);
        SDL_RenderClear(renderer);

        // Spikes
        float spikeW = 300.f, spikeH = 389.f, spikeY = DESIGN_HEIGHT - spikeH;
        for (float x = platformWidth; x < DESIGN_WIDTH - platformWidth; x += spikeW) {
            spike.setPosition(x, spikeY);
            spike.setSize(spikeW, spikeH);
            spike.render(renderer, window);
        }

        // Static platforms
        groundBottom.setPosition(leftX, leftY); groundBottom.setSize(platformWidth, platformHeight); groundBottom.render(renderer, window);
        groundTop.setPosition(leftX, leftY - 64); groundTop.setSize(platformWidth, 64); groundTop.render(renderer, window);
        groundBottom.setPosition(rightX, rightY); groundBottom.setSize(platformWidth, platformHeight); groundBottom.render(renderer, window);
        groundTop.setPosition(rightX, rightY - 64); groundTop.setSize(platformWidth, 64); groundTop.render(renderer, window);

        // Moving platform
        {
            std::lock_guard<std::mutex> lock(platformMutex);
            platform.setPosition(movingPlatform.x, movingPlatform.y);
            platform.setSize(movingPlatform.w, movingPlatform.h);
            platform.render(renderer, window);
        }

        // Player
        player.setPosition(playerX, playerY);
        player.setSize(playerWidth, playerHeight);
        player.render(renderer, window);

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    // Cleanup
    running = false;
    platformThread.join();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
