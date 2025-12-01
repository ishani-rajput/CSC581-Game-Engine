#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <iostream>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <string>
#include <variant>

#include "scaling.h"
#include "timeline.h"
#include "input.h"
#include "registry.h"
#include "event_manager.h"
#include "memory_pool.h"
#include "collision.h"
#include "physics.h"   

const int WINDOW_WIDTH = 800;
const int WINDOW_HEIGHT = 900;

const float BUBBLE_RADIUS = 20.f;
const float GUN_X = WINDOW_WIDTH / 2.f;
const float GUN_Y = WINDOW_HEIGHT - 60.f;
const float GUN_SIZE = 15.f;

enum class BubbleColor {
    Red = 0,
    Blue = 1,
    Green = 2,
    Count = 3
};

struct Bubble {
    float x, y;
    float vx, vy;
    BubbleColor color;
    bool active;
    bool markedForRemoval;
    std::string id;   

    Bubble()
        : x(0), y(0), vx(0), vy(0), color(BubbleColor::Red),
          active(false), markedForRemoval(false), id() {}

    Bubble(float px, float py, BubbleColor c)
        : x(px), y(py), vx(0), vy(0), color(c),
          active(true), markedForRemoval(false), id() {}

    SDL_FRect getRect() const {
        return SDL_FRect{x - BUBBLE_RADIUS, y - BUBBLE_RADIUS,
                         BUBBLE_RADIUS * 2, BUBBLE_RADIUS * 2};
    }

    bool contains(float px, float py) const {
        float dx = x - px;
        float dy = y - py;
        float dist = std::sqrt(dx * dx + dy * dy);
        return dist <= BUBBLE_RADIUS;
    }

    bool intersects(const Bubble& other) const {
        float dx = x - other.x;
        float dy = y - other.y;
        float dist = std::sqrt(dx * dx + dy * dy);
        return dist < (BUBBLE_RADIUS * 2.6f);  
    }
    
    bool isConnected(const Bubble& other) const {
        float dx = x - other.x;
        float dy = y - other.y;
        float dist = std::sqrt(dx * dx + dy * dy);
        return dist < (BUBBLE_RADIUS * 2.8f);
    }
};

struct Projectile {
    float x, y;
    float vx, vy;
    bool active;
    BubbleColor color;
    std::string id;    

    Projectile()
        : x(0), y(0), vx(0), vy(0), active(false),
          color(BubbleColor::Red), id() {}

    Projectile(float px, float py, float velx, float vely, BubbleColor c)
        : x(px), y(py), vx(velx), vy(vely), active(true),
          color(c), id() {}

    SDL_FRect getRect() const {
        return SDL_FRect{x - 5.f, y - 5.f, 10.f, 10.f};
    }
};

class BubbleShooterGame {
public:
    BubbleShooterGame(Engine::Registry* reg, Engine::EventManager* em, Timeline* tl)
        : registry(reg),
          eventManager(em),
          timeline(tl),
          bubbles(),
          projectiles(),
          gunAngle(0.f),  
          score(0),
          gameOver(false),
          bubbleDropTimer(0),
          bubbleDropInterval(10.0f),  
          matchTimer(0),
          matchDuration(0.1f),
          nextBubbleColor(BubbleColor::Red),
          bubbleIdCounter(0),
          projectileIdCounter(0),             
          fastLaunchRequested(false),
          fastLaunchAngle(0.0f),
          gameWon(false) {
        initializeBubbles();
        pickRandomNextBubbleColor();

        eventManager->registerListener(
            Engine::EventType::Death,
            [this](const Engine::Event& ev) {
                auto it = ev.payload.find("score");
                if (it != ev.payload.end()) {
                    if (std::holds_alternative<int>(it->second)) {
                        score += std::get<int>(it->second);
                    }
                }
            }
        );

        std::cout << "Constructor: initialized " << bubbles.size() << " bubbles" << std::endl;
    }

    void setInput(float rotateDir, bool fire, bool fastLeft = false, bool fastRight = false) {
        std::lock_guard<std::mutex> lk(m_);
        desiredRotation = rotateDir;
        if (fire) {
            wantFire = true;
        }
        if (fastLeft) {
            fastLaunchRequested = true;
            fastLaunchAngle = -135.0f;
        }
        if (fastRight) {
            fastLaunchRequested = true;
            fastLaunchAngle = -45.0f;
        }
    }

    void getGameState(std::vector<Bubble>& outBubbles,
                      std::vector<Projectile>& outProjectiles,
                      float& outGunAngle,
                      int& outScore,
                      bool& outGameOver,
                      BubbleColor& outNextColor,
                      bool& outGameWon,
                      float& outDescendOffset) const {
        std::lock_guard<std::mutex> lk(m_);
        outBubbles = bubbles;
        outProjectiles = projectiles;
        outGunAngle = gunAngle;
        outScore = score;
        outGameOver = gameOver;
        outNextColor = nextBubbleColor;
        outGameWon = gameWon;
        outDescendOffset = descendOffset;
    }

