 #include <SDL3/SDL.h>
 #include <SDL3/SDL_main.h>
 #include <iostream>
 #include <vector>
 #include <cmath>
 #include <random>
 #include <algorithm>
#include <functional>

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
constexpr float BUBBLE_RADIUS = 20.0f;
constexpr float CANNON_BASE_Y = SCREEN_HEIGHT - 50.0f;
constexpr float BUBBLE_SHOOT_SPEED = 600.0f;
constexpr float GRID_OFFSET_X = 100.0f;
constexpr float GRID_OFFSET_Y = 50.0f;
constexpr float DESCEND_SPEED = 20.0f;
constexpr float DESCEND_INTERVAL = 10.0f;
constexpr int   GRID_COLS = 15;
constexpr int   GRID_ROWS = 12;

enum class BubbleColor {
     Red,
     Blue,
     Green,
     Yellow,
     Purple,
     COUNT
};

struct GameState {
    float        cannonAngle     = -90.0f;
     float        cannonX         = SCREEN_WIDTH / 2.0f;
     bool         firing          = false;
     std::string  activeBubbleId  = "";
     BubbleColor  nextBubbleColor = BubbleColor::Red;
     float        timeSinceDescend = 0.0f;
     float        descendOffset    = 0.0f;
     int          score            = 0;
     bool         gameOver         = false;
    bool         won              = false;

    bool grid[GRID_ROWS][GRID_COLS] = {{false}};

    int shotCounter = 0;
 };
 
GameState   gGameState;
std::mt19937 gRng(std::random_device{}());

// Local scaling helper for bubble shooter (800x600 base resolution)
SDL_FRect computeBubbleScaling(const SDL_FRect& logical, SDL_Window* window) {
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

SDL_Color getColorFromEnum(BubbleColor color) {
     switch (color) {
         case BubbleColor::Red:    return SDL_Color{255, 50, 50, 255};
         case BubbleColor::Blue:   return SDL_Color{50, 150, 255, 255};
         case BubbleColor::Green:  return SDL_Color{50, 255, 50, 255};
         case BubbleColor::Yellow: return SDL_Color{255, 255, 50, 255};
         case BubbleColor::Purple: return SDL_Color{200, 50, 255, 255};
         default:                  return SDL_Color{255, 255, 255, 255};
     }
}

BubbleColor randomBubbleColor() {
     std::uniform_int_distribution<int> dist(0, static_cast<int>(BubbleColor::COUNT) - 1);
     return static_cast<BubbleColor>(dist(gRng));
}

void gridToScreen(int row, int col, float& x, float& y, float descendOffset) {
    float offsetX = (row % 2 == 1) ? BUBBLE_RADIUS : 0.0f;
     x = GRID_OFFSET_X + col * (BUBBLE_RADIUS * 2.0f) + offsetX;
     y = GRID_OFFSET_Y + row * (BUBBLE_RADIUS * 1.8f) + descendOffset;
}

void screenToGrid(float x, float y, int& row, int& col, float descendOffset) {
    float adjustedY = y - descendOffset;

    row = static_cast<int>((adjustedY - GRID_OFFSET_Y) / (BUBBLE_RADIUS * 1.8f) + 0.5f);
    
    float offsetX = (row % 2 == 1) ? BUBBLE_RADIUS : 0.0f;
     col = static_cast<int>((x - GRID_OFFSET_X - offsetX) / (BUBBLE_RADIUS * 2.0f) + 0.5f);
}

void createBubble(Engine::Registry& registry, const std::string& id,
                   float x, float y, BubbleColor color, bool isStatic) {
     auto& bubble = registry.upsert(id);
     bubble.set("x", x);
     bubble.set("y", y);
     bubble.set("radius", BUBBLE_RADIUS);
     bubble.set("color", static_cast<int>(color));
     bubble.set("static", isStatic);
     bubble.set("vx", 0.0f);
     bubble.set("vy", 0.0f);
     bubble.set("alive", true);
 
     if (!isStatic) {
         bubble.set("gridRow", -1);
         bubble.set("gridCol", -1);
     }
}

void initializeGrid(Engine::Registry& registry) {
    int bubbleCount = 0;

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            if ((row + col) % 3 != 0) {
                 float x, y;
                 gridToScreen(row, col, x, y, 0.0f);
 
                 BubbleColor color = randomBubbleColor();
                 std::string id    = "bubble_" + std::to_string(row) + "_" + std::to_string(col);
 
                 createBubble(registry, id, x, y, color, true);
                 gGameState.grid[row][col] = true;
 
                 auto* b = registry.get(id);
                 b->set("gridRow", row);
                 b->set("gridCol", col);
 
                 bubbleCount++;
             }
         }
     }
 
    std::cout << "Game initialized with " << bubbleCount << " bubbles\n";

    gGameState.nextBubbleColor = randomBubbleColor();
     createBubble(registry, "current_bubble",
                  gGameState.cannonX, CANNON_BASE_Y - 30.0f,
                  gGameState.nextBubbleColor, false);
}

