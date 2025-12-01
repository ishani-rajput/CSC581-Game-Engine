#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <vector>
#include <string>
#include <cmath>
#include <random>
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
const float PLAYER_SPEED = 400.f;
const float PLAYER_DASH_SPEED = 800.f;
const float BULLET_SPEED = 500.f;
const float ENEMY_BULLET_SPEED = 300.f;
const float ALIEN_MOVE_SPEED = 50.f;
const float ALIEN_DROP_DISTANCE = 30.f;
const float SHOOT_COOLDOWN = 0.3f;
const float SUPER_SHOT_COOLDOWN = 2.0f;
const float DASH_COOLDOWN = 1.0f;

// Sprite sizes (Kenney assets are typically square)
const float PLAYER_SIZE = 75.f;
const float ALIEN_SIZE = 60.f;
const float BULLET_WIDTH = 9.f;
const float BULLET_HEIGHT = 37.f;
const float EXPLOSION_SIZE = 64.f;

enum class BulletType {
    PlayerNormal,
    PlayerSuper,
    Enemy
};

struct Bullet {
    float x, y;
    float vy;
    bool active;
    BulletType type;
    std::string id;

    Bullet() : x(0), y(0), vy(0), active(false), type(BulletType::PlayerNormal), id("") {}
    Bullet(float px, float py, float speed, BulletType t, const std::string& bulletId)
        : x(px), y(py), vy(speed), active(true), type(t), id(bulletId) {}
};

struct Alien {
    float x, y;
    bool active;
    int type; // 0=black, 1=blue, 2=green
    std::string id;

    Alien() : x(0), y(0), active(false), type(0), id("") {}
    Alien(float px, float py, int t, const std::string& alienId)
        : x(px), y(py), active(true), type(t), id(alienId) {}
};

struct Explosion {
    float x, y;
    float lifetime;
    bool active;

    Explosion() : x(0), y(0), lifetime(0), active(false) {}
    Explosion(float px, float py) : x(px), y(py), lifetime(0.5f), active(true) {}
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
    