    void step(float dt) {
        std::lock_guard<std::mutex> lk(m_);

        if (gameOver) return;

        gunAngle += desiredRotation * 180.f * dt; 
        if (gunAngle < -90.f) gunAngle = -90.f;
        if (gunAngle > 90.f)  gunAngle = 90.f;

        if (fastLaunchRequested) {
            fireProjectileAtAngle(fastLaunchAngle);
            fastLaunchRequested = false;
            std::cout << "Fast launch at " << fastLaunchAngle << "°!" << std::endl;
        } else if (wantFire) {
            fireProjectile();
            wantFire = false;
        }

        updateProjectiles(dt);
        checkCollisions();
        updateBubbles(dt);

        bubbleDropTimer += dt;
        if (bubbleDropTimer >= bubbleDropInterval) {
            bubbleDropTimer = 0.0f;
            dropBubbleLayer();
        }

        checkGameConditions();
        
        eventManager->dispatchEvents();
    }

private:
    enum class GameState { Playing, Won, Lost };

    std::pair<float,float> snapToGrid(float px, float py) const {
        const int numCols = 8;
        const float spacingX = BUBBLE_RADIUS * 2 + 8;
        const float spacingY = BUBBLE_RADIUS * 2 + 8;
        const float gridWidth = numCols * spacingX;
        const float startX = (WINDOW_WIDTH - gridWidth) / 2 + BUBBLE_RADIUS;
        const float startY = 40.f + BUBBLE_RADIUS;

        int row = (int)std::round((py - startY) / spacingY);
        if (row < 0) row = 0;
        float rowOffsetX = (row % 2 == 1) ? spacingX / 2.f : 0.f;
        int col = (int)std::round((px - (startX + rowOffsetX)) / spacingX);
        if (col < 0) col = 0;

        float snappedX = startX + rowOffsetX + col * spacingX;
        float snappedY = startY + row * spacingY;
        return {snappedX, snappedY};
    }

    std::pair<int,int> toGridRC(float px, float py) const {
        const int numCols = 8;
        const float spacingX = BUBBLE_RADIUS * 2.5f;
        const float spacingY = BUBBLE_RADIUS * 2.2f;
        const float gridWidth = numCols * spacingX;
        const float startX = (WINDOW_WIDTH - gridWidth) / 2 + BUBBLE_RADIUS;
        const float startY = 40.f + BUBBLE_RADIUS;

        int row = (int)std::round((py - startY) / spacingY);
        if (row < 0) row = 0;
        float rowOffsetX = (row % 2 == 1) ? spacingX / 2.f : 0.f;
        int col = (int)std::round((px - (startX + rowOffsetX)) / spacingX);
        if (col < 0) col = 0;
        return {row, col};
    }

    void initializeBubbles() {
        bubbles.clear();

        const int numRows = 3;
        const int numCols = 8;
        const float spacingX = BUBBLE_RADIUS * 2.5f;
        const float spacingY = BUBBLE_RADIUS * 2.2f;
        const float gridWidth = numCols * spacingX;
        const float startX = (WINDOW_WIDTH - gridWidth) / 2 + BUBBLE_RADIUS;
        const float startY = 40.f + BUBBLE_RADIUS;

        for (int row = 0; row < numRows; ++row) {
            for (int col = 0; col < numCols; ++col) {
                float x = startX + col * spacingX;
                float y = startY + row * spacingY;
                if (row % 2 == 1) x += spacingX / 2.f;
                BubbleColor color = static_cast<BubbleColor>((row * 2 + col) % static_cast<int>(BubbleColor::Count));
                
                std::string id = "bubble_" + std::to_string(bubbleIdCounter++);
                
                bubbles.emplace_back(x, y, color);
                Bubble& bubble = bubbles.back();
                bubble.id = id; 

                auto rc = toGridRC(x, y);
                if (rc.first >= 0 && rc.first < GRID_ROWS && rc.second >= 0 && rc.second < GRID_COLS) {
                    grid[rc.first][rc.second] = true;
                }
                
                auto& gameObj = registry->upsert(id);
                gameObj.set("x", bubble.x);
                gameObj.set("y", bubble.y);
                gameObj.set("color", static_cast<int>(bubble.color));
                gameObj.set("active", true);
                gameObj.set("gridRow", rc.first);
                gameObj.set("gridCol", rc.second);
            }
        }
    }

    void fireProjectileAtAngle(float angle) {
        float radians = (angle * 3.14159f / 180.f);
        const float speed = 800.f * 1.5f;
        float velx = speed * std::sin(radians);
        float vely = -speed * std::cos(radians);

        std::string id = "projectile_" + std::to_string(projectileIdCounter++);

        projectiles.emplace_back(GUN_X, GUN_Y, velx, vely, nextBubbleColor);
        Projectile& proj = projectiles.back();
        proj.id = id;

        auto& gameObj = registry->upsert(id); 
        gameObj.set("x", proj.x);
        gameObj.set("y", proj.y);
        gameObj.set("vx", proj.vx);
        gameObj.set("vy", proj.vy);
        gameObj.set("color", static_cast<int>(proj.color));
        gameObj.set("active", true);

        pickRandomNextBubbleColor();
    }