void floodFill(Engine::Registry& registry, const std::string& startId,
                std::vector<std::string>& connected, BubbleColor targetColor) {
     auto* startBubble = registry.get(startId);
     if (!startBubble || !startBubble->get<bool>("alive")) return;
 
    int startColor = startBubble->get<int>("color");
    if (static_cast<BubbleColor>(startColor) != targetColor) return;

    startBubble->set("visited", true);
     connected.push_back(startId);
 
     float x = startBubble->get<float>("x");
    float y = startBubble->get<float>("y");

    for (const auto& [id, _] : registry) {
         auto* bubble = registry.get(id);
         if (!bubble || !bubble->get<bool>("alive")) continue;
         if (bubble->get<bool>("visited", false)) continue;
         if (!bubble->get<bool>("static")) continue;
 
         float bx = bubble->get<float>("x");
         float by = bubble->get<float>("y");
         float dx = x - bx;
         float dy = y - by;
        float dist = std::sqrt(dx * dx + dy * dy);

        if (dist < BUBBLE_RADIUS * 2.5f) {
             floodFill(registry, id, connected, targetColor);
         }
     }
}

void removeFloatingBubbles(Engine::Registry& registry, Engine::EventManager& eventManager,
                           Timeline& timeline) {
    for (const auto& [id, _] : registry) {
         auto* bubble = registry.get(id);
        if (bubble) bubble->set("visited", false);
    }

    for (int col = 0; col < GRID_COLS; col++) {
         float tx, ty;
         gridToScreen(0, col, tx, ty, gGameState.descendOffset);
 
         for (const auto& [id, _] : registry) {
             auto* bubble = registry.get(id);
             if (!bubble || !bubble->get<bool>("static") || !bubble->get<bool>("alive")) continue;
 
             float bx = bubble->get<float>("x");
             float by = bubble->get<float>("y");
            float dist = std::sqrt((tx - bx) * (tx - bx) + (ty - by) * (ty - by));

            if (dist < BUBBLE_RADIUS * 1.5f) {
                std::function<void(const std::string&)> markConnected =
                     [&](const std::string& bubbleId) {
                         auto* b = registry.get(bubbleId);
                         if (!b || !b->get<bool>("alive")) return;
                         if (b->get<bool>("visited", false)) return;
                         if (!b->get<bool>("static")) return;
 
                         b->set("visited", true);
 
                         float currX = b->get<float>("x");
                         float currY = b->get<float>("y");
 
                         for (const auto& [nId, _] : registry) {
                             auto* nb = registry.get(nId);
                             if (!nb || !nb->get<bool>("static") || !nb->get<bool>("alive")) continue;
                             if (nb->get<bool>("visited", false)) continue;
 
                             float nbx = nb->get<float>("x");
                             float nby = nb->get<float>("y");
                             float ndist = std::sqrt(
                                 (currX - nbx) * (currX - nbx) +
                                 (currY - nby) * (currY - nby)
                             );
 
                             if (ndist < BUBBLE_RADIUS * 2.5f) {
                                 markConnected(nId);
                             }
                         }
                     };
 
                 markConnected(id);
             }
        }
    }

    std::vector<std::string> toRemove;
     for (const auto& [id, _] : registry) {
         auto* bubble = registry.get(id);
         if (bubble &&
             bubble->get<bool>("static") &&
             bubble->get<bool>("alive") &&
             !bubble->get<bool>("visited", false)) {
             toRemove.push_back(id);
         }
     }
 
     for (const std::string& id : toRemove) {
         auto* bubble = registry.get(id);
         if (bubble) {
             int row = bubble->get<int>("gridRow", -1);
             int col = bubble->get<int>("gridCol", -1);
             if (row >= 0 && col >= 0) gGameState.grid[row][col] = false;
 
             bubble->set("alive", false);
 
             Engine::Event deathEvent = Engine::Events::Death(id, &timeline);
            deathEvent.payload["reason"] = std::string("floating");
            eventManager.raiseEvent(deathEvent);

            gGameState.score += 5;
        }
        registry.erase(id);
     }
 
     if (!toRemove.empty()) {
         std::cout << "  > Removed " << toRemove.size() << " floating bubbles\n";
     }
}

