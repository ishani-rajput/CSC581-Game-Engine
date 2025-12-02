#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <iostream>
#include <vector>
#include <deque>
#include <cmath>
#include <random>
#include <any>
#include "registry.h"
#include "event_manager.h"
#include "timeline.h"
#include "input.h"
#include "collision.h"
#include "physics.h"
#include "memory_pool.h"
#include "scaling.h"

constexpr float SCREEN_WIDTH = 800.0f;
constexpr float SCREEN_HEIGHT = 600.0f;
constexpr float GRID_SIZE = 20.0f;
constexpr int GRID_WIDTH = static_cast<int>(SCREEN_WIDTH / GRID_SIZE);
constexpr int GRID_HEIGHT = static_cast<int>(SCREEN_HEIGHT / GRID_SIZE);
constexpr float BASE_MOVE_INTERVAL = 0.12f; 
constexpr float DIAGONAL_MOVE_INTERVAL = BASE_MOVE_INTERVAL * 1.15f; 

struct Direction {
    int dx, dy;
    bool isDiagonal;
};

const Direction DIR_UP    = {0, -1, false};
const Direction DIR_DOWN  = {0,  1, false};
const Direction DIR_LEFT  = {-1, 0, false};
const Direction DIR_RIGHT = {1,  0, false};
const Direction DIR_UP_LEFT    = {-1, -1, true};
const Direction DIR_UP_RIGHT   = {1, -1, true};
const Direction DIR_DOWN_LEFT  = {-1,  1, true};
const Direction DIR_DOWN_RIGHT = {1,  1, true};


struct Position {
    int x, y;
    bool operator==(const Position& other) const {
        return x == other.x && y == other.y;
    }
};


enum class ObjectType {
    SnakeHead = 0,
    SnakeBody = 1,
    Food = 2
};


namespace SnakeEvents {
    inline Engine::Event FoodEaten(int score, Timeline* timeline) {
        Engine::Event evt(Engine::EventType::Custom, timeline->time(), 0);
        evt.payload["type"] = std::string("food_eaten");
        evt.payload["score"] = score;
        return evt;
    }
    
    inline Engine::Event SnakeGrew(int newLength, Timeline* timeline) {
        Engine::Event evt(Engine::EventType::Custom, timeline->time(), 0);
        evt.payload["type"] = std::string("snake_grew");
        evt.payload["length"] = newLength;
        return evt;
    }
}


struct SnakeGame {
    std::deque<std::string> snakeSegmentIds; 
    Direction currentDirection;
    Direction nextDirection;
    float timeSinceMove;
    int score;
    bool gameOver;
    bool paused;
    std::mt19937 rng;
    int segmentCounter; 
    
    SnakeGame() : 
        currentDirection(DIR_RIGHT),
        nextDirection(DIR_RIGHT),
        timeSinceMove(0.0f),
        score(0),
        gameOver(false),
        paused(false),
        rng(std::random_device{}()),
        segmentCounter(0)
    {
    }
    
    float getMoveInterval() const {
        return currentDirection.isDiagonal ? DIAGONAL_MOVE_INTERVAL : BASE_MOVE_INTERVAL;
    }
};

SnakeGame gGame;

// Local scaling helper for snake game (800x600 base resolution)
SDL_FRect computeSnakeScaling(const SDL_FRect& logical, SDL_Window* window) {
    if (Scaling::mode() == ScaleMode::Pixel) {
        return logical;
    }
    
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    
    float scaleX = ww / SCREEN_WIDTH;
    float scaleY = wh / SCREEN_HEIGHT;
    
    SDL_FRect out;
    out.x = logical.x * scaleX;
    out.y = logical.y * scaleY;
    out.w = logical.w * scaleX;
    out.h = logical.h * scaleY;
    
    return out;
}

void drawFilledRect(SDL_Renderer* renderer, SDL_Window* window, float x, float y, float w, float h, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_FRect logical = {x, y, w, h};
    SDL_FRect scaled = computeSnakeScaling(logical, window);
    SDL_RenderFillRect(renderer, &scaled);
}

