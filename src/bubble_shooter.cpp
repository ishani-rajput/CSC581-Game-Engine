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

#include "scaling.h"
#include "timeline.h"
#include "input.h"

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

    Bubble()
        : x(0), y(0), vx(0), vy(0), color(BubbleColor::Red),
          active(false), markedForRemoval(false) {}

    Bubble(float px, float py, BubbleColor c)
        : x(px), y(py), vx(0), vy(0), color(c),
          active(true), markedForRemoval(false) {}

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
        return dist <= (BUBBLE_RADIUS * 2 + 5.f);  
    }
};

struct Projectile {
    float x, y;
    float vx, vy;
    bool active;
    BubbleColor color;

    Projectile()
        : x(0), y(0), vx(0), vy(0), active(false), color(BubbleColor::Red) {}

    Projectile(float px, float py, float velx, float vely, BubbleColor c)
        : x(px), y(py), vx(velx), vy(vely), active(true), color(c) {}

    SDL_FRect getRect() const {
        return SDL_FRect{x - 5.f, y - 5.f, 10.f, 10.f};
    }
};

class BubbleShooterGame {
public:
    BubbleShooterGame()
        : bubbles(),
          projectiles(),
          gunAngle(0.f),  
          score(0),
          gameOver(false),
          bubbleDropTimer(0),
          bubbleDropInterval(10.0f),  
          level(1),
          matchTimer(0),
          matchDuration(0.1f),
          nextBubbleColor(BubbleColor::Red) {
        initializeBubbles();
        pickRandomNextBubbleColor();
        std::cout << "Constructor: initialized " << bubbles.size() << " bubbles" << std::endl;
    }

    void setInput(float rotateDir, bool fire) {
        std::lock_guard<std::mutex> lk(m_);
        desiredRotation = rotateDir;
        if (fire) {
            wantFire = true;
        }
    }

    void getGameState(std::vector<Bubble>& outBubbles,
                      std::vector<Projectile>& outProjectiles,
                      float& outGunAngle,
                      int& outScore,
                      int& outLevel,
                      bool& outGameOver,
                      BubbleColor& outNextColor) const {
        std::lock_guard<std::mutex> lk(m_);
        outBubbles = bubbles;
        outProjectiles = projectiles;
        outGunAngle = gunAngle;
        outScore = score;
        outLevel = level;
        outGameOver = gameOver;
        outNextColor = nextBubbleColor;
    }

