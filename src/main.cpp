#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <string>
#include "entity.h"
#include "input.h"
#include "scaling.h"
#include "physics.h"
#include "collision.h"
#include "timeline.h"

const int DESIGN_WIDTH = 1720;
const int DESIGN_HEIGHT = 1080;

const float PLAYER_SPEED = 300.f;
const float JUMP_VELOCITY = -850.f;

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

// Simple text rendering using SDL3's built-in debug text
void renderSimpleText(SDL_Renderer* renderer, const std::string& text, float x, float y, 
                     uint8_t r = 255, uint8_t g = 255, uint8_t b = 255) {
    // Draw background for visibility
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
    float textWidth = text.length() * 9.f;  // Approximate width
    SDL_FRect bgRect = {x - 4, y - 2, textWidth, 16};
    SDL_RenderFillRect(renderer, &bgRect);
    
    // Draw text using SDL3's built-in debug text renderer
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    SDL_RenderDebugText(renderer, x, y, text.c_str());
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
    SDL_Window* window = SDL_CreateWindow("Feeling Spikey", 
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
    Entity flag(renderer, "../assets/flag.png", 0, 0, 256, 256, 1, 0);

    // SMALLER PLAYER SIZE
    int frameCount = 8;
    int frameWidth = 128;
    int frameHeight = 128;
    float playerScale = 1.2f;  // Reduced from 2.0f
    float playerWidth = frameWidth * playerScale;
    float playerHeight = frameHeight * playerScale;
    
    Entity player(renderer, "../assets/player.png", 0, 0, frameWidth, frameHeight, frameCount, 150);

    // SMALLER PLATFORM SIZES
    float platformWidth = 200.f;   // Reduced from 400
    float platformHeight = 350.f;  // Reduced from 500
    
    // Platform 1: Left starting platform
    float leftX = 0.f;
    float leftY = DESIGN_HEIGHT - platformHeight;
    
    // Platform 2: Middle platform
    float middleX = DESIGN_WIDTH / 2.f - platformWidth / 2.f - 80.f;
    float middleY = DESIGN_HEIGHT - platformHeight;
    
    // Platform 3: Right elevated goal platform (HIGHER than middle)
    float rightX = DESIGN_WIDTH - platformWidth - 300.f;
    float rightY = DESIGN_HEIGHT - platformHeight - 200.f;  // 200px higher!

    // Shared data structures
    PlayerData playerData;
    playerData.x = leftX + 80.f;
    playerData.y = leftY - playerHeight;
    playerData.vx = 0.f;
    playerData.vy = 0.f;
    playerData.grounded = false;

    // Two moving platforms: 1 horizontal, 1 vertical
    struct MovingPlatforms {
        SDL_FRect horizontal;
        int horizontalDir;
        SDL_FRect vertical;
        int verticalDir;
        std::mutex mutex;
    };
    
    MovingPlatforms movingPlats;
    // Horizontal platform (between left and middle)
    movingPlats.horizontal = {
        leftX + platformWidth + 100.f,
        DESIGN_HEIGHT - platformHeight - 150.f,
        180.f,
        90.f
    };
    movingPlats.horizontalDir = 1;
    
    // Vertical platform (between middle and right elevated)
    movingPlats.vertical = {
        middleX + platformWidth + 100.f,
        middleY - 180.f,
        180.f,
        85.f
    };
    movingPlats.verticalDir = 1;

    Body playerBody = { 0.f, 0.f, true };
    float platformSpeed = 200.f;
    float verticalSpeed = 150.f;

    std::atomic<bool> running{true};
    std::atomic<bool> gameWon{false};
    
    // UI state for on-screen messages
    std::string statusMessage = "";
    int statusMessageTimer = 0;

    // Thread 1: Player Logic Thread
    std::thread playerThread([&]() {
        bool wasGrounded = false;
        
        while (running) {
            double dt = playerTimeline.tick();
            if (dt > 0.05f) dt = 0.05f;

            if (!playerTimeline.isPaused() && !gameWon) {
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

                // Static platform collisions - NOW WITH 3 PLATFORMS
                SDL_FRect leftPlatformRect = { leftX, leftY, platformWidth, platformHeight };
                SDL_FRect middlePlatformRect = { middleX, middleY, platformWidth, platformHeight };
                SDL_FRect rightPlatformRect = { rightX, rightY, platformWidth, platformHeight };
                
                // Left platform
                if (checkCollision(playerRect, leftPlatformRect)) {
                    if (playerBody.vy >= 0 && prevPlayerY + playerHeight <= leftY + 20) {
                        float playerCenterX = newX + playerWidth / 2.f;
                        if (playerCenterX > leftX && playerCenterX < leftX + platformWidth) {
                            newY = leftY - playerHeight;
                            playerBody.vy = 0;
                            isGrounded = true;
                        }
                    }
                }
                
                // Middle platform
                if (checkCollision(playerRect, middlePlatformRect)) {
                    if (playerBody.vy >= 0 && prevPlayerY + playerHeight <= middleY + 20) {
                        float playerCenterX = newX + playerWidth / 2.f;
                        if (playerCenterX > middleX && playerCenterX < middleX + platformWidth) {
                            newY = middleY - playerHeight;
                            playerBody.vy = 0;
                            isGrounded = true;
                        }
                    }
                }
                
                // Right platform (GOAL - elevated)
                if (checkCollision(playerRect, rightPlatformRect)) {
                    if (playerBody.vy >= 0 && prevPlayerY + playerHeight <= rightY + 20) {
                        float playerCenterX = newX + playerWidth / 2.f;
                        if (playerCenterX > rightX && playerCenterX < rightX + platformWidth) {
                            newY = rightY - playerHeight;
                            playerBody.vy = 0;
                            isGrounded = true;
                            
                            // WIN CONDITION - reached right elevated platform
                            if (newX > rightX + 50.f) {
                                gameWon = true;
                                statusMessage = "YOU WIN!";
                                statusMessageTimer = 300;
                            }
                        }
                    }
                }

                // Moving platforms collision - BOTH horizontal and vertical
                SDL_FRect horizontalPlatform, verticalPlatform;
                static float prevHorizontalX = 0.f;
                static float prevVerticalY = 0.f;
                
                {
                    std::lock_guard<std::mutex> lock(movingPlats.mutex);
                    horizontalPlatform = movingPlats.horizontal;
                    verticalPlatform = movingPlats.vertical;
                }
                
                // Update player rect
                playerRect = { newX, newY, playerWidth, playerHeight };
                
                // Check HORIZONTAL moving platform
                bool playerOverHorizontal = (newX + playerWidth > horizontalPlatform.x) && 
                                           (newX < horizontalPlatform.x + horizontalPlatform.w);
                
                if (playerOverHorizontal && checkCollision(playerRect, horizontalPlatform)) {
                    float platformTop = horizontalPlatform.y;
                    float playerBottom = prevPlayerY + playerHeight;
                    
                    if (playerBody.vy >= 0 && playerBottom <= platformTop + 25.f) {
                        float playerCenterX = newX + playerWidth / 2.f;
                        if (playerCenterX > horizontalPlatform.x && 
                            playerCenterX < horizontalPlatform.x + horizontalPlatform.w) {
                            newY = platformTop - playerHeight;
                            playerBody.vy = 0;
                            isGrounded = true;
                            
                            float platformDelta = horizontalPlatform.x - prevHorizontalX;
                            newX += platformDelta;
                        }
                    }
                }
                prevHorizontalX = horizontalPlatform.x;
                
                // Update player rect again
                playerRect = { newX, newY, playerWidth, playerHeight };
                
                // Check VERTICAL moving platform
                bool playerOverVertical = (newX + playerWidth > verticalPlatform.x) && 
                                         (newX < verticalPlatform.x + verticalPlatform.w);
                
                if (playerOverVertical && checkCollision(playerRect, verticalPlatform)) {
                    float platformTop = verticalPlatform.y;
                    float playerBottom = prevPlayerY + playerHeight;
                    
                    if (playerBody.vy >= 0 && playerBottom <= platformTop + 25.f) {
                        float playerCenterX = newX + playerWidth / 2.f;
                        if (playerCenterX > verticalPlatform.x && 
                            playerCenterX < verticalPlatform.x + verticalPlatform.w) {
                            newY = platformTop - playerHeight;
                            playerBody.vy = 0;
                            isGrounded = true;
                            
                            // Ride vertical platform
                            float platformDelta = verticalPlatform.y - prevVerticalY;
                            newY += platformDelta;
                        }
                    }
                }
                prevVerticalY = verticalPlatform.y;

                // Update player rect again for spike collision
                playerRect = { newX, newY, playerWidth, playerHeight };

                // Spike collision - TWO ZONES of spikes
                float spikeW = 220.f;
                float spikeH = 280.f;
                float spikeY = DESIGN_HEIGHT - spikeH;
                
                // Spikes between left and middle platforms
                for (float x = platformWidth; x < middleX; x += spikeW) {
                    SDL_FRect spikeRect = { x, spikeY, spikeW, spikeH };
                    if (checkCollision(playerRect, spikeRect)) {
                        newX = leftX + 80.f;
                        newY = leftY - playerHeight;
                        playerBody.vx = 0.f;
                        playerBody.vy = 0.f;
                        statusMessage = "OUCH! Respawned";
                        statusMessageTimer = 120;
                        SDL_Log("Hit spike! Respawning...");
                        break;
                    }
                }
                
                // Spikes between middle and right platforms
                for (float x = middleX + platformWidth; x < rightX; x += spikeW) {
                    SDL_FRect spikeRect = { x, spikeY, spikeW, spikeH };
                    if (checkCollision(playerRect, spikeRect)) {
                        newX = leftX + 80.f;
                        newY = leftY - playerHeight;
                        playerBody.vx = 0.f;
                        playerBody.vy = 0.f;
                        statusMessage = "OUCH! Respawned";
                        statusMessageTimer = 120;
                        SDL_Log("Hit spike! Respawning...");
                        break;
                    }
                }

                // Fall off screen reset
                if (newY > DESIGN_HEIGHT + 100) {
                    newX = leftX + 80.f;
                    newY = leftY - playerHeight;
                    playerBody.vx = 0.f;
                    playerBody.vy = 0.f;
                    statusMessage = "Fell! Respawned";
                    statusMessageTimer = 120;
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

    // Thread 2: Environment Thread - updates BOTH moving platforms
    std::thread envThread([&]() {
        while (running) {
            double dt = envTimeline.tick();
            if (dt > 0.05f) dt = 0.05f;

            if (!envTimeline.isPaused() && !gameWon) {
                std::lock_guard<std::mutex> lock(movingPlats.mutex);
                
                // Update HORIZONTAL platform
                movingPlats.horizontal.x += platformSpeed * movingPlats.horizontalDir * dt;
                if (movingPlats.horizontal.x < platformWidth) {
                    movingPlats.horizontal.x = platformWidth;
                    movingPlats.horizontalDir = 1;
                } else if (movingPlats.horizontal.x + movingPlats.horizontal.w > middleX) {
                    movingPlats.horizontal.x = middleX - movingPlats.horizontal.w;
                    movingPlats.horizontalDir = -1;
                }
                
                // Update VERTICAL platform
                movingPlats.vertical.y += verticalSpeed * movingPlats.verticalDir * dt;
                float verticalMin = rightY - 50.f;  // Can go slightly above right platform
                float verticalMax = middleY;  // Down to middle platform level
                
                if (movingPlats.vertical.y < verticalMin) {
                    movingPlats.vertical.y = verticalMin;
                    movingPlats.verticalDir = 1;
                } else if (movingPlats.vertical.y > verticalMax) {
                    movingPlats.vertical.y = verticalMax;
                    movingPlats.verticalDir = -1;
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
            }
            
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_S) {
                if (Scaling::mode() == ScaleMode::Pixel) {
                    Scaling::setMode(ScaleMode::Proportional);
                    statusMessage = "Proportional Scaling";
                } else {
                    Scaling::setMode(ScaleMode::Pixel);
                    statusMessage = "Pixel Scaling";
                }
                statusMessageTimer = 120;
            }

            // Timeline Controls
            if (e.type == SDL_EVENT_KEY_DOWN) {
                switch (e.key.scancode) {
                    case SDL_SCANCODE_P:
                        playerTimeline.togglePause();
                        envTimeline.pause(playerTimeline.isPaused());
                        statusMessage = playerTimeline.isPaused() ? "PAUSED" : "RESUMED";
                        statusMessageTimer = 120;
                        break;
                    case SDL_SCANCODE_1:
                        playerTimeline.setScale(0.5);
                        envTimeline.setScale(0.5);
                        statusMessage = "Speed: SLOW (0.5x)";
                        statusMessageTimer = 120;
                        break;
                    case SDL_SCANCODE_2:
                        playerTimeline.setScale(1.0);
                        envTimeline.setScale(1.0);
                        statusMessage = "Speed: NORMAL (1.0x)";
                        statusMessageTimer = 120;
                        break;
                    case SDL_SCANCODE_3:
                        playerTimeline.setScale(2.0);
                        envTimeline.setScale(2.0);
                        statusMessage = "Speed: FAST (2.0x)";
                        statusMessageTimer = 120;
                        break;
                    case SDL_SCANCODE_R:
                        if (gameWon) {
                            gameWon = false;
                            std::lock_guard<std::mutex> lock(playerData.mutex);
                            playerData.x = leftX + 80.f;
                            playerData.y = leftY - playerHeight;
                            playerData.vx = playerData.vy = 0.f;
                            statusMessage = "Game Restarted";
                            statusMessageTimer = 120;
                        }
                        break;
                    default:
                        break;
                }
            }
        }

        Input::poll();

        // Get current game state for rendering
        float renderPlayerX, renderPlayerY;
        SDL_FRect renderHorizontalPlatform, renderVerticalPlatform;
        
        {
            std::lock_guard<std::mutex> lock(playerData.mutex);
            renderPlayerX = playerData.x;
            renderPlayerY = playerData.y;
        }
        
        {
            std::lock_guard<std::mutex> lock(movingPlats.mutex);
            renderHorizontalPlatform = movingPlats.horizontal;
            renderVerticalPlatform = movingPlats.vertical;
        }

        // Update player animation
        player.update();

        // Rendering
        SDL_SetRenderDrawColor(renderer, 135, 206, 235, 255);
        SDL_RenderClear(renderer);

        // Render spikes in BOTH gaps
        float spikeW = 220.f;
        float spikeH = 280.f;
        float spikeY = DESIGN_HEIGHT - spikeH;
        
        // Spikes between left and middle
        for (float x = platformWidth; x < middleX; x += spikeW) {
            spike.setPosition(x, spikeY);
            spike.setSize(spikeW, spikeH);
            spike.render(renderer, window);
        }
        
        // Spikes between middle and right
        for (float x = middleX + platformWidth; x < rightX; x += spikeW) {
            spike.setPosition(x, spikeY);
            spike.setSize(spikeW, spikeH);
            spike.render(renderer, window);
        }

        // Render static platforms (3 platforms now)
        // Left platform
        groundBottom.setPosition(leftX, leftY);
        groundBottom.setSize(platformWidth, platformHeight);
        groundBottom.render(renderer, window);
        groundTop.setPosition(leftX, leftY - 48.f);
        groundTop.setSize(platformWidth, 48.f);
        groundTop.render(renderer, window);

        // Middle platform
        groundBottom.setPosition(middleX, middleY);
        groundBottom.setSize(platformWidth, platformHeight);
        groundBottom.render(renderer, window);
        groundTop.setPosition(middleX, middleY - 48.f);
        groundTop.setSize(platformWidth, 48.f);
        groundTop.render(renderer, window);

        // Right elevated platform (with flag)
        groundBottom.setPosition(rightX, rightY);
        groundBottom.setSize(platformWidth, platformHeight);
        groundBottom.render(renderer, window);
        groundTop.setPosition(rightX, rightY - 48.f);
        groundTop.setSize(platformWidth, 48.f);
        groundTop.render(renderer, window);
        
        // Render flag on right elevated platform
        float flagW = 150.f;
        float flagH = 150.f;
        flag.setPosition(rightX + platformWidth - flagW - 30.f, rightY - flagH - 48.f);
        flag.setSize(flagW, flagH);
        flag.render(renderer, window);

        // Render BOTH moving platforms
        platform.setPosition(renderHorizontalPlatform.x, renderHorizontalPlatform.y);
        platform.setSize(renderHorizontalPlatform.w, renderHorizontalPlatform.h);
        platform.render(renderer, window);
        
        platform.setPosition(renderVerticalPlatform.x, renderVerticalPlatform.y);
        platform.setSize(renderVerticalPlatform.w, renderVerticalPlatform.h);
        platform.render(renderer, window);

        // Render player (smaller)
        player.setPosition(renderPlayerX, renderPlayerY);
        player.setSize(playerWidth, playerHeight);
        player.render(renderer, window);

        // SIMPLE TEXT RENDERING using SDL3's built-in debug text
        // Controls (top left)
        renderSimpleText(renderer, "A/D: Move  W/SPACE: Jump", 20, 20, 255, 255, 255);
        renderSimpleText(renderer, "P: Pause  1/2/3: Speed  R: Restart", 20, 40, 255, 255, 255);
        
        // Status (top right)
        std::string speedText = "Speed: " + std::to_string(int(playerTimeline.scale() * 100)) + "%";
        renderSimpleText(renderer, speedText, DESIGN_WIDTH - 200, 20, 0, 255, 0);
        
        if (playerTimeline.isPaused()) {
            renderSimpleText(renderer, "PAUSED", DESIGN_WIDTH - 200, 40, 255, 0, 0);
        }
        
        // Status messages (center)
        if (statusMessageTimer > 0) {
            renderSimpleText(renderer, statusMessage, DESIGN_WIDTH / 2 - 100, 100, 255, 255, 0);
            statusMessageTimer--;
        }

        // Win screen overlay
        if (gameWon) {
            // Semi-transparent overlay
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
            SDL_FRect overlay = {0, 0, (float)DESIGN_WIDTH, (float)DESIGN_HEIGHT};
            SDL_RenderFillRect(renderer, &overlay);
            
            // Win indicator box
            SDL_SetRenderDrawColor(renderer, 255, 215, 0, 255);
            SDL_FRect winBox = {DESIGN_WIDTH / 2 - 300, DESIGN_HEIGHT / 2 - 80, 600, 160};
            SDL_RenderFillRect(renderer, &winBox);
            
            // Win text
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderDebugText(renderer, DESIGN_WIDTH / 2 - 80, DESIGN_HEIGHT / 2 - 40, "YOU WIN!");
            SDL_RenderDebugText(renderer, DESIGN_WIDTH / 2 - 120, DESIGN_HEIGHT / 2 + 20, "Press R to Restart");
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
};
