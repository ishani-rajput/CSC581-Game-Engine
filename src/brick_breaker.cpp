#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <vector>
#include <string>
#include <cmath>
#include "entity.h"
#include "input.h"
#include "scaling.h"
#include "collision.h"
#include "timeline.h"
#include "event_manager.h"
#include "object_model.h"
#include "registry.h"
#include "memory_pool.h"

const int DESIGN_WIDTH = 1720;
const int DESIGN_HEIGHT = 1080;

// Game constants
const float PADDLE_WIDTH = 150.f;
const float PADDLE_HEIGHT = 20.f;
const float PADDLE_SPEED = 600.f;
const float PADDLE_DASH_SPEED = 1200.f;
const float BALL_SIZE = 40.f;
const float BALL_SPEED = 400.f;
const float BRICK_WIDTH = 80.f;
const float BRICK_HEIGHT = 30.f;
const int BRICK_ROWS = 8;
const int BRICK_COLS = 18;
const float BRICK_PADDING = 5.f;
const float BRICK_OFFSET_TOP = 100.f;
const float DASH_COOLDOWN = 1.0f;
const float CURVE_COOLDOWN = 2.0f;

struct Brick {
    float x, y;
    int health; // 0=destroyed, 1=green, 2=blue, 3=red
    bool active;
    std::string id;
};

void renderText(SDL_Renderer* renderer, const std::string& text, float x, float y,
                uint8_t r = 255, uint8_t g = 255, uint8_t b = 255, float scale = 1.0f) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
    float textWidth = text.length() * 9.f * scale;
    SDL_FRect bgRect = {x - 4, y - 2, textWidth, 16 * scale};
    SDL_RenderFillRect(renderer, &bgRect);
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    
    if (scale != 1.0f) {
        // Render scaled text by drawing each character larger
        float charX = x;
        for (char c : text) {
            std::string charStr(1, c);
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    SDL_RenderDebugText(renderer, charX + dx, y + dy, charStr.c_str());
                }
            }
            charX += 9 * scale;
        }
    } else {
        SDL_RenderDebugText(renderer, x, y, text.c_str());
    }
}