    void pickRandomNextBubbleColor() {
        nextBubbleColor = static_cast<BubbleColor>(rand() % static_cast<int>(BubbleColor::Count));
    }

    void updateProjectiles(float dt) {
        float currentTopBoundary = 40.f + BUBBLE_RADIUS + descendOffset;
        for (auto& proj : projectiles) {
            if (!proj.active) continue;

            Body body;
            body.vx = proj.vx;
            body.vy = proj.vy;
            body.affectedByGravity = false;
            Physics::step(dt * 1000.0f, proj.x, proj.y, body);
            proj.vx = body.vx;
            proj.vy = body.vy;

            if (proj.x - BUBBLE_RADIUS < 0 || proj.x + BUBBLE_RADIUS > WINDOW_WIDTH) {
                proj.vx = -proj.vx;
                proj.x = std::clamp(proj.x, BUBBLE_RADIUS, (float)WINDOW_WIDTH - BUBBLE_RADIUS);
            }

            if (proj.y < currentTopBoundary) {
                proj.y = currentTopBoundary;
                proj.vy = 0;
                proj.active = false;
            }

            if (proj.y > WINDOW_HEIGHT) {
                proj.active = false;
            }

            if (!proj.id.empty()) {
                auto* obj = registry->get(proj.id);
                if (obj) {
                    obj->set("x", proj.x);
                    obj->set("y", proj.y);
                    obj->set("vx", proj.vx);
                    obj->set("vy", proj.vy);
                    obj->set("active", proj.active);
                }
            }
        }

        projectiles.erase(
            std::remove_if(projectiles.begin(), projectiles.end(),
                           [this](const Projectile& p) {
                               if (!p.active) {
                                   if (!p.id.empty()) {
                                       registry->erase(p.id);
                                   }
                                   return true;
                               }
                               return false;
                           }),
            projectiles.end());
    }

    void fireProjectile() {
        float radians = (gunAngle * 3.14159f / 180.f);
        const float speed = 800.f;
        float velx = speed * std::sin(radians);
        float vely = -speed * std::cos(radians);

        std::string id = "projectile_" + std::to_string(projectileIdCounter++); 

        projectiles.emplace_back(GUN_X, GUN_Y, velx, vely, nextBubbleColor);
        Projectile& proj = projectiles.back();
        proj.id = id; 

        auto& gameObj = registry->upsert(id);  
        gameObj.set("x", proj.x);
        gameObj.set("y", proj.y);
        gameObj.set("vx", proj.vx);
        gameObj.set("vy", proj.vy);
        gameObj.set("color", static_cast<int>(proj.color));
        gameObj.set("active", true);

        std::cout << "Fired projectile with color: " << (int)nextBubbleColor << std::endl;
        pickRandomNextBubbleColor();
        std::cout << "Next bubble color will be: " << (int)nextBubbleColor << std::endl;
    }

    void checkCollisions() {
        static int callCount = 0;
        if (callCount % 60 == 0) {  
            std::cout << "checkCollisions called, projectiles: " << projectiles.size() 
                      << ", bubbles: " << bubbles.size() << std::endl;
        }
        callCount++;
        
        float currentTopBoundary = 40.f + BUBBLE_RADIUS + descendOffset;
        
        for (auto& proj : projectiles) {
            if (!proj.active) continue;

            bool hitBubble = false;
            SDL_FRect projRect = proj.getRect();
            for (size_t i = 0; i < bubbles.size(); ++i) {
                Bubble& bubble = bubbles[i];
                if (!bubble.active) continue;

                SDL_FRect bubbleRect = bubble.getRect();
                if (aabbIntersect(bubbleRect, projRect)) {
                    hitBubble = true;
                    break;
                }
            }

            if (!hitBubble && proj.y > currentTopBoundary) {
                continue;
            }

            if (hitBubble || proj.y <= currentTopBoundary) {
                proj.active = false;
                
                std::cout << "Collision detected at (" << proj.x << ", " << proj.y 
                          << ")! Projectile color: " << (int)proj.color << std::endl;
                
                auto [row, col] = toGridRC(proj.x, proj.y);
                
                if (row >= GRID_ROWS) row = GRID_ROWS - 1;
                if (row < 0)         row = 0;
                if (col < 0)         col = 0;
                if (col >= GRID_COLS) col = GRID_COLS - 1;

                while (grid[row][col] && row > 0) {
                    row--;
                }

                if (grid[row][col]) {
                    std::cout << "No space available, bubble lost" << std::endl;
                    continue;
                }

                const float spacingX = BUBBLE_RADIUS * 2.5f;
                const float spacingY = BUBBLE_RADIUS * 2.2f; 
                const float gridWidth = 8 * spacingX;
                const float startX = (WINDOW_WIDTH - gridWidth) / 2 + BUBBLE_RADIUS;
                const float startY = 40.f + BUBBLE_RADIUS;

                float offsetX = (row % 2 == 1) ? spacingX / 2.f : 0.f;
                float newX = startX + offsetX + col * spacingX;
                float newY = startY + row * spacingY;

                bubbles.emplace_back(newX, newY, proj.color);
                size_t newBubbleIdx = bubbles.size() - 1;
                Bubble& newBubble = bubbles.back();

                std::string id = "bubble_" + std::to_string(bubbleIdCounter++);
                newBubble.id = id;

                auto& gameObj = registry->upsert(id);
                gameObj.set("x", newBubble.x);
                gameObj.set("y", newBubble.y);
                gameObj.set("color", static_cast<int>(newBubble.color));
                gameObj.set("active", true);
                gameObj.set("gridRow", row);
                gameObj.set("gridCol", col);
                
                std::cout << "Added new bubble at (" << newX << ", " << newY 
                          << ") grid[" << row << "][" << col << "] with color " << (int)proj.color << std::endl;
                
                grid[row][col] = true;

                popMatchingBubbles(newBubbleIdx);
            }
        }

        int removedCount = 0;
        auto newEnd = std::remove_if(
            bubbles.begin(), bubbles.end(),
            [this, &removedCount](const Bubble& b) {
                if (b.markedForRemoval) {
                    removedCount++;
                    if (!b.id.empty()) {
                        registry->erase(b.id);
                    }
                    return true;
                }
                return false;
            }
        );
        bubbles.erase(newEnd, bubbles.end());
        
        if (removedCount > 0) {
            std::cout << "Removed " << removedCount << " bubbles from vector" << std::endl;
            for (int r = 0; r < GRID_ROWS; ++r)
                for (int c = 0; c < GRID_COLS; ++c)
                    grid[r][c] = false;
            for (const auto& b : bubbles) {
                if (!b.active) continue;
                auto rc = toGridRC(b.x, b.y);
                if (rc.first >= 0 && rc.first < GRID_ROWS &&
                    rc.second >= 0 && rc.second < GRID_COLS) {
                    grid[rc.first][rc.second] = true;
                }
            }
        }
    }