void handleBubbleCollision(Engine::Registry& registry, Engine::EventManager& eventManager,
                            Timeline& timeline, const std::string& projectileId) {
     auto* projectile = registry.get(projectileId);
     if (!projectile) return;
 
     float px = projectile->get<float>("x");
     float py = projectile->get<float>("y");
    BubbleColor pColor = static_cast<BubbleColor>(projectile->get<int>("color"));

    int gridRow, gridCol;
    screenToGrid(px, py, gridRow, gridCol, gGameState.descendOffset);

    if (gridRow >= GRID_ROWS) {
         std::cout << "Game Over.\n";
         gGameState.gameOver = true;
         gGameState.won = false;
         registry.erase(projectileId);
        return;
    }

    gridCol = std::clamp(gridCol, 0, GRID_COLS - 1);
    
    if (gridRow >= 0 && gGameState.grid[gridRow][gridCol]) {
        if (gridRow > 0) gridRow--;
        else {
            gGameState.gameOver = true;
             return;
         }
     }
 
    if (gridRow < 0) gridRow = 0;

    float snapX, snapY;
     gridToScreen(gridRow, gridCol, snapX, snapY, gGameState.descendOffset);
 
     projectile->set("x", snapX);
     projectile->set("y", snapY);
     projectile->set("static", true);
     projectile->set("vx", 0.0f);
     projectile->set("vy", 0.0f);
     projectile->set("gridRow", gridRow);
     projectile->set("gridCol", gridCol);
    gGameState.grid[gridRow][gridCol] = true;

    for (const auto& [id, _] : registry) {
         auto* bubble = registry.get(id);
        if (bubble) bubble->set("visited", false);
    }

    std::vector<std::string> connected;
    floodFill(registry, projectileId, connected, pColor);

    if (connected.size() >= 3) {
         std::cout <<connected.size() << " bubbles POP!\n";
         for (const std::string& id : connected) {
             auto* bubble = registry.get(id);
             if (bubble) {
                 int row = bubble->get<int>("gridRow", -1);
                 int col = bubble->get<int>("gridCol", -1);
                 if (row >= 0 && col >= 0) {
                     gGameState.grid[row][col] = false;
                 }
 
                 bubble->set("alive", false);
 
                 Engine::Event deathEvent = Engine::Events::Death(id, &timeline);
                 deathEvent.payload["reason"] = std::string("matched");
                 eventManager.raiseEvent(deathEvent);
 
                 gGameState.score += 10;
             }
            registry.erase(id);
        }

        removeFloatingBubbles(registry, eventManager, timeline);
     }

    gGameState.firing = false;
    gGameState.activeBubbleId.clear();

    gGameState.nextBubbleColor = randomBubbleColor();
     auto& preview = registry.upsert("current_bubble");
     preview.set("x", gGameState.cannonX);
     preview.set("y", CANNON_BASE_Y - 30.0f);
     preview.set("radius", BUBBLE_RADIUS);
     preview.set("color", static_cast<int>(gGameState.nextBubbleColor));
     preview.set("static", false);
     preview.set("vx", 0.0f);
     preview.set("vy", 0.0f);
     preview.set("alive", true);
     preview.set("gridRow", -1);
     preview.set("gridCol", -1);
}