    SDL_Window* window = SDL_CreateWindow("Space Invaders - Engine Demo",
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
    Engine::PoolAllocator<Engine::GameObject> bulletPool(150);   // 150 bullets max
    Engine::PoolAllocator<Engine::GameObject> alienPool(60);     // 60 aliens max
    Engine::PoolAllocator<Engine::GameObject> explosionPool(30); // 30 explosions max

    // ENGINE FEATURE: Registry with pool allocators
    Engine::Registry bulletRegistry(&bulletPool);
    Engine::Registry alienRegistry(&alienPool);
    Engine::Registry explosionRegistry(&explosionPool);

    // ENGINE FEATURE: Input Chord Registration (Section 2 requirement)
    Input::registerChord(InputChord("super_shot", {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_SPACE}));
    Input::registerChord(InputChord("dash_left", {SDL_SCANCODE_LCTRL, SDL_SCANCODE_A}));
    Input::registerChord(InputChord("dash_right", {SDL_SCANCODE_LCTRL, SDL_SCANCODE_D}));

    // Load sprites
    Entity background(renderer, "../assets/space_invaders/background.png", 0, 0, 256, 256, 1, 0);
    Entity playerShip(renderer, "../assets/space_invaders/playerShip1_red.png", 0, 0, 99, 75, 1, 0);
    Entity alienBlack(renderer, "../assets/space_invaders/enemyBlack3.png", 0, 0, 103, 84, 1, 0);
    Entity alienBlue(renderer, "../assets/space_invaders/enemyBlue4.png", 0, 0, 93, 84, 1, 0);
    Entity alienGreen(renderer, "../assets/space_invaders/enemyGreen3.png", 0, 0, 93, 84, 1, 0);
    Entity laserBlue(renderer, "../assets/space_invaders/laserBlue13.png", 0, 0, 9, 37, 1, 0);
    Entity laserRed(renderer, "../assets/space_invaders/laserRed10.png", 0, 0, 9, 37, 1, 0);
    Entity meteor(renderer, "../assets/space_invaders/meteorBrown_big4.png", 0, 0, 101, 84, 1, 0);

    // Game state
    float playerX = DESIGN_WIDTH / 2.f - PLAYER_SIZE / 2.f;
    float playerY = DESIGN_HEIGHT - PLAYER_SIZE - 20.f;
    int playerLives = 3;
    int score = 0;
    bool gameStarted = false;
    bool gameOver = false;
    bool gameWon = false;

    float shootTimer = 0.f;
    float superShotTimer = 0.f;
    float dashTimer = 0.f;
    bool isDashing = false;
    float dashDuration = 0.f;
    
    // Status message system
    std::string statusMessage = "";
    int statusMessageTimer = 0;

    // Alien grid state
    float alienGridX = 100.f;
    float alienGridY = 80.f;
    int alienDirection = 1; // 1 = right, -1 = left
    float alienMoveTimer = 0.f;
    const float ALIEN_MOVE_INTERVAL = 0.5f;
    int aliensRemaining = 0;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> shootChance(0.0, 1.0);

    // Initialize alien grid (5 rows x 11 columns)
    int alienId = 0;
    for (int row = 0; row < 5; row++) {
        int alienType = (row == 0) ? 0 : (row <= 2) ? 1 : 2; // Black on top, blue middle, green bottom
        for (int col = 0; col < 11; col++) {
            float x = alienGridX + col * (ALIEN_SIZE + 10.f);
            float y = alienGridY + row * (ALIEN_SIZE + 10.f);
            
            std::string id = "alien_" + std::to_string(alienId++);
            auto& alien = alienRegistry.upsert(id);
            alien.set<float>("x", x);
            alien.set<float>("y", y);
            alien.set<int>("type", alienType);
            alien.set<bool>("active", true);
            aliensRemaining++;
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
                    playerLives = 3;
                    score = 0;
                    gameStarted = false;
                    gameOver = false;
                    gameWon = false;
                    playerX = DESIGN_WIDTH / 2.f - PLAYER_SIZE / 2.f;
                    
                    bulletRegistry.clear();
                    alienRegistry.clear();
                    explosionRegistry.clear();
                    
                    alienId = 0;
                    aliensRemaining = 0;
                    for (int row = 0; row < 5; row++) {
                        int alienType = (row == 0) ? 0 : (row <= 2) ? 1 : 2;
                        for (int col = 0; col < 11; col++) {
                            float x = alienGridX + col * (ALIEN_SIZE + 10.f);
                            float y = alienGridY + row * (ALIEN_SIZE + 10.f);
                            
                            std::string id = "alien_" + std::to_string(alienId++);
                            auto& alien = alienRegistry.upsert(id);
                            alien.set<float>("x", x);
                            alien.set<float>("y", y);
                            alien.set<int>("type", alienType);
                            alien.set<bool>("active", true);
                            aliensRemaining++;
                        }
                    }
                    alienDirection = 1;
                    
                    shootTimer = 0.f;
                    superShotTimer = 0.f;
                    dashTimer = 0.f;
                    isDashing = false;
                    dashDuration = 0.f;
                }
            }
        }

        // Render start screen
        if (!gameStarted) {
            SDL_SetRenderDrawColor(renderer, 10, 10, 30, 255);
            SDL_RenderClear(renderer);
            
            // Background - stretch to fill entire design space
            background.setPosition(0, 0);
            background.setSize(DESIGN_WIDTH, DESIGN_HEIGHT);
            background.render(renderer, window);
            
            // Draw title and instructions
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 220);
            SDL_FRect titleBox = {DESIGN_WIDTH / 2 - 350, DESIGN_HEIGHT / 2 - 200, 700, 400};
            SDL_RenderFillRect(renderer, &titleBox);
            
            SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
            SDL_RenderRect(renderer, &titleBox);
            
            renderText(renderer, "SPACE INVADERS", DESIGN_WIDTH / 2 - 90, DESIGN_HEIGHT / 2 - 150, 0, 255, 0);
            renderText(renderer, "Destroy all alien invaders!", DESIGN_WIDTH / 2 - 140, DESIGN_HEIGHT / 2 - 100, 200, 200, 200);
            
            renderText(renderer, "Controls:", DESIGN_WIDTH / 2 - 50, DESIGN_HEIGHT / 2 - 70, 255, 255, 0);
            renderText(renderer, "A/D or Arrows - Move", DESIGN_WIDTH / 2 - 110, DESIGN_HEIGHT / 2 - 40, 200, 200, 200);
            renderText(renderer, "Space - Shoot", DESIGN_WIDTH / 2 - 70, DESIGN_HEIGHT / 2 - 10, 200, 200, 200);
            renderText(renderer, "Shift+Space - Super Shot (pierces)", DESIGN_WIDTH / 2 - 170, DESIGN_HEIGHT / 2 + 20, 255, 200, 100);
            renderText(renderer, "Ctrl+A/D - Quick Dash", DESIGN_WIDTH / 2 - 110, DESIGN_HEIGHT / 2 + 50, 255, 200, 100);
            renderText(renderer, "P - Pause | 1/2/3 - Speed Control", DESIGN_WIDTH / 2 - 150, DESIGN_HEIGHT / 2 + 80, 200, 200, 200);
            renderText(renderer, "S - Toggle Scaling Mode", DESIGN_WIDTH / 2 - 120, DESIGN_HEIGHT / 2 + 110, 200, 200, 200);
            