    void popMatchingBubbles(size_t hitBubbleIndex) {
        std::vector<bool> visited(bubbles.size(), false);
        std::vector<size_t> queue;
        std::vector<size_t> toRemove;

        const float matchDist = BUBBLE_RADIUS * 2.6f;

        queue.push_back(hitBubbleIndex);
        visited[hitBubbleIndex] = true;

        size_t qIndex = 0;
        while (qIndex < queue.size()) {
            size_t current = queue[qIndex++];
            Bubble& b = bubbles[current];
            toRemove.push_back(current);

            for (size_t i = 0; i < bubbles.size(); ++i) {
                if (visited[i] || !bubbles[i].active) continue;
                if (bubbles[i].color != b.color) continue;

                float dx = bubbles[i].x - b.x;
                float dy = bubbles[i].y - b.y;
                float dist = std::sqrt(dx*dx + dy*dy);

                if (dist < matchDist) {
                    visited[i] = true;
                    queue.push_back(i);
                }
            }
        }

        std::cout << "Found " << toRemove.size() << " connected bubbles of color " 
                  << (int)bubbles[hitBubbleIndex].color << std::endl;

        if (toRemove.size() >= 3) {
            std::cout << "Removing " << toRemove.size() << " bubbles!" << std::endl;
            for (size_t idx : toRemove) {
                bubbles[idx].markedForRemoval = true;
                
                const std::string& bubbleId = bubbles[idx].id;
                if (!bubbleId.empty()) {
                    Engine::Event deathEvent = Engine::Events::Death(bubbleId, timeline);
                    deathEvent.payload["reason"] = std::string("matched");
                    deathEvent.payload["score"]  = 10; 
                    eventManager->raiseEvent(deathEvent);
                }
            }
            
            removeFloatingBubbles();
        } else {
            std::cout << "Not enough matches (need 3+), only found " << toRemove.size() << std::endl;
        }
    }