    void step(float dt) {
        std::lock_guard<std::mutex> lk(m_);

        if (gameOver) return;

        gunAngle += desiredRotation * 180.f * dt; 
        if (gunAngle < -90.f) gunAngle = -90.f;
        if (gunAngle > 90.f) gunAngle = 90.f;

        if (wantFire) {
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
    }

private:
    enum class GameState { Playing, Won, Lost };

    void initializeBubbles() {
    bubbles.clear();

    const int numRows = 4;
    const int numCols = 8;
    const float spacingX = BUBBLE_RADIUS * 2 + 8; 
    const float spacingY = BUBBLE_RADIUS * 2 + 8;
    const float gridWidth = numCols * spacingX;
    const float startX = (WINDOW_WIDTH - gridWidth) / 2 + BUBBLE_RADIUS;
    const float startY = 40.f + BUBBLE_RADIUS; 

    for (int row = 0; row < numRows; ++row) {
        for (int col = 0; col < numCols; ++col) {
            float x = startX + col * spacingX;
            float y = startY + row * spacingY;
            if (row % 2 == 1) x += spacingX / 2.f;
            BubbleColor color = static_cast<BubbleColor>((row * 2 + col) % static_cast<int>(BubbleColor::Count));
            bubbles.emplace_back(x, y, color);
        }
    }
}

    void fireProjectile() {
        float radians = (gunAngle * 3.14159f / 180.f);

        const float speed = 800.f;

        float velx = speed * std::sin(radians);
        float vely = -speed * std::cos(radians); 

        projectiles.emplace_back(GUN_X, GUN_Y, velx, vely, nextBubbleColor);
        std::cout << "Fired projectile with color: " << (int)nextBubbleColor << std::endl;
        pickRandomNextBubbleColor();
        std::cout << "Next bubble color will be: " << (int)nextBubbleColor << std::endl;
    }

    void pickRandomNextBubbleColor() {
        nextBubbleColor = static_cast<BubbleColor>(rand() % static_cast<int>(BubbleColor::Count));
    }

    void updateProjectiles(float dt) {
        for (auto& proj : projectiles) {
            if (!proj.active) continue;

            proj.x += proj.vx * dt;
            proj.y += proj.vy * dt;

            if (proj.x < 0 || proj.x > WINDOW_WIDTH ||
                proj.y < 0 || proj.y > WINDOW_HEIGHT) {
                proj.active = false;
            }
        }

        projectiles.erase(
            std::remove_if(projectiles.begin(), projectiles.end(),
                          [](const Projectile& p) { return !p.active; }),
            projectiles.end());
    }

    void checkCollisions() {
        static int callCount = 0;
        if (callCount % 60 == 0) {  
            std::cout << "checkCollisions called, projectiles: " << projectiles.size() 
                      << ", bubbles: " << bubbles.size() << std::endl;
        }
        callCount++;
        
        for (auto& proj : projectiles) {
            if (!proj.active) continue;

            for (size_t i = 0; i < bubbles.size(); ++i) {
                Bubble& bubble = bubbles[i];
                if (!bubble.active) continue;

                if (bubble.contains(proj.x, proj.y)) {
                    proj.active = false;
                    
                    std::cout << "Collision detected at (" << proj.x << ", " << proj.y 
                              << ")! Projectile color: " << (int)proj.color << std::endl;
                    
                    float dx = proj.x - bubble.x;
                    float dy = proj.y - bubble.y;
                    float dist = std::sqrt(dx*dx + dy*dy);
                    
                    float newX = proj.x;
                    float newY = proj.y;
                    
                    if (dist > 0.001f) {
                        newX = bubble.x + (dx/dist) * (BUBBLE_RADIUS * 2 + 4.f);
                        newY = bubble.y + (dy/dist) * (BUBBLE_RADIUS * 2 + 4.f);
                    }
                    
                    const float minDistance = BUBBLE_RADIUS * 2 + 2.f;
                    const int maxAttempts = 8;
                    
                    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
                        bool tooClose = false;
                        
                        for (const auto& existingBubble : bubbles) {
                            if (!existingBubble.active) continue;
                            
                            float checkDx = newX - existingBubble.x;
                            float checkDy = newY - existingBubble.y;
                            float checkDist = std::sqrt(checkDx*checkDx + checkDy*checkDy);
                            
                            if (checkDist < minDistance) {
                                tooClose = true;
                                
                                float pushAngle = std::atan2(checkDy, checkDx);
                                newX = existingBubble.x + std::cos(pushAngle) * minDistance;
                                newY = existingBubble.y + std::sin(pushAngle) * minDistance;
                                break;
                            }
                        }
                        
                        if (!tooClose) break;
                    }
                    
                    bubbles.emplace_back(newX, newY, proj.color);
                    size_t newBubbleIdx = bubbles.size() - 1;
                    std::cout << "Added new bubble at (" << newX << ", " << newY 
                              << ") with color " << (int)proj.color << std::endl;
                    
                    popMatchingBubbles(newBubbleIdx);
                    
                    score += 10;
                    break;
                }
            }
        }

        int removedCount = 0;
        auto newEnd = std::remove_if(bubbles.begin(), bubbles.end(),
                          [&removedCount](const Bubble& b) { 
                              if (b.markedForRemoval) removedCount++;
                              return b.markedForRemoval; 
                          });
        bubbles.erase(newEnd, bubbles.end());
        
        if (removedCount > 0) {
            std::cout << "Removed " << removedCount << " bubbles from vector" << std::endl;
        }
    }