int main(int, char**) {
    SDL_Init(SDL_INIT_VIDEO);
    
    // Get display mode for fullscreen desktop resolution
    SDL_DisplayID displayID = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(displayID);
    int windowWidth = mode ? mode->w : DESIGN_WIDTH;
    int windowHeight = mode ? mode->h : DESIGN_HEIGHT;
    
    SDL_Window* window = SDL_CreateWindow("Brick Breaker - Engine Demo",
                                          windowWidth, windowHeight, 
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    Scaling::setMode(ScaleMode::Proportional);

    // ENGINE FEATURE: Timeline system
    Timeline gameTimeline;
    gameTimeline.anchorToRealTime();

    // ENGINE FEATURE: Event Manager
    Engine::EventManager eventManager(&gameTimeline);

    // ENGINE FEATURE: Memory Pool Allocators (Section 1 requirement)
    Engine::PoolAllocator<Engine::GameObject> brickPool(200);    // 200 bricks max

    // ENGINE FEATURE: Registry with pool allocators
    Engine::Registry brickRegistry(&brickPool);

    // ENGINE FEATURE: Input Chord Registration (Section 2 requirement)
    Input::registerChord(InputChord("paddle_dash_left", {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_A}));
    Input::registerChord(InputChord("paddle_dash_right", {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_D}));
    Input::registerChord(InputChord("curve_ball", {SDL_SCANCODE_SPACE, SDL_SCANCODE_C}));

    // Game state
    float paddleX = DESIGN_WIDTH / 2.f - PADDLE_WIDTH / 2.f;
    float paddleY = DESIGN_HEIGHT - 100.f;
    
    float ballX = DESIGN_WIDTH / 2.f - BALL_SIZE / 2.f;
    float ballY = paddleY - BALL_SIZE - 10.f;
    float ballVX = BALL_SPEED * 0.7f;
    float ballVY = -BALL_SPEED;
    bool ballLaunched = false;
    
    int lives = 3;
    int score = 0;
    bool gameStarted = false;
    bool gameOver = false;
    bool gameWon = false;
    
    float dashTimer = 0.f;
    float curveTimer = 0.f;
    bool isDashing = false;
    bool spaceWasPressed = false;
    float dashDuration = 0.f;
    float curveSpin = 0.f; // Extra horizontal velocity for curve effect
    
    // Status message system
    std::string statusMessage = "";
    int statusMessageTimer = 0;

    // Initialize brick grid
    int bricksRemaining = 0;
    float gridStartX = (DESIGN_WIDTH - (BRICK_COLS * (BRICK_WIDTH + BRICK_PADDING))) / 2.f;
    
    for (int row = 0; row < BRICK_ROWS; row++) {
        int health = (row < 2) ? 3 : (row < 5) ? 2 : 1; // Red top, blue middle, green bottom
        for (int col = 0; col < BRICK_COLS; col++) {
            float x = gridStartX + col * (BRICK_WIDTH + BRICK_PADDING);
            float y = BRICK_OFFSET_TOP + row * (BRICK_HEIGHT + BRICK_PADDING);
            
            std::string id = "brick_" + std::to_string(row) + "_" + std::to_string(col);
            auto& brick = brickRegistry.upsert(id);
            brick.set<float>("x", x);
            brick.set<float>("y", y);
            brick.set<int>("health", health);
            brick.set<bool>("active", true);
            bricksRemaining++;
        }
    }

    // ENGINE FEATURE: Event Listeners
    eventManager.registerListener(Engine::EventType::Death, [&](const Engine::Event& e) {
        std::string who = std::get<std::string>(e.payload.at("entity"));
        SDL_Log("[EVENT] Death: %s at time %.3f", who.c_str(), e.timestamp);
    });

    eventManager.registerListener(Engine::EventType::Collision, [&](const Engine::Event& e) {
        std::string objA = std::get<std::string>(e.payload.at("A"));
        std::string objB = std::get<std::string>(e.payload.at("B"));
        SDL_Log("[EVENT] Collision: %s <-> %s", objA.c_str(), objB.c_str());
    });

    eventManager.registerListener(Engine::EventType::InputChord, [&](const Engine::Event& e) {
        std::string chord = std::get<std::string>(e.payload.at("chord"));
        SDL_Log("[EVENT] Input Chord: %s activated", chord.c_str());
    });

    bool running = true;
    SDL_Event event;

    while (running) {
        // ENGINE FEATURE: Timeline tick
        double dt = gameTimeline.tick();
        if (dt > 0.05) dt = 0.05;

        // ENGINE FEATURE: Input polling
        Input::poll();

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) running = false;
                
                // Start game
                if (!gameStarted && event.key.scancode == SDL_SCANCODE_RETURN) {
                    gameStarted = true;
                    gameTimeline.anchorToRealTime();
                }
                
                if (event.key.scancode == SDL_SCANCODE_P && gameStarted && !gameOver && !gameWon) {
                    gameTimeline.togglePause();
                    statusMessage = gameTimeline.isPaused() ? "Paused" : "Resumed";
                    statusMessageTimer = 60;
                }
                
                // Timeline speed control
                if (event.key.scancode == SDL_SCANCODE_1 && gameStarted && !gameOver && !gameWon) {
                    gameTimeline.setScale(0.5);
                    statusMessage = "Speed: 0.5x (Slow Motion)";
                    statusMessageTimer = 120;
                }
                if (event.key.scancode == SDL_SCANCODE_2 && gameStarted && !gameOver && !gameWon) {
                    gameTimeline.setScale(1.0);
                    statusMessage = "Speed: 1.0x (Normal)";
                    statusMessageTimer = 120;
                }
                if (event.key.scancode == SDL_SCANCODE_3 && gameStarted && !gameOver && !gameWon) {
                    gameTimeline.setScale(2.0);
                    statusMessage = "Speed: 2.0x (Fast Forward)";
                    statusMessageTimer = 120;
                }
                
                // Scaling mode toggle
                if (event.key.scancode == SDL_SCANCODE_S) {
                    if (Scaling::mode() == ScaleMode::Proportional) {
                        Scaling::setMode(ScaleMode::Pixel);
                        statusMessage = "Scaling: Pixel Perfect";
                    } else {
                        Scaling::setMode(ScaleMode::Proportional);
                        statusMessage = "Scaling: Proportional";
                    }
                    statusMessageTimer = 120;
                }
                
                if (event.key.scancode == SDL_SCANCODE_R && (gameOver || gameWon)) {
                    // Go back to start screen
                    lives = 3;
                    score = 0;
                    gameStarted = false;
                    gameOver = false;
                    gameWon = false;
                    ballLaunched = false;
                    ballX = DESIGN_WIDTH / 2.f - BALL_SIZE / 2.f;
                    ballY = paddleY - BALL_SIZE - 10.f;
                    ballVX = BALL_SPEED * 0.7f;
                    ballVY = -BALL_SPEED;
                    paddleX = DESIGN_WIDTH / 2.f - PADDLE_WIDTH / 2.f;
                    curveSpin = 0.f;
                    
                    brickRegistry.clear();
                    bricksRemaining = 0;
                    
                    for (int row = 0; row < BRICK_ROWS; row++) {
                        int health = (row < 2) ? 3 : (row < 5) ? 2 : 1;
                        for (int col = 0; col < BRICK_COLS; col++) {
                            float x = gridStartX + col * (BRICK_WIDTH + BRICK_PADDING);
                            float y = BRICK_OFFSET_TOP + row * (BRICK_HEIGHT + BRICK_PADDING);
                            
                            std::string id = "brick_" + std::to_string(row) + "_" + std::to_string(col);
                            auto& brick = brickRegistry.upsert(id);
                            brick.set<float>("x", x);
                            brick.set<float>("y", y);
                            brick.set<int>("health", health);
                            brick.set<bool>("active", true);
                            bricksRemaining++;
                        }
                    }
                    
                    dashTimer = 0.f;
                    curveTimer = 0.f;
                    isDashing = false;
                    dashDuration = 0.f;
                    spaceWasPressed = false;  // Reset space key state
                }
            }
        }

        // Render start screen
        if (!gameStarted) {
            SDL_SetRenderDrawColor(renderer, 20, 20, 40, 255);
            SDL_RenderClear(renderer);
            
            // Draw title and instructions
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 220);
            SDL_FRect titleBox = {DESIGN_WIDTH / 2 - 350, DESIGN_HEIGHT / 2 - 250, 700, 500};
            SDL_RenderFillRect(renderer, &titleBox);
            
            SDL_SetRenderDrawColor(renderer, 255, 100, 0, 255);
            SDL_RenderRect(renderer, &titleBox);
            
            renderText(renderer, "BRICK BREAKER", DESIGN_WIDTH / 2 - 80, DESIGN_HEIGHT / 2 - 200, 255, 100, 0);
            renderText(renderer, "Break all the bricks with the ball!", DESIGN_WIDTH / 2 - 170, DESIGN_HEIGHT / 2 - 150, 200, 200, 200);
            
            renderText(renderer, "Controls:", DESIGN_WIDTH / 2 - 50, DESIGN_HEIGHT / 2 - 100, 255, 255, 0);
            renderText(renderer, "A/D or Arrows - Move Paddle", DESIGN_WIDTH / 2 - 130, DESIGN_HEIGHT / 2 - 70, 200, 200, 200);
            renderText(renderer, "Space - Launch Ball", DESIGN_WIDTH / 2 - 90, DESIGN_HEIGHT / 2 - 40, 200, 200, 200);
            renderText(renderer, "Shift+A/D - Paddle Dash", DESIGN_WIDTH / 2 - 110, DESIGN_HEIGHT / 2 - 10, 255, 200, 100);
            renderText(renderer, "Space+C - Curve Ball (add spin)", DESIGN_WIDTH / 2 - 140, DESIGN_HEIGHT / 2 + 20, 255, 200, 100);
            renderText(renderer, "P - Pause | 1/2/3 - Speed Control", DESIGN_WIDTH / 2 - 150, DESIGN_HEIGHT / 2 + 50, 200, 200, 200);
            renderText(renderer, "S - Toggle Scaling Mode", DESIGN_WIDTH / 2 - 120, DESIGN_HEIGHT / 2 + 80, 200, 200, 200);
            
            renderText(renderer, "Brick Colors:", DESIGN_WIDTH / 2 - 70, DESIGN_HEIGHT / 2 + 120, 255, 255, 0);
            renderText(renderer, "Red = 30pts | Blue = 20pts | Green = 10pts", DESIGN_WIDTH / 2 - 200, DESIGN_HEIGHT / 2 + 150, 200, 200, 200);
            
            renderText(renderer, "Press ENTER to Start", DESIGN_WIDTH / 2 - 110, DESIGN_HEIGHT / 2 + 200, 0, 255, 255);
            
            SDL_RenderPresent(renderer);
            SDL_Delay(16);
            continue;
        }
        
        if (gameTimeline.isPaused()) {
            // Render paused state
            SDL_SetRenderDrawColor(renderer, 20, 20, 40, 255);
            SDL_RenderClear(renderer);
            
            renderText(renderer, "PAUSED - Press P to Resume", DESIGN_WIDTH / 2 - 150, DESIGN_HEIGHT / 2, 255, 255, 0);
            
            SDL_RenderPresent(renderer);
            SDL_Delay(16);
            continue;
        }
        
        // Game over or won - show end screen and wait for restart
        if (gameOver || gameWon) {
            SDL_SetRenderDrawColor(renderer, 20, 20, 40, 255);
            SDL_RenderClear(renderer);
            
            // Show game over/win overlay
            if (gameOver) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 220);
                SDL_FRect overlay = {DESIGN_WIDTH / 2 - 300, DESIGN_HEIGHT / 2 - 150, 600, 300};
                SDL_RenderFillRect(renderer, &overlay);
                
                SDL_SetRenderDrawColor(renderer, 255, 50, 50, 255);
                SDL_RenderRect(renderer, &overlay);
                
                renderText(renderer, "GAME OVER!", DESIGN_WIDTH / 2 - 90, DESIGN_HEIGHT / 2 - 110, 255, 50, 50, 2.0f);
                renderText(renderer, "The ball fell below the paddle!", DESIGN_WIDTH / 2 - 160, DESIGN_HEIGHT / 2 - 60, 255, 100, 100);
                renderText(renderer, "Final Score: " + std::to_string(score), DESIGN_WIDTH / 2 - 120, DESIGN_HEIGHT / 2 - 20, 255, 255, 0, 1.5f);
                renderText(renderer, "Press R to Play Again", DESIGN_WIDTH / 2 - 100, DESIGN_HEIGHT / 2 + 40, 0, 255, 255);
                renderText(renderer, "Press ESC to Exit", DESIGN_WIDTH / 2 - 80, DESIGN_HEIGHT / 2 + 70, 150, 150, 150);
            }
            
            if (gameWon) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 220);
                SDL_FRect overlay = {DESIGN_WIDTH / 2 - 300, DESIGN_HEIGHT / 2 - 150, 600, 300};
                SDL_RenderFillRect(renderer, &overlay);
                
                SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                SDL_RenderRect(renderer, &overlay);
                
                renderText(renderer, "VICTORY!", DESIGN_WIDTH / 2 - 90, DESIGN_HEIGHT / 2 - 110, 0, 255, 0, 2.5f);
                renderText(renderer, "You broke all the bricks!", DESIGN_WIDTH / 2 - 130, DESIGN_HEIGHT / 2 - 60, 100, 255, 100);
                renderText(renderer, "Final Score: " + std::to_string(score), DESIGN_WIDTH / 2 - 120, DESIGN_HEIGHT / 2 - 20, 255, 255, 0, 1.5f);
                renderText(renderer, "Press R to Play Again", DESIGN_WIDTH / 2 - 100, DESIGN_HEIGHT / 2 + 40, 0, 255, 255);
                renderText(renderer, "Press ESC to Exit", DESIGN_WIDTH / 2 - 80, DESIGN_HEIGHT / 2 + 70, 150, 150, 150);
            }
            
            SDL_RenderPresent(renderer);
            SDL_Delay(16);
            continue;
        }

        // Update timers
        dashTimer += dt;
        curveTimer += dt;
        
        if (statusMessageTimer > 0) {
            statusMessageTimer--;
        }
        
        if (isDashing) {
            dashDuration -= dt;
            if (dashDuration <= 0) isDashing = false;
        }

        // Paddle movement
        float moveSpeed = isDashing ? PADDLE_DASH_SPEED : PADDLE_SPEED;
        
        if (Input::isKeyPressed(SDL_SCANCODE_A) || Input::isKeyPressed(SDL_SCANCODE_LEFT)) {
            paddleX -= moveSpeed * dt;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_D) || Input::isKeyPressed(SDL_SCANCODE_RIGHT)) {
            paddleX += moveSpeed * dt;
        }
        
        // Clamp paddle position
        if (paddleX < 0) paddleX = 0;
        if (paddleX + PADDLE_WIDTH > DESIGN_WIDTH) paddleX = DESIGN_WIDTH - PADDLE_WIDTH;

        // ENGINE FEATURE: Input Chords (Section 2 requirement)
        // Dash chords
        if (Input::isChordJustPressed("paddle_dash_left") && dashTimer >= DASH_COOLDOWN) {
            isDashing = true;
            dashDuration = 0.3f;
            dashTimer = 0.f;
            paddleX -= 150.f;
            if (paddleX < 0) paddleX = 0;
            
            statusMessage = "Paddle Dash Left!";
            statusMessageTimer = 30;
            eventManager.raiseEvent(Engine::Events::InputChord("paddle_dash_left", &gameTimeline));
        }
        if (Input::isChordJustPressed("paddle_dash_right") && dashTimer >= DASH_COOLDOWN) {
            isDashing = true;
            dashDuration = 0.3f;
            dashTimer = 0.f;
            paddleX += 150.f;
            if (paddleX + PADDLE_WIDTH > DESIGN_WIDTH) paddleX = DESIGN_WIDTH - PADDLE_WIDTH;
            
            statusMessage = "Paddle Dash Right!";
            statusMessageTimer = 30;
            eventManager.raiseEvent(Engine::Events::InputChord("paddle_dash_right", &gameTimeline));
        }
        
        // Curve ball chord (must check before regular Space launch)
        bool curveActivated = false;
        if (Input::isChordJustPressed("curve_ball") && curveTimer >= CURVE_COOLDOWN && ballLaunched) {
            curveSpin = (ballVX > 0) ? 200.f : -200.f; // Add spin based on current direction
            curveTimer = 0.f;
            statusMessage = "CURVE BALL!";
            statusMessageTimer = 60;
            eventManager.raiseEvent(Engine::Events::InputChord("curve_ball", &gameTimeline));
            curveActivated = true;
        }
        
        // Ball launch - only if Space is pressed but not as part of curve chord
        if (gameStarted && !ballLaunched && Input::isKeyPressed(SDL_SCANCODE_SPACE) && !Input::isChordActive("curve_ball") && !curveActivated) {
            if (!spaceWasPressed) {
                ballLaunched = true;
                statusMessage = "Ball Launched!";
                statusMessageTimer = 60;
            }
            spaceWasPressed = true;
        } else {
            spaceWasPressed = false;
        }

        // Ball physics
        if (!ballLaunched) {
            // Ball follows paddle before launch
            ballX = paddleX + PADDLE_WIDTH / 2.f - BALL_SIZE / 2.f;
            ballY = paddleY - BALL_SIZE - 5.f;
        } else {
            // Apply curve spin (gradually decrease)
            if (curveSpin != 0.f) {
                ballVX += curveSpin * dt;
                curveSpin *= 0.95f; // Decay spin over time
                if (std::abs(curveSpin) < 10.f) curveSpin = 0.f;
            }
            
            ballX += ballVX * dt;
            ballY += ballVY * dt;
            
            // Wall collisions
            if (ballX <= 0) {
                ballX = 0;
                ballVX = std::abs(ballVX);
                eventManager.raiseEvent(Engine::Events::Collision("ball", "wall_left", &gameTimeline));
            }
            if (ballX + BALL_SIZE >= DESIGN_WIDTH) {
                ballX = DESIGN_WIDTH - BALL_SIZE;
                ballVX = -std::abs(ballVX);
                eventManager.raiseEvent(Engine::Events::Collision("ball", "wall_right", &gameTimeline));
            }
            if (ballY <= 0) {
                ballY = 0;
                ballVY = std::abs(ballVY);
                eventManager.raiseEvent(Engine::Events::Collision("ball", "wall_top", &gameTimeline));
            }
            
            // Paddle collision
            SDL_FRect ballRect = {ballX, ballY, BALL_SIZE, BALL_SIZE};
            SDL_FRect paddleRect = {paddleX, paddleY, PADDLE_WIDTH, PADDLE_HEIGHT};
            
            if (aabbIntersect(ballRect, paddleRect) && ballVY > 0) {
                ballY = paddleY - BALL_SIZE;
                ballVY = -std::abs(ballVY);
                
                // Add spin based on where ball hits paddle
                float hitPos = (ballX + BALL_SIZE / 2.f) - (paddleX + PADDLE_WIDTH / 2.f);
                ballVX += hitPos * 2.f;
                
                // Clamp ball speed
                float speed = std::sqrt(ballVX * ballVX + ballVY * ballVY);
                if (speed > BALL_SPEED * 1.5f) {
                    ballVX = ballVX / speed * BALL_SPEED * 1.5f;
                    ballVY = ballVY / speed * BALL_SPEED * 1.5f;
                }
                
                eventManager.raiseEvent(Engine::Events::Collision("ball", "paddle", &gameTimeline));
            }
            
            // Ball fell below paddle
            if (ballY > DESIGN_HEIGHT) {
                lives--;
                eventManager.raiseEvent(Engine::Events::Death("ball", &gameTimeline));
                
                if (lives <= 0) {
                    gameOver = true;
                } else {
                    ballLaunched = false;
                    ballX = paddleX + PADDLE_WIDTH / 2.f - BALL_SIZE / 2.f;
                    ballY = paddleY - BALL_SIZE - 10.f;
                    ballVX = BALL_SPEED * 0.7f;
                    ballVY = -BALL_SPEED;
                    curveSpin = 0.f;
                    spaceWasPressed = false;  // Reset space key state for new launch
                    statusMessage = "Ball Lost! Lives: " + std::to_string(lives);
                    statusMessageTimer = 120;
                }
            }
            
            // Brick collisions
            for (const auto& id : brickRegistry.getAllIds()) {
                auto* brick = brickRegistry.get(id);
                if (!brick || !brick->get<bool>("active", false)) continue;
                
                float bx = brick->get<float>("x", 0.f);
                float by = brick->get<float>("y", 0.f);
                int health = brick->get<int>("health", 0);
                
                SDL_FRect brickRect = {bx, by, BRICK_WIDTH, BRICK_HEIGHT};
                
                if (aabbIntersect(ballRect, brickRect)) {
                    // Simple collision response - reverse ball direction
                    float ballCenterX = ballX + BALL_SIZE / 2.f;
                    float ballCenterY = ballY + BALL_SIZE / 2.f;
                    float brickCenterX = bx + BRICK_WIDTH / 2.f;
                    float brickCenterY = by + BRICK_HEIGHT / 2.f;
                    
                    // Determine collision side and push ball away
                    bool horizontalCollision = std::abs(ballCenterX - brickCenterX) > std::abs(ballCenterY - brickCenterY);
                    
                    if (horizontalCollision) {
                        ballVX = -ballVX;
                        // Push ball out of brick horizontally
                        if (ballCenterX < brickCenterX) {
                            ballX = bx - BALL_SIZE - 1.f; // Push left
                        } else {
                            ballX = bx + BRICK_WIDTH + 1.f; // Push right
                        }
                    } else {
                        ballVY = -ballVY;
                        // Push ball out of brick vertically
                        if (ballCenterY < brickCenterY) {
                            ballY = by - BALL_SIZE - 1.f; // Push up
                        } else {
                            ballY = by + BRICK_HEIGHT + 1.f; // Push down
                        }
                    }
                    
                    // Damage brick
                    health--;
                    if (health <= 0) {
                        brick->set<bool>("active", false);
                        bricksRemaining--;
                        int points = 10; // Green
                        if (health == -1) points = 20; // Was blue
                        if (health == -2) points = 30; // Was red
                        score += points;
                        
                        eventManager.raiseEvent(Engine::Events::Death(id, &gameTimeline));
                    } else {
                        brick->set<int>("health", health);
                    }
                    
                    eventManager.raiseEvent(Engine::Events::Collision("ball", id, &gameTimeline));
                    break; // Only one brick collision per frame
                }
            }
        }

        // Check win condition
        if (bricksRemaining == 0) {
            gameWon = true;
        }

        // ENGINE FEATURE: Dispatch events
        eventManager.dispatchEvents();

        // Render
        SDL_SetRenderDrawColor(renderer, 20, 20, 40, 255);
        SDL_RenderClear(renderer);

        // Draw bricks
        for (const auto& id : brickRegistry.getAllIds()) {
            auto* brick = brickRegistry.get(id);
            if (!brick || !brick->get<bool>("active", false)) continue;
            
            float x = brick->get<float>("x", 0.f);
            float y = brick->get<float>("y", 0.f);
            int health = brick->get<int>("health", 0);
            
            // Color based on health
            if (health == 3) SDL_SetRenderDrawColor(renderer, 255, 50, 50, 255);      // Red
            else if (health == 2) SDL_SetRenderDrawColor(renderer, 50, 150, 255, 255); // Blue
            else SDL_SetRenderDrawColor(renderer, 50, 255, 50, 255);                   // Green
            
            SDL_FRect brickRect = {x, y, BRICK_WIDTH, BRICK_HEIGHT};
            SDL_RenderFillRect(renderer, &brickRect);
            
            // Brick border
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderRect(renderer, &brickRect);
        }

        // Draw paddle
        SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
        SDL_FRect paddleRect = {paddleX, paddleY, PADDLE_WIDTH, PADDLE_HEIGHT};
        SDL_RenderFillRect(renderer, &paddleRect);
        SDL_SetRenderDrawColor(renderer, 100, 100, 100, 255);
        SDL_RenderRect(renderer, &paddleRect);

        // Draw ball as filled circle
        SDL_SetRenderDrawColor(renderer, 255, 80, 80, 255);
        float radius = BALL_SIZE / 2.f;
        float centerX = ballX + radius;
        float centerY = ballY + radius;
        // Draw filled circle using multiple rectangles
        for (int y = -radius; y <= radius; y++) {
            float width = std::sqrt(radius * radius - y * y) * 2;
            SDL_FRect rect = {centerX - width/2, centerY + y, width, 1};
            SDL_RenderFillRect(renderer, &rect);
        }

        // UI
        renderText(renderer, "Score: " + std::to_string(score), 20, 20, 255, 255, 0);
        renderText(renderer, "Lives: " + std::to_string(lives), 20, 40, 255, 100, 100);
        renderText(renderer, "Bricks: " + std::to_string(bricksRemaining), 20, 60, 100, 255, 100);
        
        renderText(renderer, "Controls: A/D - Move | Space - Launch", 20, DESIGN_HEIGHT - 80, 200, 200, 200);
        renderText(renderer, "Chords: Shift+A/D - Dash | Space+C - Curve", 20, DESIGN_HEIGHT - 60, 255, 200, 100);
        renderText(renderer, "P - Pause | 1/2/3 - Speed | S - Scaling", 20, DESIGN_HEIGHT - 40, 200, 200, 200);
        
        std::string scaleText = "Speed: " + std::to_string(int(gameTimeline.scale() * 100)) + "%";
        renderText(renderer, scaleText, 20, DESIGN_HEIGHT - 20, 0, 255, 0);
        
        // Cooldown indicators
        if (dashTimer < DASH_COOLDOWN) {
            float pct = dashTimer / DASH_COOLDOWN;
            std::string cooldown = "Dash: " + std::to_string((int)((1.0f - pct) * DASH_COOLDOWN * 10) / 10.0f) + "s";
            renderText(renderer, cooldown, DESIGN_WIDTH - 150, 20, 255, 150, 0);
        } else {
            renderText(renderer, "Dash: READY", DESIGN_WIDTH - 150, 20, 0, 255, 0);
        }
        
        if (curveTimer < CURVE_COOLDOWN && ballLaunched) {
            float pct = curveTimer / CURVE_COOLDOWN;
            std::string cooldown = "Curve: " + std::to_string((int)((1.0f - pct) * CURVE_COOLDOWN * 10) / 10.0f) + "s";
            renderText(renderer, cooldown, DESIGN_WIDTH - 150, 40, 255, 150, 0);
        } else if (ballLaunched) {
            renderText(renderer, "Curve: READY", DESIGN_WIDTH - 150, 40, 0, 255, 0);
        }
        
        // Scaling mode indicator
        std::string scalingMode = (Scaling::mode() == ScaleMode::Proportional) ? "Proportional" : "Pixel";
        renderText(renderer, "Scaling: " + scalingMode, DESIGN_WIDTH - 200, 60, 150, 150, 255);
        
        // Status message
        if (statusMessageTimer > 0) {
            renderText(renderer, statusMessage, DESIGN_WIDTH / 2 - 150, 100, 255, 255, 0);
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