    void removeFloatingBubbles() {
        int totalActive = 0;
        int alreadyMarked = 0;
        for (const auto& bubble : bubbles) {
            if (bubble.active) totalActive++;
            if (bubble.markedForRemoval) alreadyMarked++;
        }
        std::cout << "removeFloatingBubbles: total active=" << totalActive 
                  << ", already marked=" << alreadyMarked << std::endl;
        
        for (auto& bubble : bubbles) {
            bubble.vx = 0.0f;
        }
        
        float minY = 999999.0f;
        for (size_t i = 0; i < bubbles.size(); ++i) {
            if (!bubbles[i].markedForRemoval && bubbles[i].active) {
                if (bubbles[i].y < minY) minY = bubbles[i].y;
            }
        }
        
        std::cout << "Min Y found: " << minY << std::endl;
        
        std::vector<size_t> queue;
        int topRowCount = 0;
        for (size_t i = 0; i < bubbles.size(); ++i) {
            if (bubbles[i].markedForRemoval) continue;
            
            if (bubbles[i].active && bubbles[i].y < minY + 60.0f) {
                queue.push_back(i);
                bubbles[i].vx = 1.0f;
                topRowCount++;
            }
        }
        
        std::cout << "Starting BFS from " << topRowCount << " bubbles in top row" << std::endl;
        
        size_t qIndex = 0;
        while (qIndex < queue.size()) {
            size_t current = queue[qIndex++];
            const Bubble& b = bubbles[current];
            
            for (size_t i = 0; i < bubbles.size(); ++i) {
                if (bubbles[i].markedForRemoval || !bubbles[i].active || bubbles[i].vx == 1.0f) continue;
                
                if (bubbles[i].isConnected(b)) {
                    bubbles[i].vx = 1.0f;
                    queue.push_back(i);
                }
            }
        }
        
        std::cout << "BFS reached " << queue.size() << " total bubbles" << std::endl;
        
        int floatingCount = 0;
        for (size_t i = 0; i < bubbles.size(); ++i) {
            if (bubbles[i].markedForRemoval) continue;
            
            if (bubbles[i].active && bubbles[i].vx != 1.0f) {
                bubbles[i].markedForRemoval = true;
                floatingCount++;
                
                const std::string& bubbleId = bubbles[i].id;
                if (!bubbleId.empty()) {
                    Engine::Event deathEvent = Engine::Events::Death(bubbleId, timeline);
                    deathEvent.payload["reason"] = std::string("floating");
                    deathEvent.payload["score"]  = 5;
                    eventManager->raiseEvent(deathEvent);
                }
            }
        }
        
        for (auto& bubble : bubbles) {
            bubble.vx = 0.0f;
        }
        
        if (floatingCount > 0) {
            std::cout << "Removed " << floatingCount << " floating bubbles!" << std::endl;
        }
    }

    void updateBubbles(float /*dt*/) {
        for (auto& bubble : bubbles) {
            if (!bubble.active) continue;
        }
    }

    void dropBubbleLayer() {
        const float dropDistance = 45.f;

        for (auto& bubble : bubbles) {
            if (bubble.active) {
                bubble.y += dropDistance;

                if (!bubble.id.empty()) {
                    auto* obj = registry->get(bubble.id);
                    if (obj) {
                        obj->set("y", bubble.y);
                    }
                }
            }
        }

        descendOffset += dropDistance;
        bubbleDropInterval *= 0.98f;  
    }

    void checkGameConditions() {
        const float loseHeight = WINDOW_HEIGHT - 100.f;
        for (const auto& bubble : bubbles) {
            if (bubble.active && bubble.y + BUBBLE_RADIUS >= loseHeight) {
                gameOver = true;
                gameWon = false;
                std::cout << "Game Over! Bubbles reached bottom." << std::endl;
                return;
            }
        }

        bool allCleared = true;
        for (const auto& bubble : bubbles) {
            if (bubble.active) {
                allCleared = false;
                break;
            }
        }

        if (allCleared) {
            gameOver = true;
            gameWon = true;
            std::cout << "You win! All bubbles cleared." << std::endl;
            return;
        }
    }

    mutable std::mutex m_;
    std::vector<Bubble> bubbles;
    std::vector<Projectile> projectiles;

    float gunAngle;
    float desiredRotation{0};
    bool wantFire{false};
    bool fastLaunchRequested{false};
    float fastLaunchAngle{0.0f};

    int score;
    bool gameOver;
    bool gameWon;
    float bubbleDropTimer;
    float bubbleDropInterval;
    float matchTimer;
    float matchDuration;
    BubbleColor nextBubbleColor;
    
    Engine::Registry* registry;
    Engine::EventManager* eventManager;
    Timeline* timeline;
    int bubbleIdCounter;
    int projectileIdCounter; 
    
    static constexpr int GRID_ROWS = 20;
    static constexpr int GRID_COLS = 15;
    bool grid[GRID_ROWS][GRID_COLS] = {{false}};
    float descendOffset = 0.0f;
};