    void popMatchingBubbles(size_t hitBubbleIndex) {
        std::vector<bool> visited(bubbles.size(), false);
        std::vector<size_t> queue;
        std::vector<size_t> toRemove;

        queue.push_back(hitBubbleIndex);
        visited[hitBubbleIndex] = true;

        size_t qIndex = 0;
        while (qIndex < queue.size()) {
            size_t current = queue[qIndex++];
            Bubble& b = bubbles[current];
            toRemove.push_back(current);

            for (size_t i = 0; i < bubbles.size(); ++i) {
                if (visited[i] || !bubbles[i].active) continue;

                if (bubbles[i].color == b.color && bubbles[i].intersects(b)) {
                    visited[i] = true;
                    queue.push_back(i);
                }
            }
        }

        std::cout << "Found " << toRemove.size() << " connected bubbles" << std::endl;

        if (toRemove.size() >= 3) {
            std::cout << "Removing " << toRemove.size() << " bubbles!" << std::endl;
            for (size_t idx : toRemove) {
                bubbles[idx].markedForRemoval = true;
            }
            score += 5 * (toRemove.size() - 2);  
        } else {
            std::cout << "Not enough matches (need 3+)" << std::endl;
        }
    }

    void removeFloatingBubbles() {
    }

    void updateBubbles(float dt) {
        for (auto& bubble : bubbles) {
            if (!bubble.active) continue;
        }
    }

    void dropBubbleLayer() {
        const float dropDistance = 45.f;

        for (auto& bubble : bubbles) {
            if (bubble.active) {
                bubble.y += dropDistance;
            }
        }

        bubbleDropInterval *= 0.98f;  
    }