void checkWinCondition(GameState& state) {
     bool anyBubblesLeft = false;
     for (int row = 0; row < GRID_ROWS; row++) {
         for (int col = 0; col < GRID_COLS; col++) {
             if (state.grid[row][col]) {
                 anyBubblesLeft = true;
                 break;
             }
         }
         if (anyBubblesLeft) break;
     }
 
     if (!anyBubblesLeft) {
         state.won = true;
         state.gameOver = true;
         std::cout << " Final Score: " << state.score << "\n";
     }
}

void checkLoseCondition(GameState& state, Engine::Registry& registry) {
    float deathLineY = CANNON_BASE_Y - BUBBLE_RADIUS * 3.0f;

    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            if (state.grid[row][col]) {
                float x, y;
                gridToScreen(row, col, x, y, state.descendOffset);
                
                if (y + BUBBLE_RADIUS >= deathLineY) {
                    std::cout << "\n*** GAME OVER *** Bubbles reached red line! Final Score: " 
                              << state.score << "\n";
                    std::cout << "Auto-resetting...\n\n";
                    
                    registry.clear();
                    
                    state.cannonAngle = -90.0f;
                    state.firing = false;
                    state.activeBubbleId = "";
                    state.timeSinceDescend = 0.0f;
                    state.descendOffset = 0.0f;
                    state.score = 0;
                    state.gameOver = false;
                    state.won = false;
                    state.shotCounter = 0;
                    
                    for (int r = 0; r < GRID_ROWS; r++) {
                        for (int c = 0; c < GRID_COLS; c++) {
                            state.grid[r][c] = false;
                        }
                    }
                    
                    initializeGrid(registry);
                    return;
                }
            }
        }
    }
}