            renderText(renderer, "Press ENTER to Start", DESIGN_WIDTH / 2 - 110, DESIGN_HEIGHT / 2 + 160, 0, 255, 255);
            
            SDL_RenderPresent(renderer);
            SDL_Delay(16);
            continue;
        }
        
        if (gameTimeline.isPaused()) {
            // Render paused state
            SDL_SetRenderDrawColor(renderer, 10, 10, 30, 255);
            SDL_RenderClear(renderer);
            
            // Background - stretch to fill entire design space
            background.setPosition(0, 0);
            background.setSize(DESIGN_WIDTH, DESIGN_HEIGHT);
            background.render(renderer, window);
            
            renderText(renderer, "PAUSED - Press P to Resume", DESIGN_WIDTH / 2 - 150, DESIGN_HEIGHT / 2, 255, 255, 0);
            
            SDL_RenderPresent(renderer);
            SDL_Delay(16);
            continue;
        }
        
        // Game over or won - show end screen and wait for restart
        if (gameOver || gameWon) {
            SDL_SetRenderDrawColor(renderer, 10, 10, 30, 255);
            SDL_RenderClear(renderer);
            
            // Background
            background.setPosition(0, 0);
            background.setSize(DESIGN_WIDTH, DESIGN_HEIGHT);
            background.render(renderer, window);
            
            // Show game over/win overlay
            if (gameOver) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 220);
                SDL_FRect overlay = {DESIGN_WIDTH / 2 - 300, DESIGN_HEIGHT / 2 - 150, 600, 300};
                SDL_RenderFillRect(renderer, &overlay);
                
                SDL_SetRenderDrawColor(renderer, 255, 50, 50, 255);
                SDL_RenderRect(renderer, &overlay);
                
                renderText(renderer, "GAME OVER!", DESIGN_WIDTH / 2 - 90, DESIGN_HEIGHT / 2 - 110, 255, 50, 50, 2.0f);
                renderText(renderer, "The aliens have invaded Earth!", DESIGN_WIDTH / 2 - 160, DESIGN_HEIGHT / 2 - 60, 255, 100, 100);
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
                renderText(renderer, "You saved Earth from the alien invasion!", DESIGN_WIDTH / 2 - 200, DESIGN_HEIGHT / 2 - 60, 100, 255, 100);
                renderText(renderer, "Final Score: " + std::to_string(score), DESIGN_WIDTH / 2 - 120, DESIGN_HEIGHT / 2 - 20, 255, 255, 0, 1.5f);
                renderText(renderer, "Press R to Play Again", DESIGN_WIDTH / 2 - 100, DESIGN_HEIGHT / 2 + 40, 0, 255, 255);
                renderText(renderer, "Press ESC to Exit", DESIGN_WIDTH / 2 - 80, DESIGN_HEIGHT / 2 + 70, 150, 150, 150);
            }
            
            SDL_RenderPresent(renderer);
            SDL_Delay(16);
            continue;
        }

        // Update timers
        shootTimer += dt;
        superShotTimer += dt;
        dashTimer += dt;
        alienMoveTimer += dt;
        
        // Update status message timer
        if (statusMessageTimer > 0) {
            statusMessageTimer--;
        }
        
        if (isDashing) {
            dashDuration -= dt;
            if (dashDuration <= 0) isDashing = false;
        }

        // Player movement
        float moveSpeed = isDashing ? PLAYER_DASH_SPEED : PLAYER_SPEED;
        
        if (Input::isKeyPressed(SDL_SCANCODE_A) || Input::isKeyPressed(SDL_SCANCODE_LEFT)) {
            playerX -= moveSpeed * dt;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_D) || Input::isKeyPressed(SDL_SCANCODE_RIGHT)) {
            playerX += moveSpeed * dt;
        }
        
        // Clamp player position
        if (playerX < 0) playerX = 0;
        if (playerX + PLAYER_SIZE > DESIGN_WIDTH) playerX = DESIGN_WIDTH - PLAYER_SIZE;

        // ENGINE FEATURE: Input Chords (Section 2 requirement)
        // Dash chords
        if (Input::isChordJustPressed("dash_left") && dashTimer >= DASH_COOLDOWN) {
            isDashing = true;
            dashDuration = 0.3f;
            dashTimer = 0.f;
            playerX -= 100.f; // Instant dash
            if (playerX < 0) playerX = 0;
            
            statusMessage = "Dash Left!";
            statusMessageTimer = 30;
            
            // Raise chord event
            eventManager.raiseEvent(Engine::Events::InputChord("dash_left", &gameTimeline));
        }
        if (Input::isChordJustPressed("dash_right") && dashTimer >= DASH_COOLDOWN) {
            isDashing = true;
            dashDuration = 0.3f;
            dashTimer = 0.f;
            playerX += 100.f;
            if (playerX + PLAYER_SIZE > DESIGN_WIDTH) playerX = DESIGN_WIDTH - PLAYER_SIZE;
            
            statusMessage = "Dash Right!";
            statusMessageTimer = 30;
            
            eventManager.raiseEvent(Engine::Events::InputChord("dash_right", &gameTimeline));
        }

        // Super shot chord
        if (Input::isChordJustPressed("super_shot") && superShotTimer >= SUPER_SHOT_COOLDOWN) {
            std::string bulletId = "bullet_super_" + std::to_string(gameTimeline.time());
            auto& bullet = bulletRegistry.upsert(bulletId);
            bullet.set<float>("x", playerX + PLAYER_SIZE / 2.f - BULLET_WIDTH / 2.f);
            bullet.set<float>("y", playerY);
            bullet.set<float>("vy", -BULLET_SPEED * 1.5f);
            bullet.set<int>("type", static_cast<int>(BulletType::PlayerSuper));
            bullet.set<bool>("active", true);
            
            superShotTimer = 0.f;
            statusMessage = "SUPER SHOT!";
            statusMessageTimer = 60;
            eventManager.raiseEvent(Engine::Events::InputChord("super_shot", &gameTimeline));
            SDL_Log("Super shot fired!");
        }
        // Normal shooting
        else if (Input::isKeyPressed(SDL_SCANCODE_SPACE) && shootTimer >= SHOOT_COOLDOWN) {
            std::string bulletId = "bullet_player_" + std::to_string(gameTimeline.time());
            auto& bullet = bulletRegistry.upsert(bulletId);
            bullet.set<float>("x", playerX + PLAYER_SIZE / 2.f - BULLET_WIDTH / 2.f);
            bullet.set<float>("y", playerY);
            bullet.set<float>("vy", -BULLET_SPEED);
            bullet.set<int>("type", static_cast<int>(BulletType::PlayerNormal));
            bullet.set<bool>("active", true);
            
            shootTimer = 0.f;
        }

        // Update bullets
        for (const auto& id : bulletRegistry.getAllIds()) {
            auto* bullet = bulletRegistry.get(id);
            if (!bullet || !bullet->get<bool>("active", false)) continue;
            
            float x = bullet->get<float>("x", 0.f);
            float y = bullet->get<float>("y", 0.f);
            float vy = bullet->get<float>("vy", 0.f);
            
            y += vy * dt;
            bullet->set<float>("y", y);
            
            // Remove off-screen bullets
            if (y < -BULLET_HEIGHT || y > DESIGN_HEIGHT) {
                bullet->set<bool>("active", false);
            }
        }

        // Move aliens
        if (alienMoveTimer >= ALIEN_MOVE_INTERVAL) {
            alienMoveTimer = 0.f;
            
            bool needToDescend = false;
            float leftmost = DESIGN_WIDTH;
            float rightmost = 0;
            
            // Find grid bounds
            for (const auto& id : alienRegistry.getAllIds()) {
                auto* alien = alienRegistry.get(id);
                if (!alien || !alien->get<bool>("active", false)) continue;
                
                float x = alien->get<float>("x", 0.f);
                if (x < leftmost) leftmost = x;
                if (x + ALIEN_SIZE > rightmost) rightmost = x + ALIEN_SIZE;
            }
            
            // Check if need to change direction
            if ((alienDirection == 1 && rightmost >= DESIGN_WIDTH - 10) ||
                (alienDirection == -1 && leftmost <= 10)) {
                needToDescend = true;
                alienDirection *= -1;
            }
            
            // Move all aliens
            for (const auto& id : alienRegistry.getAllIds()) {
                auto* alien = alienRegistry.get(id);
                if (!alien || !alien->get<bool>("active", false)) continue;
                
                float x = alien->get<float>("x", 0.f);
                float y = alien->get<float>("y", 0.f);
                
                if (needToDescend) {
                    y += ALIEN_DROP_DISTANCE;
                    
                    // Check if aliens reached bottom
                    if (y + ALIEN_SIZE >= playerY) {
                        gameOver = true;
                        eventManager.raiseEvent(Engine::Events::Death("player", &gameTimeline));
                    }
                } else {
                    x += alienDirection * ALIEN_MOVE_SPEED;
                }
                
                alien->set<float>("x", x);
                alien->set<float>("y", y);
            }
        }

        // Aliens shoot randomly
        for (const auto& id : alienRegistry.getAllIds()) {
            auto* alien = alienRegistry.get(id);
            if (!alien || !alien->get<bool>("active", false)) continue;
            
            if (shootChance(gen) < 0.001) { // Low probability per frame
                float x = alien->get<float>("x", 0.f);
                float y = alien->get<float>("y", 0.f);
                
                std::string bulletId = "bullet_enemy_" + std::to_string(gameTimeline.time()) + "_" + id;
                auto& bullet = bulletRegistry.upsert(bulletId);
                bullet.set<float>("x", x + ALIEN_SIZE / 2.f - BULLET_WIDTH / 2.f);
                bullet.set<float>("y", y + ALIEN_SIZE);
                bullet.set<float>("vy", ENEMY_BULLET_SPEED);
                bullet.set<int>("type", static_cast<int>(BulletType::Enemy));
                bullet.set<bool>("active", true);
            }
        }

        // Collision detection - Player bullets vs Aliens
        for (const auto& bulletId : bulletRegistry.getAllIds()) {
            auto* bullet = bulletRegistry.get(bulletId);
            if (!bullet || !bullet->get<bool>("active", false)) continue;
            
            int bulletType = bullet->get<int>("type", 0);
            if (bulletType == static_cast<int>(BulletType::Enemy)) continue;
            
            float bx = bullet->get<float>("x", 0.f);
            float by = bullet->get<float>("y", 0.f);
            SDL_FRect bulletRect = {bx, by, BULLET_WIDTH, BULLET_HEIGHT};
            
            bool bulletDestroyed = false;
            
            for (const auto& alienId : alienRegistry.getAllIds()) {
                auto* alien = alienRegistry.get(alienId);
                if (!alien || !alien->get<bool>("active", false)) continue;
                
                float ax = alien->get<float>("x", 0.f);
                float ay = alien->get<float>("y", 0.f);
                SDL_FRect alienRect = {ax, ay, ALIEN_SIZE, ALIEN_SIZE};
                
                if (aabbIntersect(bulletRect, alienRect)) {
                    // Hit!
                    alien->set<bool>("active", false);
                    aliensRemaining--;
                    
                    int alienType = alien->get<int>("type", 0);
                    int points = (alienType == 0) ? 30 : (alienType == 1) ? 20 : 10;
                    score += points;
                    
                    // Raise events
                    eventManager.raiseEvent(Engine::Events::Collision(bulletId, alienId, &gameTimeline));
                    eventManager.raiseEvent(Engine::Events::Death(alienId, &gameTimeline));
                    
                    // Super shot pierces, normal bullets don't
                    if (bulletType == static_cast<int>(BulletType::PlayerNormal)) {
                        bullet->set<bool>("active", false);
                        bulletDestroyed = true;
                        break;
                    }
                }
            }
        }

        // Collision detection - Enemy bullets vs Player
        for (const auto& bulletId : bulletRegistry.getAllIds()) {
            auto* bullet = bulletRegistry.get(bulletId);
            if (!bullet || !bullet->get<bool>("active", false)) continue;
            
            int bulletType = bullet->get<int>("type", 0);
            if (bulletType != static_cast<int>(BulletType::Enemy)) continue;
            
            float bx = bullet->get<float>("x", 0.f);
            float by = bullet->get<float>("y", 0.f);
            SDL_FRect bulletRect = {bx, by, BULLET_WIDTH, BULLET_HEIGHT};
            SDL_FRect playerRect = {playerX, playerY, PLAYER_SIZE, PLAYER_SIZE};
            
            if (aabbIntersect(bulletRect, playerRect)) {
                bullet->set<bool>("active", false);
                playerLives--;
                
                eventManager.raiseEvent(Engine::Events::Collision("player", bulletId, &gameTimeline));
                
                if (playerLives <= 0) {
                    gameOver = true;
                    eventManager.raiseEvent(Engine::Events::Death("player", &gameTimeline));
                } else {
                    statusMessage = "Hit! Lives: " + std::to_string(playerLives);
                    statusMessageTimer = 90;
                }
            }
        }

        // Check win condition
        if (aliensRemaining == 0) {
            gameWon = true;
        }

        // ENGINE FEATURE: Dispatch events
        eventManager.dispatchEvents();

        // Render
        SDL_SetRenderDrawColor(renderer, 10, 10, 30, 255);
        SDL_RenderClear(renderer);

        // Background - stretch to fill entire design space (single large quad)
        background.setPosition(0, 0);
        background.setSize(DESIGN_WIDTH, DESIGN_HEIGHT);
        background.render(renderer, window);

        // Player
        playerShip.setPosition(playerX, playerY);
        playerShip.setSize(PLAYER_SIZE, PLAYER_SIZE);
        playerShip.render(renderer, window);

        // Aliens
        for (const auto& id : alienRegistry.getAllIds()) {
            auto* alien = alienRegistry.get(id);
            if (!alien || !alien->get<bool>("active", false)) continue;
            
            float x = alien->get<float>("x", 0.f);
            float y = alien->get<float>("y", 0.f);
            int type = alien->get<int>("type", 0);
            
            Entity* alienSprite = (type == 0) ? &alienBlack : (type == 1) ? &alienBlue : &alienGreen;
            alienSprite->setPosition(x, y);
            alienSprite->setSize(ALIEN_SIZE, ALIEN_SIZE);
            alienSprite->render(renderer, window);
        }

        // Bullets
        for (const auto& id : bulletRegistry.getAllIds()) {
            auto* bullet = bulletRegistry.get(id);
            if (!bullet || !bullet->get<bool>("active", false)) continue;
            
            float x = bullet->get<float>("x", 0.f);
            float y = bullet->get<float>("y", 0.f);
            int type = bullet->get<int>("type", 0);
            
            Entity* laser = (type == static_cast<int>(BulletType::Enemy)) ? &laserRed : &laserBlue;
            laser->setPosition(x, y);
            laser->setSize(BULLET_WIDTH, BULLET_HEIGHT);
            laser->render(renderer, window);
        }

        // UI
        renderText(renderer, "Score: " + std::to_string(score), 20, 20, 255, 255, 0);
        renderText(renderer, "Lives: " + std::to_string(playerLives), 20, 40, 255, 100, 100);
        renderText(renderer, "Aliens: " + std::to_string(aliensRemaining), 20, 60, 100, 255, 100);
        
        renderText(renderer, "Controls: A/D - Move | Space - Shoot", 20, DESIGN_HEIGHT - 80, 200, 200, 200);
        renderText(renderer, "Chords: Shift+Space - Super Shot | Ctrl+A/D - Dash", 20, DESIGN_HEIGHT - 60, 255, 200, 100);
        renderText(renderer, "P - Pause | 1/2/3 - Speed | S - Scaling", 20, DESIGN_HEIGHT - 40, 200, 200, 200);
        
        std::string scaleText = "Speed: " + std::to_string(int(gameTimeline.scale() * 100)) + "%";
        renderText(renderer, scaleText, 20, DESIGN_HEIGHT - 20, 0, 255, 0);
        
        // Cooldown indicators
        if (superShotTimer < SUPER_SHOT_COOLDOWN) {
            float pct = superShotTimer / SUPER_SHOT_COOLDOWN;
            std::string cooldown = "Super: " + std::to_string((int)((1.0f - pct) * SUPER_SHOT_COOLDOWN * 10) / 10.0f) + "s";
            renderText(renderer, cooldown, DESIGN_WIDTH - 150, 20, 255, 150, 0);
        } else {
            renderText(renderer, "Super: READY", DESIGN_WIDTH - 150, 20, 0, 255, 0);
        }
        
        if (dashTimer < DASH_COOLDOWN) {
            float pct = dashTimer / DASH_COOLDOWN;
            std::string cooldown = "Dash: " + std::to_string((int)((1.0f - pct) * DASH_COOLDOWN * 10) / 10.0f) + "s";
            renderText(renderer, cooldown, DESIGN_WIDTH - 150, 40, 255, 150, 0);
        } else {
            renderText(renderer, "Dash: READY", DESIGN_WIDTH - 150, 40, 0, 255, 0);
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