    void checkGameConditions() {
        for (const auto& bubble : bubbles) {
            if (bubble.active && bubble.y + BUBBLE_RADIUS >= WINDOW_HEIGHT) {
                gameOver = true;
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

        if (allCleared && !bubbles.empty()) {
            level++;
            bubbleDropInterval = 10.0f / level; 
            initializeBubbles();
            std::cout << "Level " << level << " cleared! Starting next level..." << std::endl;
        }
    }

    mutable std::mutex m_;
    std::vector<Bubble> bubbles;
    std::vector<Projectile> projectiles;

    float gunAngle;
    float desiredRotation{0};
    bool wantFire{false};

    int score;
    int level;
    bool gameOver;
    float bubbleDropTimer;
    float bubbleDropInterval;
    float matchTimer;
    float matchDuration;
    BubbleColor nextBubbleColor;
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

    BubbleShooterGame game;
    std::atomic<bool> running(true);

    {
        std::vector<Bubble> dbgBubbles;
        std::vector<Projectile> dbgProj;
        float dbgAngle; int dbgScore; int dbgLevel; bool dbgOver;
        BubbleColor dbgNextColor;
        game.getGameState(dbgBubbles, dbgProj, dbgAngle, dbgScore, dbgLevel, dbgOver, dbgNextColor);
        std::cout << "After construction: game reports " << dbgBubbles.size() << " bubbles" << std::endl;
    }

    Timeline gameTime;
    gameTime.anchorToRealTime();

    std::thread tGameLoop(gameLoop, std::ref(running), std::ref(game),
                          std::ref(gameTime));

    Timeline renderTime;
    renderTime.anchorToRealTime();

    bool prevT = false;

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

        renderTime.tick();

        float rotateDir = 0;
        if (Input::isKeyPressed(SDL_SCANCODE_A) ||
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
        
        game.setInput(rotateDir, fire);

        std::vector<Bubble> bubbles;
        std::vector<Projectile> projectiles;
        float gunAngle;
        int score, level;
        bool gameOver;
        BubbleColor nextColor;
        game.getGameState(bubbles, projectiles, gunAngle, score, level, gameOver, nextColor);

        static int frameCount = 0;
        if (frameCount == 0) {
            std::cout << "Frame 0: Retrieved " << bubbles.size() << " bubbles from game" << std::endl;
        }
        frameCount++;
        if (frameCount > 300) frameCount = 0;

        SDL_SetRenderDrawColor(renderer, 10, 10, 30, 255);  
        SDL_RenderClear(renderer);

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

        std::cout << "\rScore: " << score << " | Level: " << level
                  << " | Bubbles: " << bubbles.size() << " | Angle: " << (int)gunAngle
                  << "° | Projectiles: " << projectiles.size() << "   " << std::flush;

        if (gameOver) {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
            SDL_FRect overlay = {0.f, 0.f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT};
            SDL_RenderFillRect(renderer, &overlay);
            
            SDL_SetRenderDrawColor(renderer, 255, 50, 50, 255);
            float centerX = WINDOW_WIDTH / 2.f;
            float centerY = WINDOW_HEIGHT / 2.f;
            
            SDL_FRect redBox = {centerX - 300.f, centerY - 100.f, 600.f, 200.f};
            SDL_RenderFillRect(renderer, &redBox);
            
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            
            SDL_FRect rect;  
            
            rect = {centerX - 280.f, centerY - 60.f, 40.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX - 280.f, centerY - 60.f, 10.f, 80.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX - 280.f, centerY + 10.f, 40.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX - 250.f, centerY + 10.f, 10.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            
            rect = {centerX - 220.f, centerY - 60.f, 40.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX - 220.f, centerY - 60.f, 10.f, 80.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX - 180.f, centerY - 60.f, 10.f, 80.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX - 220.f, centerY - 10.f, 40.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            
            rect = {centerX - 150.f, centerY - 60.f, 10.f, 80.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX - 100.f, centerY - 60.f, 10.f, 80.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX - 150.f, centerY - 60.f, 20.f, 20.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX - 120.f, centerY - 60.f, 20.f, 20.f}; SDL_RenderFillRect(renderer, &rect);
            
            rect = {centerX - 70.f, centerY - 60.f, 40.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX - 70.f, centerY - 60.f, 10.f, 80.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX - 70.f, centerY - 10.f, 30.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX - 70.f, centerY + 10.f, 40.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            
            rect = {centerX + 20.f, centerY - 60.f, 40.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX + 20.f, centerY - 60.f, 10.f, 80.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX + 50.f, centerY - 60.f, 10.f, 80.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX + 20.f, centerY + 10.f, 40.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            
            rect = {centerX + 80.f, centerY - 60.f, 10.f, 60.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX + 130.f, centerY - 60.f, 10.f, 60.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX + 90.f, centerY + 0.f, 10.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX + 100.f, centerY + 10.f, 10.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX + 110.f, centerY + 10.f, 10.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX + 120.f, centerY + 0.f, 10.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            
            rect = {centerX + 160.f, centerY - 60.f, 40.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX + 160.f, centerY - 60.f, 10.f, 80.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX + 160.f, centerY - 10.f, 30.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX + 160.f, centerY + 10.f, 40.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            
            rect = {centerX + 220.f, centerY - 60.f, 40.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX + 220.f, centerY - 60.f, 10.f, 80.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX + 250.f, centerY - 60.f, 10.f, 40.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX + 220.f, centerY - 10.f, 40.f, 10.f}; SDL_RenderFillRect(renderer, &rect);
            rect = {centerX + 250.f, centerY + 0.f, 10.f, 20.f}; SDL_RenderFillRect(renderer, &rect);
            
            drawNumber(renderer, score, (int)(centerX - 50), (int)(centerY + 50), 40, 40);
            
            std::cout << " | GAME OVER!" << std::endl;
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16); 
    }

    tGameLoop.join();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