void drawCircle(SDL_Renderer* renderer, SDL_Window* window, float cx, float cy, float radius, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    
    // Apply scaling to the center point and radius
    SDL_FRect logical = {cx - radius, cy - radius, radius * 2, radius * 2};
    SDL_FRect scaled = computeSnakeScaling(logical, window);
    float scaledCx = scaled.x + scaled.w / 2;
    float scaledCy = scaled.y + scaled.h / 2;
    float scaledRadius = scaled.w / 2;
    
    int r = static_cast<int>(scaledRadius);
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        int dx = static_cast<int>(std::sqrt(std::max(r2 - dy * dy, 0)));
        SDL_RenderLine(renderer, scaledCx - dx, scaledCy + dy, scaledCx + dx, scaledCy + dy);
    }
}


void resetGame(SnakeGame& game, Engine::Registry& registry, Engine::EventManager& eventManager, Timeline& timeline);
void initializeSnake(SnakeGame& game, Engine::Registry& registry);
void spawnFood(Engine::Registry& registry, const SnakeGame& game);

void createSnakeSegment(Engine::Registry& registry, const std::string& id, int gridX, int gridY, bool isHead) {
    auto& segment = registry.upsert(id);
    segment.set("gridX", gridX);
    segment.set("gridY", gridY);
    segment.set("type", static_cast<int>(isHead ? ObjectType::SnakeHead : ObjectType::SnakeBody));
    segment.set("alive", true);
}

void createFood(Engine::Registry& registry, int gridX, int gridY) {
    auto& food = registry.upsert("food");
    food.set("gridX", gridX);
    food.set("gridY", gridY);
    food.set("type", static_cast<int>(ObjectType::Food));
    food.set("alive", true);
}

void spawnFood(Engine::Registry& registry, const SnakeGame& game) {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> distX(0, GRID_WIDTH - 1);
    std::uniform_int_distribution<int> distY(0, GRID_HEIGHT - 1);
    
    int foodX, foodY;
    bool validPosition;
    
    do {
        foodX = distX(rng);
        foodY = distY(rng);
        validPosition = true;
        
        for (const auto& segId : game.snakeSegmentIds) {
            auto* seg = registry.get(segId);
            if (seg && seg->get<int>("gridX") == foodX && seg->get<int>("gridY") == foodY) {
                validPosition = false;
                break;
            }
        }
    } while (!validPosition);
    
    createFood(registry, foodX, foodY);
}

void initializeSnake(SnakeGame& game, Engine::Registry& registry) {
    game.snakeSegmentIds.clear();
    game.segmentCounter = 0;
    
    int startX = GRID_WIDTH / 2;
    int startY = GRID_HEIGHT / 2;
    
    std::string headId = "snake_" + std::to_string(game.segmentCounter++);
    createSnakeSegment(registry, headId, startX, startY, true);
    game.snakeSegmentIds.push_back(headId);
    
    for (int i = 1; i < 3; i++) {
        std::string bodyId = "snake_" + std::to_string(game.segmentCounter++);
        createSnakeSegment(registry, bodyId, startX - i, startY, false);
        game.snakeSegmentIds.push_back(bodyId);
    }
    
    spawnFood(registry, game);
}