static void gameLoop(std::atomic<bool>& running, BubbleShooterGame& game,
                      Timeline& time) {
    while (running) {
        float dt = (float)time.tick();
        game.step(dt);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

static void drawBubble(SDL_Renderer* renderer, float x, float y,
                       BubbleColor color) {
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

    switch (color) {
    case BubbleColor::Red:
        SDL_SetRenderDrawColor(renderer, 255, 50, 50, 255);
        break;
    case BubbleColor::Blue:
        SDL_SetRenderDrawColor(renderer, 50, 100, 255, 255);
        break;
    case BubbleColor::Green:
        SDL_SetRenderDrawColor(renderer, 50, 255, 100, 255);
        break;
    default:
        SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
    }

    int radius = (int)BUBBLE_RADIUS;
    for (int dy = -radius; dy <= radius; ++dy) {
        int dx = (int)std::sqrt(radius * radius - dy * dy);
        SDL_RenderLine(renderer, x - dx, y + dy, x + dx, y + dy);
    }
}

static void drawCircle(SDL_Renderer* renderer, float cx, float cy, int radius, bool filled) {
    int x = 0;
    int y = radius;
    int d = 3 - 2 * radius;

    while (x <= y) {
        if (filled) {
            SDL_RenderLine(renderer, cx - x, cy - y, cx + x, cy - y);
            SDL_RenderLine(renderer, cx - x, cy + y, cx + x, cy + y);
            SDL_RenderLine(renderer, cx - y, cy - x, cx + y, cy - x);
            SDL_RenderLine(renderer, cx - y, cy + x, cx + y, cy + x);
        } else {
            SDL_RenderPoint(renderer, cx + x, cy - y);
            SDL_RenderPoint(renderer, cx - x, cy - y);
            SDL_RenderPoint(renderer, cx + x, cy + y);
            SDL_RenderPoint(renderer, cx - x, cy + y);
            SDL_RenderPoint(renderer, cx + y, cy - x);
            SDL_RenderPoint(renderer, cx - y, cy - x);
            SDL_RenderPoint(renderer, cx + y, cy + x);
            SDL_RenderPoint(renderer, cx - y, cy + x);
        }

        if (d < 0) {
            d = d + 4 * x + 6;
        } else {
            d = d + 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

static void drawGun(SDL_Renderer* renderer, float angle, BubbleColor nextColor) {
    uint8_t r = 150, g = 255, b = 150; 
    switch (nextColor) {
    case BubbleColor::Red:
        r = 255; g = 100; b = 100;
        break;
    case BubbleColor::Blue:
        r = 100; g = 150; b = 255;
        break;
    case BubbleColor::Green:
        r = 100; g = 255; b = 150;
        break;
    case BubbleColor::Count:
        break;
    }
    
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    
    drawCircle(renderer, GUN_X, GUN_Y, 15, true);
    
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    drawCircle(renderer, GUN_X, GUN_Y, 15, false);

    float radians = (angle * 3.14159f / 180.f);
    float endX = GUN_X + 80.f * std::sin(radians); 
    float endY = GUN_Y - 80.f * std::cos(radians);  

    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    SDL_RenderLine(renderer, GUN_X, GUN_Y, endX, endY);
}

static void drawDigit(SDL_Renderer* renderer, int digit, int x, int y, int w, int h) {
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255); 
    
    int segW = w / 5;
    int segH = h / 2;
    
    bool segments[7] = {false};  
    
    switch(digit) {
        case 0: segments[0]=segments[1]=segments[2]=segments[3]=segments[4]=segments[5]=true; break;
        case 1: segments[1]=segments[2]=true; break;
        case 2: segments[0]=segments[1]=segments[6]=segments[4]=segments[3]=true; break;
        case 3: segments[0]=segments[1]=segments[6]=segments[2]=segments[3]=true; break;
        case 4: segments[5]=segments[6]=segments[1]=segments[2]=true; break;
        case 5: segments[0]=segments[5]=segments[6]=segments[2]=segments[3]=true; break;
        case 6: segments[0]=segments[5]=segments[4]=segments[3]=segments[2]=segments[6]=true; break;
        case 7: segments[0]=segments[1]=segments[2]=true; break;
        case 8: segments[0]=segments[1]=segments[2]=segments[3]=segments[4]=segments[5]=segments[6]=true; break;
        case 9: segments[0]=segments[1]=segments[2]=segments[6]=segments[5]=segments[3]=true; break;
    }
    
    SDL_FRect rect;
    if (segments[0]) { rect = SDL_FRect{(float)x, (float)y, (float)w, (float)segW}; SDL_RenderFillRect(renderer, &rect); } 
    if (segments[1]) { rect = SDL_FRect{(float)(x+w-segW), (float)y, (float)segW, (float)segH}; SDL_RenderFillRect(renderer, &rect); } 
    if (segments[2]) { rect = SDL_FRect{(float)(x+w-segW), (float)(y+segH), (float)segW, (float)segH}; SDL_RenderFillRect(renderer, &rect); } 
    if (segments[3]) { rect = SDL_FRect{(float)x, (float)(y+h-segW), (float)w, (float)segW}; SDL_RenderFillRect(renderer, &rect); }  
    if (segments[4]) { rect = SDL_FRect{(float)x, (float)(y+segH), (float)segW, (float)segH}; SDL_RenderFillRect(renderer, &rect); }
    if (segments[5]) { rect = SDL_FRect{(float)x, (float)y, (float)segW, (float)segH}; SDL_RenderFillRect(renderer, &rect); }  
    if (segments[6]) { rect = SDL_FRect{(float)x, (float)(y+segH-segW/2), (float)w, (float)segW}; SDL_RenderFillRect(renderer, &rect); }  
}

static void drawNumber(SDL_Renderer* renderer, int number, int x, int y, int digitWidth, int digitHeight) {
    if (number == 0) {
        drawDigit(renderer, 0, x, y, digitWidth, digitHeight);
        return;
    }
    
    std::vector<int> digits;
    int n = number;
    while (n > 0) {
        digits.push_back(n % 10);
        n /= 10;
    }
    
    int offsetX = x;
    for (int i = digits.size() - 1; i >= 0; --i) {
        drawDigit(renderer, digits[i], offsetX, y, digitWidth, digitHeight);
        offsetX += digitWidth + 5;  
    }
}

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Bubble Shooter", WINDOW_WIDTH,
                                     WINDOW_HEIGHT, SDL_WINDOW_RESIZABLE,
                                     &window, &renderer)) {
        return 1;
    }

    Scaling::setMode(ScaleMode::Pixel);
    srand((unsigned)time(nullptr));

    SDL_Texture* winTexture = IMG_LoadTexture(renderer, "../assets/win.png");
    SDL_Texture* loseTexture = IMG_LoadTexture(renderer, "../assets/lose.png");
    if (!winTexture) {
        std::cout << "Failed to load win.png: " << SDL_GetError() << std::endl;
    }
    if (!loseTexture) {
        std::cout << "Failed to load lose.png: " << SDL_GetError() << std::endl;
    }

    Timeline gameTime;
    gameTime.anchorToRealTime();
    
    Engine::PoolAllocator<Engine::GameObject> bubblePool(500);
    Engine::Registry registry(&bubblePool);
    Engine::EventManager eventManager(&gameTime);
    
    InputChord fastLaunchLeft("fast_launch_left", {SDL_SCANCODE_SPACE, SDL_SCANCODE_A});
    InputChord fastLaunchRight("fast_launch_right", {SDL_SCANCODE_SPACE, SDL_SCANCODE_D});
    Input::registerChord(fastLaunchLeft);
    Input::registerChord(fastLaunchRight);

    BubbleShooterGame game(&registry, &eventManager, &gameTime);
    std::atomic<bool> running(true);

    {
        std::vector<Bubble> dbgBubbles;
        std::vector<Projectile> dbgProj;
        float dbgAngle; int dbgScore; bool dbgOver; bool dbgWon; float dbgDescend;
        BubbleColor dbgNextColor;
        game.getGameState(dbgBubbles, dbgProj, dbgAngle, dbgScore, dbgOver, dbgNextColor, dbgWon, dbgDescend);
        std::cout << "After construction: game reports " << dbgBubbles.size() << " bubbles" << std::endl;
    }

    std::thread tGameLoop(gameLoop, std::ref(running), std::ref(game),
                          std::ref(gameTime));

    Timeline renderTime;
    renderTime.anchorToRealTime();

    bool prevT = false;
    bool prev1 = false, prev2 = false, prev3 = false; 

    SDL_Event ev;
    while (running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
        }

        Input::poll();

        bool tNow = Input::isKeyPressed(SDL_SCANCODE_T);
        if (tNow && !prevT) {
            auto current = Scaling::mode();
            Scaling::setMode(
                current == ScaleMode::Pixel ? ScaleMode::Proportional
                                             : ScaleMode::Pixel);
        }
        prevT = tNow;

        static bool prevP = false;
        bool curP = Input::isKeyPressed(SDL_SCANCODE_P);
        if (curP && !prevP) {
            gameTime.togglePause();
        }
        prevP = curP;

        bool k1 = Input::isKeyPressed(SDL_SCANCODE_1);
        bool k2 = Input::isKeyPressed(SDL_SCANCODE_2);
        bool k3 = Input::isKeyPressed(SDL_SCANCODE_3);

        if (k1 && !prev1) {
            gameTime.setScale(0.5);
            std::cout << "\nTime scale: 0.5x\n";
        }
        if (k2 && !prev2) {
            gameTime.setScale(1.0);
            std::cout << "\nTime scale: 1.0x\n";
        }
        if (k3 && !prev3) {
            gameTime.setScale(2.0);
            std::cout << "\nTime scale: 2.0x\n";
        }
        prev1 = k1;
        prev2 = k2;
        prev3 = k3;

        renderTime.tick();

        float rotateDir = 0;
        bool fastLaunchLeftActive = Input::isChordActive("fast_launch_left");
        bool fastLaunchRightActive = Input::isChordActive("fast_launch_right");

        static bool prevFastLeft = false;
        static bool prevFastRight = false;
        bool fastLaunchLeftEdge = fastLaunchLeftActive && !prevFastLeft;
        bool fastLaunchRightEdge = fastLaunchRightActive && !prevFastRight;

        if (fastLaunchLeftActive) {
            rotateDir = -2.f;
        } else if (fastLaunchRightActive) {
            rotateDir = 2.f;
        } else if (Input::isKeyPressed(SDL_SCANCODE_A) ||
                   Input::isKeyPressed(SDL_SCANCODE_LEFT)) {
            rotateDir = -1.f;
        } else if (Input::isKeyPressed(SDL_SCANCODE_D) ||
                   Input::isKeyPressed(SDL_SCANCODE_RIGHT)) {
            rotateDir = 1.f;
        }

        static bool prevFire = false;
        bool curFire = Input::isKeyPressed(SDL_SCANCODE_SPACE);
        bool fire = curFire && !prevFire;
        prevFire = curFire;
        
    game.setInput(rotateDir, fire, fastLaunchLeftEdge, fastLaunchRightEdge);

    prevFastLeft = fastLaunchLeftActive;
    prevFastRight = fastLaunchRightActive;

        std::vector<Bubble> bubbles;
        std::vector<Projectile> projectiles;
        float gunAngle;
        int score;
        bool gameOver;
        bool gameWon;
        float descendOffset;
        BubbleColor nextColor;
        game.getGameState(bubbles, projectiles, gunAngle, score, gameOver, nextColor, gameWon, descendOffset);

        static int frameCount = 0;
        if (frameCount == 0) {
            std::cout << "Frame 0: Retrieved " << bubbles.size() << " bubbles from game" << std::endl;
        }
        frameCount++;
        if (frameCount > 300) frameCount = 0;

        SDL_SetRenderDrawColor(renderer, 10, 10, 30, 255);  
        SDL_RenderClear(renderer);

        float topBoundary = 40.f + BUBBLE_RADIUS + descendOffset;
        SDL_SetRenderDrawColor(renderer, 255, 100, 100, 255);
        for (int i = 0; i < 3; ++i) {
            SDL_RenderLine(renderer, 0, topBoundary + i, WINDOW_WIDTH, topBoundary + i);
        }

        float bottomBoundary = WINDOW_HEIGHT - 100.f;
        SDL_SetRenderDrawColor(renderer, 255, 50, 50, 255);
        for (int i = 0; i < 5; ++i) {
            SDL_RenderLine(renderer, 0, bottomBoundary - i, WINDOW_WIDTH, bottomBoundary - i);
        }

        for (const auto& bubble : bubbles) {
            if (bubble.active) {
                switch (bubble.color) {
                case BubbleColor::Red:
                    SDL_SetRenderDrawColor(renderer, 255, 50, 50, 255);
                    break;
                case BubbleColor::Blue:
                    SDL_SetRenderDrawColor(renderer, 50, 100, 255, 255);
                    break;
                case BubbleColor::Green:
                    SDL_SetRenderDrawColor(renderer, 50, 255, 100, 255);
                    break;
                default:
                    SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
                }
                
                drawCircle(renderer, (int)bubble.x, (int)bubble.y, (int)BUBBLE_RADIUS, true);
                
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                drawCircle(renderer, (int)bubble.x, (int)bubble.y, (int)BUBBLE_RADIUS, false);
            }
        }

        for (const auto& proj : projectiles) {
            if (proj.active) {
                switch (proj.color) {
                case BubbleColor::Red:
                    SDL_SetRenderDrawColor(renderer, 255, 50, 50, 255);
                    break;
                case BubbleColor::Blue:
                    SDL_SetRenderDrawColor(renderer, 50, 100, 255, 255);
                    break;
                case BubbleColor::Green:
                    SDL_SetRenderDrawColor(renderer, 50, 255, 100, 255);
                    break;
                default:
                    SDL_SetRenderDrawColor(renderer, 255, 255, 100, 255);
                    break;
                }
                
                drawCircle(renderer, (int)proj.x, (int)proj.y, 5, true);
            }
        }

        drawGun(renderer, gunAngle, nextColor);

        SDL_SetRenderDrawColor(renderer, 100, 100, 100, 200);
        SDL_FRect scoreBackground = {10.f, 10.f, 200.f, 50.f};
        SDL_RenderFillRect(renderer, &scoreBackground);
        drawNumber(renderer, score, 20, 20, 30, 30);

        if (!gameOver) {
            std::cout << "\rScore: " << score
                      << " | Bubbles: " << bubbles.size()
                      << " | Angle: " << (int)gunAngle
                      << "° | Projectiles: " << projectiles.size()
                      << "   " << std::flush;
        }

        if (gameOver) {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
            SDL_FRect overlay = {0.f, 0.f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT};
            SDL_RenderFillRect(renderer, &overlay);
            
            float centerX = WINDOW_WIDTH / 2.f;
            float centerY = WINDOW_HEIGHT / 2.f;

            if (gameWon) {
                if (winTexture) {
                    SDL_FRect imgRect = {centerX - 200.f, centerY - 220.f, 400.f, 200.f};
                    SDL_RenderTexture(renderer, winTexture, nullptr, &imgRect);
                }
                SDL_SetRenderDrawColor(renderer, 20, 20, 40, 255);
                SDL_FRect rect = {centerX - 250.f, centerY + 40.f, 500.f, 100.f};
                SDL_RenderFillRect(renderer, &rect);
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                drawNumber(renderer, score, (int)(centerX - 100), (int)(centerY + 55), 60, 70);
            } else {
                if (loseTexture) {
                    SDL_FRect imgRect = {centerX - 200.f, centerY - 150.f, 400.f, 150.f};
                    SDL_RenderTexture(renderer, loseTexture, nullptr, &imgRect);
                }
                SDL_SetRenderDrawColor(renderer, 20, 20, 40, 255);
                SDL_FRect rect = {centerX - 200.f, centerY + 10.f, 400.f, 90.f};
                SDL_RenderFillRect(renderer, &rect);
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                drawNumber(renderer, score, (int)(centerX - 70), (int)(centerY + 25), 50, 60);
            }
            static bool loggedGameOver = false;
            if (!loggedGameOver) {
                std::cout << " | GAME OVER!" << std::endl;
                loggedGameOver = true;
            }
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16); 
    }

    tGameLoop.join();

    if (winTexture) SDL_DestroyTexture(winTexture);
    if (loseTexture) SDL_DestroyTexture(loseTexture);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