void updateGame(Engine::Registry& registry, Engine::EventManager& eventManager,
                Timeline& timeline, float dt) {

    if (gGameState.firing && !gGameState.activeBubbleId.empty()) {
        auto* bubble = registry.get(gGameState.activeBubbleId);

        if (bubble && bubble->get<bool>("alive")) {
             float x  = bubble->get<float>("x");
             float y  = bubble->get<float>("y");
             float vx = bubble->get<float>("vx");
            float vy = bubble->get<float>("vy");

            x += vx * dt;
            y += vy * dt;

            if (x - BUBBLE_RADIUS < 0.0f || x + BUBBLE_RADIUS > SCREEN_WIDTH) {
                 vx = -vx;
                 x  = std::clamp(x, BUBBLE_RADIUS, SCREEN_WIDTH - BUBBLE_RADIUS);
             }
 
             bubble->set("x", x);
             bubble->set("y", y);
            bubble->set("vx", vx);
            bubble->set("vy", vy);

            bool collided = false;
             for (const auto& [id, _] : registry) {
                 if (id == gGameState.activeBubbleId) continue;
 
                 auto* other = registry.get(id);
                 if (!other || !other->get<bool>("static")) continue;
                 if (!other->get<bool>("alive")) continue;
 
                 float ox = other->get<float>("x");
                 float oy = other->get<float>("y");
                 float dx = x - ox;
                 float dy = y - oy;
                float dist = std::sqrt(dx * dx + dy * dy);

                if (dist < BUBBLE_RADIUS * 1.9f) {
                     collided = true;
                     break;
                 }
            }

            if (y - BUBBLE_RADIUS <= GRID_OFFSET_Y + gGameState.descendOffset) {
                 collided = true;
             }
 
             if (collided) {
                 handleBubbleCollision(registry, eventManager, timeline,
                                       gGameState.activeBubbleId);
                return;
            }

            if (y > SCREEN_HEIGHT + BUBBLE_RADIUS || y < -100.0f ||
                 x < -100.0f || x > SCREEN_WIDTH + 100.0f) {
                 registry.erase(gGameState.activeBubbleId);
                gGameState.firing = false;
                gGameState.activeBubbleId.clear();

                gGameState.nextBubbleColor = randomBubbleColor();
                 createBubble(registry, "current_bubble",
                              gGameState.cannonX, CANNON_BASE_Y - 30.0f,
                             gGameState.nextBubbleColor, false);
            }
        } else {
            gGameState.firing = false;
             gGameState.activeBubbleId.clear();
             if (!registry.get("current_bubble")) {
                 gGameState.nextBubbleColor = randomBubbleColor();
                 createBubble(registry, "current_bubble",
                              gGameState.cannonX, CANNON_BASE_Y - 30.0f,
                              gGameState.nextBubbleColor, false);
             }
        }
    }

   gGameState.timeSinceDescend += dt;
    if (gGameState.timeSinceDescend >= DESCEND_INTERVAL) {
       gGameState.timeSinceDescend = 0.0f;
       gGameState.descendOffset += DESCEND_SPEED;

       for (const auto& [id, _] : registry) {
            auto* bubble = registry.get(id);
            if (bubble && bubble->get<bool>("static")) {
                int row = bubble->get<int>("gridRow", -1);
                int col = bubble->get<int>("gridCol", -1);
                if (row >= 0 && col >= 0) {
                    float x, y;
                    gridToScreen(row, col, x, y, gGameState.descendOffset);
                    bubble->set("x", x);
                    bubble->set("y", y);
                }
            }
        }
        std::cout << "Bubbles descended: " << gGameState.descendOffset 
                  << "px (Time scale: " << timeline.scale() << "x)\n";
    }
 
      checkWinCondition(gGameState);
      checkLoseCondition(gGameState, registry);
}