void updateGame(SnakeGame& game, Engine::Registry& registry, Timeline& timeline, Engine::EventManager& eventManager, float dt) {
    if (game.gameOver || game.paused) return;
    
    game.timeSinceMove += dt;
    
    if (game.timeSinceMove >= game.getMoveInterval()) {
        game.timeSinceMove = 0.0f;
        
        game.currentDirection = game.nextDirection;
        
        auto* head = registry.get(game.snakeSegmentIds.front());
        if (!head) return;
        
        int headX = head->get<int>("gridX");
        int headY = head->get<int>("gridY");
        
        int newHeadX = headX + game.currentDirection.dx;
        int newHeadY = headY + game.currentDirection.dy;
        
        if (newHeadX < 0) {
            newHeadX = GRID_WIDTH - 1;
        } else if (newHeadX >= GRID_WIDTH) {
            newHeadX = 0;
        }
        
        if (newHeadY < 0 || newHeadY >= GRID_HEIGHT) {
            std::cout << "*** GAME OVER *** Hit top/bottom wall! Final Score: " << game.score << "\n";
            std::cout << "[GAME] Auto-resetting...\n\n";
            
            Engine::Event deathEvent = Engine::Events::Death("snake_head", &timeline);
            deathEvent.payload["reason"] = std::string("wall_collision");
            eventManager.raiseEvent(deathEvent);
            
            resetGame(game, registry, eventManager, timeline);
            return;
        }
        
        for (const auto& segId : game.snakeSegmentIds) {
            auto* seg = registry.get(segId);
            if (seg && seg->get<int>("gridX") == newHeadX && seg->get<int>("gridY") == newHeadY) {
                std::cout << "*** GAME OVER *** Hit yourself! Final Score: " << game.score << "\n";
                std::cout << "[GAME] Auto-resetting...\n\n";
                
                Engine::Event deathEvent = Engine::Events::Death("snake_head", &timeline);
                deathEvent.payload["reason"] = std::string("self_collision");
                eventManager.raiseEvent(deathEvent);
                
                resetGame(game, registry, eventManager, timeline);
                return;
            }
        }
        
        auto* food = registry.get("food");
        bool ateFood = false;
        if (food) {
            int foodX = food->get<int>("gridX");
            int foodY = food->get<int>("gridY");
            
            if (newHeadX == foodX && newHeadY == foodY) {
                ateFood = true;
                game.score += 10;
                
                Engine::Event foodEvent = SnakeEvents::FoodEaten(game.score, &timeline);
                eventManager.raiseEvent(foodEvent);
                
                std::cout << "[SNAKE] Food eaten! Score: " << game.score << "\n";
                
                spawnFood(registry, game);
            }
        }
        
        head->set("type", static_cast<int>(ObjectType::SnakeBody));
        
        std::string newHeadId = "snake_" + std::to_string(game.segmentCounter++);
        createSnakeSegment(registry, newHeadId, newHeadX, newHeadY, true);
        game.snakeSegmentIds.push_front(newHeadId);
        
        if (ateFood) {
            Engine::Event growEvent = SnakeEvents::SnakeGrew(game.snakeSegmentIds.size(), &timeline);
            eventManager.raiseEvent(growEvent);
        } else {
            std::string tailId = game.snakeSegmentIds.back();
            game.snakeSegmentIds.pop_back();
            registry.erase(tailId);
        }
    }
}

void renderGame(SDL_Renderer* renderer, SDL_Window* window, const SnakeGame& game, Engine::Registry& registry) {
    SDL_SetRenderDrawColor(renderer, 10, 10, 15, 255);
    SDL_RenderClear(renderer);
    
    // Draw game area background
    SDL_FRect gameAreaLogical = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
    SDL_FRect gameArea = computeSnakeScaling(gameAreaLogical, window);
    SDL_SetRenderDrawColor(renderer, 20, 20, 30, 255);
    SDL_RenderFillRect(renderer, &gameArea);
    
    // Draw game area border
    SDL_SetRenderDrawColor(renderer, 100, 100, 120, 255);
    SDL_RenderRect(renderer, &gameArea);
    
    auto* food = registry.get("food");
    if (food && food->get<bool>("alive", true)) {
        int foodX = food->get<int>("gridX");
        int foodY = food->get<int>("gridY");
        float foodCenterX = foodX * GRID_SIZE + GRID_SIZE / 2;
        float foodCenterY = foodY * GRID_SIZE + GRID_SIZE / 2;
        drawCircle(renderer, window, foodCenterX, foodCenterY, GRID_SIZE / 2 - 2, {255, 50, 50, 255});
    }
    
    for (const auto& segId : game.snakeSegmentIds) {
        auto* segment = registry.get(segId);
        if (!segment || !segment->get<bool>("alive", true)) continue;
        
        int gridX = segment->get<int>("gridX");
        int gridY = segment->get<int>("gridY");
        int type = segment->get<int>("type");
        
        float x = gridX * GRID_SIZE;
        float y = gridY * GRID_SIZE;
        
        if (type == static_cast<int>(ObjectType::SnakeHead)) {
            drawFilledRect(renderer, window, x + 1, y + 1, GRID_SIZE - 2, GRID_SIZE - 2, {100, 255, 100, 255});
        } else {
            drawFilledRect(renderer, window, x + 1, y + 1, GRID_SIZE - 2, GRID_SIZE - 2, {50, 200, 50, 255});
        }
    }
    
    if (game.gameOver) {
        SDL_SetRenderDrawColor(renderer, 255, 50, 50, 255);
        float cx = SCREEN_WIDTH / 2.0f;
        float cy = SCREEN_HEIGHT / 2.0f;
        
        SDL_FRect line1Start = {cx - 30, cy - 30, 0, 0};
        SDL_FRect line1End = {cx + 30, cy + 30, 0, 0};
        SDL_FRect line2Start = {cx - 30, cy + 30, 0, 0};
        SDL_FRect line2End = {cx + 30, cy - 30, 0, 0};
        
        SDL_FRect s1 = computeSnakeScaling(line1Start, window);
        SDL_FRect e1 = computeSnakeScaling(line1End, window);
        SDL_FRect s2 = computeSnakeScaling(line2Start, window);
        SDL_FRect e2 = computeSnakeScaling(line2End, window);
        
        SDL_RenderLine(renderer, s1.x, s1.y, e1.x, e1.y);
        SDL_RenderLine(renderer, s2.x, s2.y, e2.x, e2.y);
    }
    
    SDL_RenderPresent(renderer);
}

