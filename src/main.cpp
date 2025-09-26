#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include "entity.h"
#include "input.h"
#include "scaling.h"
#include "physics.h"
#include "collision.h"
#include "timeline.h"

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

// Shared data structures with proper synchronization
struct PlayerData {
    float x, y;
    float vx, vy;
    bool grounded;
    std::mutex mutex;
};

struct EnvironmentData {
    SDL_FRect movingPlatform;
    int platformDir;
    std::mutex mutex;
};

int main(int, char**) {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("Feeling Spikey - Multithreaded", 
                                         DESIGN_WIDTH, DESIGN_HEIGHT, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    Scaling::setMode(ScaleMode::Proportional);

    Physics::setGravity(2000.f);

    // Separate timelines for different threads
    Timeline playerTimeline;
    playerTimeline.anchorToRealTime();
    
    Timeline envTimeline;
    envTimeline.anchorToRealTime();

    Entity groundBottom(renderer, "../assets/ground_bottom.png", 0, 0, 64, 64, 1, 0);
    Entity groundTop(renderer, "../assets/ground.png", 0, 0, 64, 64, 1, 0);
    Entity platform(renderer, "../assets/platform.png", 0, 0, 384, 128, 1, 0);
    Entity spike(renderer, "../assets/spikes.png", 0, 0, 256, 256, 1, 0);

    int frameCount = 8;
    int frameWidth = 128;
    int frameHeight = 128;
    float playerScale = 2.0f;
    float playerWidth = frameWidth * playerScale;
    float playerHeight = frameHeight * playerScale;
    
    Entity player(renderer, "../assets/player.png", 0, 0, frameWidth, frameHeight, frameCount, 150);

    float platformWidth = 400.f;
    float platformHeight = 500.f;
    float leftX = 0.f;
    float leftY = DESIGN_HEIGHT - platformHeight;
    float rightX = DESIGN_WIDTH - platformWidth;
    float rightY = DESIGN_HEIGHT - platformHeight;

    // Shared data structures
    PlayerData playerData;
    playerData.x = leftX + 100.f;
    playerData.y = leftY - playerHeight;
    playerData.vx = 0.f;
    playerData.vy = 0.f;
    playerData.grounded = false;

    EnvironmentData envData;
    envData.movingPlatform = { 
        DESIGN_WIDTH / 2.f - 192, 
        DESIGN_HEIGHT - platformHeight - 200, 
        384, 
        128 
    };
    envData.platformDir = 1;

    Body playerBody = { 0.f, 0.f, true };
    float platformSpeed = 200.f;

    std::atomic<bool> running{true};
    std::atomic<bool> showTimelineInfo{false};

    // Thread 1: Player Logic Thread
    std::thread playerThread([&]() {
        bool wasGrounded = false;
        
        while (running) {
            double dt = playerTimeline.tick();
            if (dt > 0.05f) dt = 0.05f;

            if (!playerTimeline.isPaused()) {
                float prevPlayerX, prevPlayerY;
                bool isGrounded = false;
                
                // Get current player position
                {
                    std::lock_guard<std::mutex> lock(playerData.mutex);
                    prevPlayerX = playerData.x;
                    prevPlayerY = playerData.y;
                    playerBody.vx = playerData.vx;
                    playerBody.vy = playerData.vy;
                }

                // Input handling
                playerBody.vx = 0.f;
                if (Input::isKeyPressed(SDL_SCANCODE_A)) playerBody.vx = -PLAYER_SPEED;
                if (Input::isKeyPressed(SDL_SCANCODE_D)) playerBody.vx = PLAYER_SPEED;
                if ((Input::isKeyPressed(SDL_SCANCODE_W) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) && wasGrounded) {
                    playerBody.vy = JUMP_VELOCITY;
                }

                // Physics simulation
                float newX = prevPlayerX, newY = prevPlayerY;
                Physics::step(dt * 1000, newX, newY, playerBody);
                clampPlayerPosition(newX, newY, playerWidth, playerHeight);

                SDL_FRect playerRect = { newX, newY, playerWidth, playerHeight };

                // Static platform collisions
                SDL_FRect leftPlatformRect = { leftX, leftY, platformWidth, platformHeight };
                SDL_FRect rightPlatformRect = { rightX, rightY, platformWidth, platformHeight };
                
                if (checkCollision(playerRect, leftPlatformRect)) {
                    if (playerBody.vy >= 0 && prevPlayerY + playerHeight <= leftY + 20) {
                        newY = leftY - playerHeight;
                        playerBody.vy = 0;
                        isGrounded = true;
                    } else if (playerBody.vx < 0 && prevPlayerX >= leftX + platformWidth - 10) {
                        newX = leftX + platformWidth;
                        playerBody.vx = 0;
                    } else if (playerBody.vx > 0 && prevPlayerX + playerWidth <= leftX + 10) {
                        newX = leftX - playerWidth;
                        playerBody.vx = 0;
                    }
                }
                
                if (checkCollision(playerRect, rightPlatformRect)) {
                    if (playerBody.vy >= 0 && prevPlayerY + playerHeight <= rightY + 20) {
                        newY = rightY - playerHeight;
                        playerBody.vy = 0;
                        isGrounded = true;
                    } else if (playerBody.vx < 0 && prevPlayerX >= rightX + platformWidth - 10) {
                        newX = rightX + platformWidth;
                        playerBody.vx = 0;
                    } else if (playerBody.vx > 0 && prevPlayerX + playerWidth <= rightX + 10) {
                        newX = rightX - playerWidth;
                        playerBody.vx = 0;
                    }
                }

                // Moving platform collision
                SDL_FRect movingPlatformRect;
                int platformDir;
                {
                    std::lock_guard<std::mutex> lock(envData.mutex);
                    movingPlatformRect = envData.movingPlatform;
                    platformDir = envData.platformDir;
                }
                
                if (checkCollision(playerRect, movingPlatformRect)) {
                    if (playerBody.vy >= 0 && prevPlayerY + playerHeight <= movingPlatformRect.y + 20) {
                        newY = movingPlatformRect.y - playerHeight;
                        playerBody.vy = 0;
                        newX += platformSpeed * platformDir * dt; // ride along
                        isGrounded = true;
                    }
                }

                // Spike collision
                float spikeW = 300.f;
                float spikeH = 389.f;
                float spikeY = DESIGN_HEIGHT - spikeH;
                for (float x = platformWidth; x < DESIGN_WIDTH - platformWidth; x += spikeW) {
                    SDL_FRect spikeRect = { x, spikeY, spikeW, spikeH };
                    if (checkCollision(playerRect, spikeRect)) {
                        newX = leftX + 100.f;
                        newY = leftY - playerHeight;
                        playerBody.vx = 0.f;
                        playerBody.vy = 0.f;
                        break;
                    }
                }

                // Fall off screen reset
                if (newY > DESIGN_HEIGHT + 100) {
                    newX = leftX + 100.f;
                    newY = leftY - playerHeight;
                    playerBody.vx = 0.f;
                    playerBody.vy = 0.f;
                }

                // Update shared player data
                {
                    std::lock_guard<std::mutex> lock(playerData.mutex);
                    playerData.x = newX;
                    playerData.y = newY;
                    playerData.vx = playerBody.vx;
                    playerData.vy = playerBody.vy;
                    playerData.grounded = isGrounded;
                }

                wasGrounded = isGrounded;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    });

    // Thread 2: Environment Thread
    std::thread envThread([&]() {
        while (running) {
            double dt = envTimeline.tick();
            if (dt > 0.05f) dt = 0.05f;

            if (!envTimeline.isPaused()) {
                std::lock_guard<std::mutex> lock(envData.mutex);
                
                // Update moving platform
                envData.movingPlatform.x += platformSpeed * envData.platformDir * dt;
                if (envData.movingPlatform.x < platformWidth) {
                    envData.movingPlatform.x = platformWidth;
                    envData.platformDir = 1;
                } else if (envData.movingPlatform.x + envData.movingPlatform.w > DESIGN_WIDTH - platformWidth) {
                    envData.movingPlatform.x = DESIGN_WIDTH - platformWidth - envData.movingPlatform.w;
                    envData.platformDir = -1;
                }
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    });

    // Main thread: Event handling and rendering
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) 
                running = false;
            
            if (e.type == SDL_EVENT_WINDOW_RESIZED) {
                std::lock_guard<std::mutex> lock(playerData.mutex);
                clampPlayerPosition(playerData.x, playerData.y, playerWidth, playerHeight);
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

            // Timeline Controls (affect both timelines for consistency in single-player)
            if (e.type == SDL_EVENT_KEY_DOWN) {
                switch (e.key.scancode) {
                    case SDL_SCANCODE_P:
                        playerTimeline.togglePause();
                        envTimeline.pause(playerTimeline.isPaused()); // Keep in sync
                        SDL_Log(playerTimeline.isPaused() ? "Game PAUSED" : "Game RESUMED");
                        break;
                    case SDL_SCANCODE_1:
                        playerTimeline.setScale(0.5);
                        envTimeline.setScale(0.5); // Keep in sync
                        SDL_Log("Game speed: SLOW (0.5x)");
                        break;
                    case SDL_SCANCODE_2:
                        playerTimeline.setScale(1.0);
                        envTimeline.setScale(1.0); // Keep in sync
                        SDL_Log("Game speed: NORMAL (1.0x)");
                        break;
                    case SDL_SCANCODE_3:
                        playerTimeline.setScale(2.0);
                        envTimeline.setScale(2.0); // Keep in sync
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

        // Get current game state for rendering
        float renderPlayerX, renderPlayerY;
        SDL_FRect renderMovingPlatform;
        
        {
            std::lock_guard<std::mutex> lock(playerData.mutex);
            renderPlayerX = playerData.x;
            renderPlayerY = playerData.y;
        }
        
        {
            std::lock_guard<std::mutex> lock(envData.mutex);
            renderMovingPlatform = envData.movingPlatform;
        }

        // Update player animation
        player.update();

        // Rendering
        SDL_SetRenderDrawColor(renderer, 135, 206, 235, 255);
        SDL_RenderClear(renderer);

        // Render spikes
        float spikeW = 300.f;
        float spikeH = 389.f;
        float spikeY = DESIGN_HEIGHT - spikeH;
        for (float x = platformWidth; x < DESIGN_WIDTH - platformWidth; x += spikeW) {
            spike.setPosition(x, spikeY);
            spike.setSize(spikeW, spikeH);
            spike.render(renderer, window);
        }

        // Render static platforms
        groundBottom.setPosition(leftX, leftY);
        groundBottom.setSize(platformWidth, platformHeight);
        groundBottom.render(renderer, window);

        groundTop.setPosition(leftX, leftY - 64);
        groundTop.setSize(platformWidth, 64);
        groundTop.render(renderer, window);

        groundBottom.setPosition(rightX, rightY);
        groundBottom.setSize(platformWidth, platformHeight);
        groundBottom.render(renderer, window);

        groundTop.setPosition(rightX, rightY - 64);
        groundTop.setSize(platformWidth, 64);
        groundTop.render(renderer, window);

        // Render moving platform
        platform.setPosition(renderMovingPlatform.x, renderMovingPlatform.y);
        platform.setSize(renderMovingPlatform.w, renderMovingPlatform.h);
        platform.render(renderer, window);

        // Render player
        player.setPosition(renderPlayerX, renderPlayerY);
        player.setSize(playerWidth, playerHeight);
        player.render(renderer, window);

        // Timeline info display
        if (showTimelineInfo) {
            static int logCounter = 0;
            if (++logCounter > 60) {
                SDL_Log("Timeline - Scale: %.1fx, Paused: %s", 
                       playerTimeline.scale(), 
                       playerTimeline.isPaused() ? "Yes" : "No");
                logCounter = 0;
            }
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    // Cleanup
    running = false;
    playerThread.join();
    envThread.join();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}