void drawSimpleCircle(SDL_Renderer* renderer, SDL_Window* window, float cx, float cy,
                       float radius, SDL_Color color) {
     SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
     
     // Apply scaling to the center point and radius
     SDL_FRect logical = {cx - radius, cy - radius, radius * 2, radius * 2};
     SDL_FRect scaled = computeBubbleScaling(logical, window);
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

void renderGame(SDL_Renderer* renderer, SDL_Window* window, Engine::Registry& registry, GameState& state) {
    SDL_SetRenderDrawColor(renderer, 10, 10, 15, 255);
    SDL_RenderClear(renderer);
    
    // Draw game area background
    SDL_FRect gameAreaLogical = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
    SDL_FRect gameArea = computeBubbleScaling(gameAreaLogical, window);
    SDL_SetRenderDrawColor(renderer, 20, 20, 40, 255);
    SDL_RenderFillRect(renderer, &gameArea);
    
    // Draw game area border
    SDL_SetRenderDrawColor(renderer, 100, 100, 120, 255);
    SDL_RenderRect(renderer, &gameArea);

    for (const auto& [id, _] : registry) {
         auto* bubble = registry.get(id);
         if (!bubble || !bubble->get<bool>("alive")) continue;
         if (id == "current_bubble" && !state.firing) continue;
 
        float x      = bubble->get<float>("x");
        float y      = bubble->get<float>("y");
        float radius = bubble->get<float>("radius") * 0.9f;
        int   color  = bubble->get<int>("color");
 
         SDL_Color sdlColor = getColorFromEnum(static_cast<BubbleColor>(color));
        drawSimpleCircle(renderer, window, x, y, radius, sdlColor);
    }

    if (!state.firing) {
         SDL_Color nextColor = getColorFromEnum(state.nextBubbleColor);
         drawSimpleCircle(renderer, window, state.cannonX, CANNON_BASE_Y,
                         BUBBLE_RADIUS * 0.9f, nextColor);
    }

   if (!state.firing) {
         SDL_SetRenderDrawColor(renderer, 50, 200, 50, 255);
         float angleRad = state.cannonAngle * M_PI / 180.0f;
         float cosAngle = std::cos(angleRad);
         float sinAngle = std::sin(angleRad);
         for (float dist = 25.0f; dist < 200.0f; dist += 10.0f) {
             float dotX = state.cannonX + cosAngle * dist;
             float dotY = CANNON_BASE_Y + sinAngle * dist;
             SDL_FRect dotLogical = {dotX, dotY, 0, 0};
             SDL_FRect dotScaled = computeBubbleScaling(dotLogical, window);
             SDL_RenderPoint(renderer, dotScaled.x, dotScaled.y);
        }
    }

   SDL_SetRenderDrawColor(renderer, 255, 50, 50, 255);
   float deathLine = CANNON_BASE_Y - BUBBLE_RADIUS * 3.0f;
   SDL_FRect lineStart = {0, deathLine, 0, 0};
   SDL_FRect lineEnd = {SCREEN_WIDTH, deathLine, 0, 0};
   SDL_FRect scaledStart = computeBubbleScaling(lineStart, window);
   SDL_FRect scaledEnd = computeBubbleScaling(lineEnd, window);
   SDL_RenderLine(renderer, scaledStart.x, scaledStart.y, scaledEnd.x, scaledEnd.y);

   if (state.gameOver && state.won) {
        SDL_SetRenderDrawColor(renderer, 50, 255, 50, 255);
        float cx = SCREEN_WIDTH / 2.0f;
        float cy = SCREEN_HEIGHT / 2.0f;
        
        SDL_FRect p1 = {cx - 30, cy, 0, 0};
        SDL_FRect p2 = {cx - 10, cy + 20, 0, 0};
        SDL_FRect p3 = {cx + 30, cy - 30, 0, 0};
        
        SDL_FRect s1 = computeBubbleScaling(p1, window);
        SDL_FRect s2 = computeBubbleScaling(p2, window);
        SDL_FRect s3 = computeBubbleScaling(p3, window);
        
        SDL_RenderLine(renderer, s1.x, s1.y, s2.x, s2.y);
        SDL_RenderLine(renderer, s2.x, s2.y, s3.x, s3.y);
    }
 
     SDL_RenderPresent(renderer);
 }
 
 int main(int argc, char* argv[]) {
     if (SDL_Init(SDL_INIT_VIDEO) < 0) {
         std::cerr << "SDL init failed: " << SDL_GetError() << "\n";
         return 1;
     }
 
     SDL_Window* window = SDL_CreateWindow(
         "Bubble Shooter",
         static_cast<int>(SCREEN_WIDTH), static_cast<int>(SCREEN_HEIGHT),
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
    Engine::PoolAllocator<Engine::GameObject> bubblePool(200);
    Engine::Registry registry(&bubblePool);

    InputChord fastLaunchLeft("fast_launch_left",  {SDL_SCANCODE_SPACE, SDL_SCANCODE_A});
     InputChord fastLaunchRight("fast_launch_right",{SDL_SCANCODE_SPACE, SDL_SCANCODE_D});
     Input::registerChord(fastLaunchLeft);
     Input::registerChord(fastLaunchRight);
 
     initializeGrid(registry);
 
    bool running = true;
    bool prevSpace = false;
    bool prevP     = false;
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
            gameTime.togglePause();
            std::cout << (gameTime.isPaused() ? "PAUSED" : "RESUMED") << "\n";
        }
        prevP = pNow;

        bool k1Now = Input::isKeyPressed(SDL_SCANCODE_1);
        bool k2Now = Input::isKeyPressed(SDL_SCANCODE_2);
        bool k3Now = Input::isKeyPressed(SDL_SCANCODE_3);

        if (k1Now && !prev1) {
            gameTime.setScale(0.5);
            std::cout << "Time scale: 0.5x\n";
        }
        if (k2Now && !prev2) {
            gameTime.setScale(1.0);
            std::cout << "Time scale: 1.0x\n";
        }
        if (k3Now && !prev3) {
            gameTime.setScale(2.0);
            std::cout << "Time scale: 2.0x\n";
        }

        prev1 = k1Now;
        prev2 = k2Now;
        prev3 = k3Now;

        bool aPress = Input::isKeyPressed(SDL_SCANCODE_A);
        bool dPress = Input::isKeyPressed(SDL_SCANCODE_D);
        float rotateSpeed = 120.0f;

        double dtSec = gameTime.tick();
        if (dtSec <= 0.0) { SDL_Delay(1); continue; }
        float dt = static_cast<float>(dtSec);
 
        bool fastLaunchLeftActive  = Input::isChordActive("fast_launch_left");
        bool fastLaunchRightActive = Input::isChordActive("fast_launch_right");

        if ((fastLaunchLeftActive || fastLaunchRightActive) && !prevSpace && !gGameState.firing) {
            auto* preview = registry.get("current_bubble");
            if (preview) {
                BubbleColor color = static_cast<BubbleColor>(preview->get<int>("color"));
                std::string shotId = "shot_" + std::to_string(gGameState.shotCounter++);
                createBubble(registry, shotId, gGameState.cannonX, CANNON_BASE_Y - 30.0f, color, false);

                auto* proj = registry.get(shotId);
                if (proj) {
                    gGameState.firing = true;
                    gGameState.activeBubbleId = shotId;
                    float angleDeg = fastLaunchLeftActive ? -135.0f : -45.0f;
                    float angleRad = angleDeg * M_PI / 180.0f;
                    float speed = BUBBLE_SHOOT_SPEED * 1.5f;
                    proj->set("vx", std::cos(angleRad) * speed);
                    proj->set("vy", std::sin(angleRad) * speed);
                    prevSpace = true;
                    std::cout << "Fast launch " << (fastLaunchLeftActive ? "LEFT" : "RIGHT") << "!\n";
                }
            }
        }
        else if (!Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
            if (aPress) gGameState.cannonAngle = std::max(gGameState.cannonAngle - rotateSpeed * dt, -160.0f);
            if (dPress) gGameState.cannonAngle = std::min(gGameState.cannonAngle + rotateSpeed * dt, -20.0f);
        }

        bool spaceNow = Input::isKeyPressed(SDL_SCANCODE_SPACE);
        if (spaceNow && !prevSpace && !gGameState.firing && !fastLaunchLeftActive && !fastLaunchRightActive) {
            auto* preview = registry.get("current_bubble");
            if (preview) {
                BubbleColor color = static_cast<BubbleColor>(preview->get<int>("color"));
                std::string shotId = "shot_" + std::to_string(gGameState.shotCounter++);
                createBubble(registry, shotId, gGameState.cannonX, CANNON_BASE_Y - 30.0f, color, false);

                auto* proj = registry.get(shotId);
                if (proj) {
                    gGameState.firing = true;
                    gGameState.activeBubbleId = shotId;
                    float angleRad = gGameState.cannonAngle * M_PI / 180.0f;
                    proj->set("vx", std::cos(angleRad) * BUBBLE_SHOOT_SPEED);
                    proj->set("vy", std::sin(angleRad) * BUBBLE_SHOOT_SPEED);
                    std::cout << "Bubble fired at " << gGameState.cannonAngle << "°\n";
                }
            }
        }
        if (!spaceNow) prevSpace = false;

        updateGame(registry, eventManager, gameTime, dt);
        eventManager.dispatchEvents();

        renderGame(renderer, window, registry, gGameState);
     }
 
    std::cout << "\nGame Ended\n";
    std::cout << "  Final Score: " << gGameState.score << "\n";

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}