void resetGame(SnakeGame& game, Engine::Registry& registry, Engine::EventManager& eventManager, Timeline& timeline) {
    registry.clear();
    
    game.snakeSegmentIds.clear();
    game.currentDirection = DIR_RIGHT;
    game.nextDirection = DIR_RIGHT;
    game.timeSinceMove = 0.0f;
    game.score = 0;
    game.gameOver = false;
    game.segmentCounter = 0;
    
    initializeSnake(game, registry);
    
    std::cout << "[SNAKE] Game reset!\n";
}

int main(int argc, char* argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL init failed: " << SDL_GetError() << "\n";
        return 1;
    }
    
    SDL_Window* window = SDL_CreateWindow(
        "Snake Game",
        static_cast<int>(SCREEN_WIDTH),
        static_cast<int>(SCREEN_HEIGHT),
        SDL_WINDOW_RESIZABLE
    );
    
    if (!window) {
        std::cerr << "Window creation failed: " << SDL_GetError() << "\n";
        return 1;
    }
    
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::cerr << "Renderer creation failed: " << SDL_GetError() << "\n";
        return 1;
    }
    
    SDL_SetRenderVSync(renderer, 1);
    
    Timeline gameTime;
    gameTime.anchorToRealTime();
    gameTime.setScale(1.0);
    
    Engine::EventManager eventManager(&gameTime);
    Engine::PoolAllocator<Engine::GameObject> objectPool(500);
    Engine::Registry registry(&objectPool);
    
    InputChord moveUpLeft("up_left", {SDL_SCANCODE_W, SDL_SCANCODE_A});
    InputChord moveUpRight("up_right", {SDL_SCANCODE_W, SDL_SCANCODE_D});
    InputChord moveDownLeft("down_left", {SDL_SCANCODE_S, SDL_SCANCODE_A});
    InputChord moveDownRight("down_right", {SDL_SCANCODE_S, SDL_SCANCODE_D});
    
    Input::registerChord(moveUpLeft);
    Input::registerChord(moveUpRight);
    Input::registerChord(moveDownLeft);
    Input::registerChord(moveDownRight);
    
    initializeSnake(gGame, registry);
    
    bool running = true;
    bool prevP = false;
    bool prevR = false;
    bool prevToggle = false;
    bool prev1 = false, prev2 = false, prev3 = false;
    
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
        }
        
        Input::poll();
        
        if (Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) {
            running = false;
        }
        
        bool toggleNow = Input::isKeyPressed(SDL_SCANCODE_T);
        if (toggleNow && !prevToggle) {
            Scaling::setMode(Scaling::mode() == ScaleMode::Pixel ? ScaleMode::Proportional : ScaleMode::Pixel);
            std::cout << "Scaling mode: " << (Scaling::mode() == ScaleMode::Pixel ? "Pixel" : "Proportional") << "\n";
            
            Engine::Event toggleEvent = Engine::Events::Input("T", true, &gameTime);
            toggleEvent.payload["action"] = std::string("toggle_scale");
            toggleEvent.payload["state"] = std::string(Scaling::mode() == ScaleMode::Pixel ? "Pixel" : "Proportional");
            eventManager.raiseEvent(toggleEvent);
        }
        prevToggle = toggleNow;
        
        bool pNow = Input::isKeyPressed(SDL_SCANCODE_P);
        if (pNow && !prevP) {
            gGame.paused = !gGame.paused;
            std::cout << "[GAME] " << (gGame.paused ? "PAUSED" : "RESUMED") << "\n";
        }
        prevP = pNow;
        
        bool rNow = Input::isKeyPressed(SDL_SCANCODE_R);
        if (rNow && !prevR) {
            resetGame(gGame, registry, eventManager, gameTime);
        }
        prevR = rNow;
        
        bool k1Now = Input::isKeyPressed(SDL_SCANCODE_1);
        bool k2Now = Input::isKeyPressed(SDL_SCANCODE_2);
        bool k3Now = Input::isKeyPressed(SDL_SCANCODE_3);
        
        if (k1Now && !prev1) {
            gameTime.setScale(0.5);
            std::cout << "[GAME] Time scale: 0.5x (slow motion)\n";
        }
        if (k2Now && !prev2) {
            gameTime.setScale(1.0);
            std::cout << "[GAME] Time scale: 1.0x (normal)\n";
        }
        if (k3Now && !prev3) {
            gameTime.setScale(2.0);
            std::cout << "[GAME] Time scale: 2.0x (fast forward)\n";
        }
        
        prev1 = k1Now;
        prev2 = k2Now;
        prev3 = k3Now;
        
        double dtSec = gameTime.tick();
        if (dtSec <= 0.0) {
            SDL_Delay(1);
            continue;
        }
        float dt = static_cast<float>(dtSec);
        
        if (!gGame.gameOver && !gGame.paused) {
            Direction newDir = gGame.nextDirection;
            bool dirChanged = false;
            
            if (Input::isChordActive("up_left")) {
                newDir = DIR_UP_LEFT;
                dirChanged = true;
            } else if (Input::isChordActive("up_right")) {
                newDir = DIR_UP_RIGHT;
                dirChanged = true;
            } else if (Input::isChordActive("down_left")) {
                newDir = DIR_DOWN_LEFT;
                dirChanged = true;
            } else if (Input::isChordActive("down_right")) {
                newDir = DIR_DOWN_RIGHT;
                dirChanged = true;
            }
            else if (Input::isKeyPressed(SDL_SCANCODE_W)) {
                newDir = DIR_UP;
                dirChanged = true;
            } else if (Input::isKeyPressed(SDL_SCANCODE_S)) {
                newDir = DIR_DOWN;
                dirChanged = true;
            } else if (Input::isKeyPressed(SDL_SCANCODE_A)) {
                newDir = DIR_LEFT;
                dirChanged = true;
            } else if (Input::isKeyPressed(SDL_SCANCODE_D)) {
                newDir = DIR_RIGHT;
                dirChanged = true;
            }
            
            if (dirChanged && !gGame.snakeSegmentIds.empty()) {
                auto* head = registry.get(gGame.snakeSegmentIds.front());
                if (head) {
                    int headX = head->get<int>("gridX");
                    int headY = head->get<int>("gridY");
                    
                    int nextX = headX + newDir.dx;
                    int nextY = headY + newDir.dy;
                    
                    bool validMove = true;
                    if (gGame.snakeSegmentIds.size() >= 2) {
                        auto* neck = registry.get(gGame.snakeSegmentIds[1]);
                        if (neck) {
                            int neckX = neck->get<int>("gridX");
                            int neckY = neck->get<int>("gridY");
                            if (nextX == neckX && nextY == neckY) {
                                validMove = false;
                            }
                        }
                    }
                    
                    if (validMove) {
                        gGame.nextDirection = newDir;
                    }
                }
            } else if (dirChanged) {
                gGame.nextDirection = newDir;
            }
        }
        
        updateGame(gGame, registry, gameTime, eventManager, dt);
        eventManager.dispatchEvents();
        
        renderGame(renderer, window, gGame, registry);
    }
    
    std::cout << "\n=== Game Ended ===\n";
    std::cout << "  Final Score: " << gGame.score << "\n";
    
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}