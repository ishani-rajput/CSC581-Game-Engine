 #include <SDL3/SDL.h>
 #include <SDL3/SDL_main.h>
 #include <iostream>
 #include <vector>
 #include <cmath>
 #include <random>
 #include <algorithm>
 #include <functional>
 
 // Engine includes
 #include "registry.h"
 #include "event_manager.h"
 #include "timeline.h"
 #include "input.h"
 #include "collision.h"
 #include "physics.h"
 #include "memory_pool.h"
 
 // Game constants
 constexpr float SCREEN_WIDTH = 800.0f;
 constexpr float SCREEN_HEIGHT = 600.0f;
 constexpr float BUBBLE_RADIUS = 20.0f;
 constexpr float CANNON_BASE_Y = SCREEN_HEIGHT - 50.0f;
 constexpr float BUBBLE_SHOOT_SPEED = 600.0f; // Slightly faster for better feel
 constexpr float GRID_OFFSET_X = 100.0f;
 constexpr float GRID_OFFSET_Y = 50.0f;
constexpr float DESCEND_SPEED = 20.0f;        // Pixels to descend
constexpr float DESCEND_INTERVAL = 10.0f;     // Seconds between descends (at 1.0x speed)
 constexpr int   GRID_COLS = 15;
 constexpr int   GRID_ROWS = 12;               // Increased rows slightly for buffer
 
 // Bubble colors (Min 2 required)
 enum class BubbleColor {
     Red,
     Blue,
     Green,
     Yellow,
     Purple,
     COUNT
 };
 
 // Game state
 struct GameState {
     float        cannonAngle     = -90.0f; // Degrees, -90 is straight up
     float        cannonX         = SCREEN_WIDTH / 2.0f;
     bool         firing          = false;
     std::string  activeBubbleId  = "";
     BubbleColor  nextBubbleColor = BubbleColor::Red;
     float        timeSinceDescend = 0.0f;
     float        descendOffset    = 0.0f;
     int          score            = 0;
     bool         gameOver         = false;
     bool         won              = false;
 
     // Grid tracking (for snap-to-grid logic)
     bool grid[GRID_ROWS][GRID_COLS] = {{false}};
 
     // NEW: unique IDs for each projectile shot
     int shotCounter = 0;
 };
 
 GameState   gGameState;
 std::mt19937 gRng(std::random_device{}());
 
 // Convert bubble color to SDL color
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
 
 // Random bubble color
 BubbleColor randomBubbleColor() {
     std::uniform_int_distribution<int> dist(0, static_cast<int>(BubbleColor::COUNT) - 1);
     return static_cast<BubbleColor>(dist(gRng));
 }
 
 // Convert grid position to screen position
 void gridToScreen(int row, int col, float& x, float& y, float descendOffset) {
     // Hexagonal grid offset
     float offsetX = (row % 2 == 1) ? BUBBLE_RADIUS : 0.0f;
     x = GRID_OFFSET_X + col * (BUBBLE_RADIUS * 2.0f) + offsetX;
     y = GRID_OFFSET_Y + row * (BUBBLE_RADIUS * 1.8f) + descendOffset;
 }
 
 // Convert screen position to grid position
 // Returns -1 in row if out of bounds (handled by caller)
 void screenToGrid(float x, float y, int& row, int& col, float descendOffset) {
     float adjustedY = y - descendOffset;
 
     // Find closest row
     row = static_cast<int>((adjustedY - GRID_OFFSET_Y) / (BUBBLE_RADIUS * 1.8f) + 0.5f);
     
     // Account for hexagonal offset
     float offsetX = (row % 2 == 1) ? BUBBLE_RADIUS : 0.0f;
     col = static_cast<int>((x - GRID_OFFSET_X - offsetX) / (BUBBLE_RADIUS * 2.0f) + 0.5f);
 }
 
 // Create a bubble using engine's Registry and GameObject
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
 
 // Initialize game grid with bubbles
 void initializeGrid(Engine::Registry& registry) {
     int bubbleCount = 0;
 
     // Create initial bubble grid (4 initial rows)
     for (int row = 0; row < 4; row++) {
         for (int col = 0; col < GRID_COLS; col++) {
             // Create checkerboard pattern with some randomness
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
 
     std::cout << "Grid initialized with " << bubbleCount << " bubbles\n";
 
     // Create next bubble preview at the cannon
     gGameState.nextBubbleColor = randomBubbleColor();
     createBubble(registry, "current_bubble",
                  gGameState.cannonX, CANNON_BASE_Y - 30.0f,
                  gGameState.nextBubbleColor, false);
 }
 
 // Find connected bubbles of same color (flood fill)
 void floodFill(Engine::Registry& registry, const std::string& startId,
                std::vector<std::string>& connected, BubbleColor targetColor) {
     auto* startBubble = registry.get(startId);
     if (!startBubble || !startBubble->get<bool>("alive")) return;
 
     int startColor = startBubble->get<int>("color");
     if (static_cast<BubbleColor>(startColor) != targetColor) return;
 
     // Mark as visited
     startBubble->set("visited", true);
     connected.push_back(startId);
 
     float x = startBubble->get<float>("x");
     float y = startBubble->get<float>("y");
 
     // Check all nearby bubbles (6 neighbors in hex grid)
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
 
         // If close enough (neighbor), recurse
         if (dist < BUBBLE_RADIUS * 2.5f) {
             floodFill(registry, id, connected, targetColor);
         }
     }
 }
 
 // Check for floating bubbles (not connected to top)
 void removeFloatingBubbles(Engine::Registry& registry, Engine::EventManager& eventManager,
                            Timeline& timeline) {
     // Clear visited flags
     for (const auto& [id, _] : registry) {
         auto* bubble = registry.get(id);
         if (bubble) bubble->set("visited", false);
     }
 
     // Mark all bubbles connected to top row
     for (int col = 0; col < GRID_COLS; col++) {
         float tx, ty;
         gridToScreen(0, col, tx, ty, gGameState.descendOffset);
 
         for (const auto& [id, _] : registry) {
             auto* bubble = registry.get(id);
             if (!bubble || !bubble->get<bool>("static") || !bubble->get<bool>("alive")) continue;
 
             float bx = bubble->get<float>("x");
             float by = bubble->get<float>("y");
             float dist = std::sqrt((tx - bx) * (tx - bx) + (ty - by) * (ty - by));
 
             // Check if bubble is roughly in the top row position
             if (dist < BUBBLE_RADIUS * 1.5f) {
                 // Start flood fill from here to mark all attached
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
 
     // Remove bubbles not connected to top
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
 
             gGameState.score += 5; // Bonus points
         }
         registry.erase(id);
     }
 
     if (!toRemove.empty()) {
         std::cout << "  > Removed " << toRemove.size() << " floating bubbles (bonus!)\n";
     }
 }
 
 // Handle bubble collision and matching
 void handleBubbleCollision(Engine::Registry& registry, Engine::EventManager& eventManager,
                            Timeline& timeline, const std::string& projectileId) {
     auto* projectile = registry.get(projectileId);
     if (!projectile) return;
 
     float px = projectile->get<float>("x");
     float py = projectile->get<float>("y");
     BubbleColor pColor = static_cast<BubbleColor>(projectile->get<int>("color"));
 
     // 1. Calculate Grid Position
     int gridRow, gridCol;
     screenToGrid(px, py, gridRow, gridCol, gGameState.descendOffset);
 
     // FIX: If the ball tries to land below the allowable grid rows, it's Game Over
     if (gridRow >= GRID_ROWS) {
         std::cout << "  > Bubble Overflow! Triggering Game Over.\n";
         gGameState.gameOver = true;
         gGameState.won = false;
         registry.erase(projectileId);
         return;
     }
 
     // Clamp columns only (rows are critical for game over)
     gridCol = std::clamp(gridCol, 0, GRID_COLS - 1);
     
     // Ensure we aren't overwriting an existing bubble (simple collision resolution)
     // If the calculated spot is taken, search upwards
     if (gridRow >= 0 && gGameState.grid[gridRow][gridCol]) {
         // Try one row up
         if (gridRow > 0) gridRow--;
         else {
             // Grid is full to the top - extremely rare but handled
             gGameState.gameOver = true; 
             return;
         }
     }
 
     if (gridRow < 0) gridRow = 0;
 
     // 2. Snap to grid and make STATIC (stick)
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
 
     // 3. Clear visited flags for flood fill
     for (const auto& [id, _] : registry) {
         auto* bubble = registry.get(id);
         if (bubble) bubble->set("visited", false);
     }
 
     // 4. Find connected bubbles of SAME color
     std::vector<std::string> connected;
     floodFill(registry, projectileId, connected, pColor);
 
     // 5. Pop only if 3 or more match
     if (connected.size() >= 3) {
         std::cout << "  > Matched " << connected.size() << " bubbles! POP!\n";
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
 
         // After popping, remove floating bubbles
         removeFloatingBubbles(registry, eventManager, timeline);
     } else {
         std::cout << "  > Stuck (cluster size: " << connected.size() << ")\n";
     }
 
     // 6. Reset firing state
     gGameState.firing = false;
     gGameState.activeBubbleId.clear();
 
     // 7. Create next preview bubble
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
 
 // Check win condition
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
         std::cout << "\n*** YOU WIN! *** Final Score: " << state.score << "\n";
     }
 }
 
// Check lose condition and auto-reset
void checkLoseCondition(GameState& state, Engine::Registry& registry) {
    // Check if the BOTTOM of any bubble touches or crosses the red line
    float deathLineY = CANNON_BASE_Y - BUBBLE_RADIUS * 3.0f;

    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            if (state.grid[row][col]) {
                float x, y;
                gridToScreen(row, col, x, y, state.descendOffset);
                
                // Check if bottom of bubble passes the death line
                if (y + BUBBLE_RADIUS >= deathLineY) {
                    std::cout << "\n*** GAME OVER *** Bubbles reached red line! Final Score: " 
                              << state.score << "\n";
                    std::cout << "[GAME] Auto-resetting...\n\n";
                    
                    // Clear all game objects
                    registry.clear();
                    
                    // Reset game state
                    state.cannonAngle = -90.0f;
                    state.firing = false;
                    state.activeBubbleId = "";
                    state.timeSinceDescend = 0.0f;
                    state.descendOffset = 0.0f;
                    state.score = 0;
                    state.gameOver = false;
                    state.won = false;
                    state.shotCounter = 0;
                    
                    // Clear grid
                    for (int r = 0; r < GRID_ROWS; r++) {
                        for (int c = 0; c < GRID_COLS; c++) {
                            state.grid[r][c] = false;
                        }
                    }
                    
                    // Reinitialize grid
                    initializeGrid(registry);
                    return;
                }
            }
        }
    }
}
 
// Update game logic
void updateGame(Engine::Registry& registry, Engine::EventManager& eventManager,
                Timeline& timeline, float dt) {
 
     // Update firing bubble physics
     if (gGameState.firing && !gGameState.activeBubbleId.empty()) {
         auto* bubble = registry.get(gGameState.activeBubbleId);
 
         // Safety check
         if (bubble && bubble->get<bool>("alive")) {
             float x  = bubble->get<float>("x");
             float y  = bubble->get<float>("y");
             float vx = bubble->get<float>("vx");
             float vy = bubble->get<float>("vy");
 
             // Physics integration
             x += vx * dt;
             y += vy * dt;
 
             // Wall bounce
             if (x - BUBBLE_RADIUS < 0.0f || x + BUBBLE_RADIUS > SCREEN_WIDTH) {
                 vx = -vx;
                 x  = std::clamp(x, BUBBLE_RADIUS, SCREEN_WIDTH - BUBBLE_RADIUS);
             }
 
             bubble->set("x", x);
             bubble->set("y", y);
             bubble->set("vx", vx);
             bubble->set("vy", vy);
 
             // Collision with static bubbles
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
 
                 // Check if bubbles are touching
                 if (dist < BUBBLE_RADIUS * 1.9f) {
                     collided = true;
                     break;
                 }
             }
 
             // Ceiling collision
             if (y - BUBBLE_RADIUS <= GRID_OFFSET_Y + gGameState.descendOffset) {
                 collided = true;
             }
 
             if (collided) {
                 handleBubbleCollision(registry, eventManager, timeline,
                                       gGameState.activeBubbleId);
                 return;
             }
 
             // Safety: off-screen
             if (y > SCREEN_HEIGHT + BUBBLE_RADIUS || y < -100.0f ||
                 x < -100.0f || x > SCREEN_WIDTH + 100.0f) {
                 registry.erase(gGameState.activeBubbleId);
                 gGameState.firing = false;
                 gGameState.activeBubbleId.clear();
 
                 // Create a new preview
                 gGameState.nextBubbleColor = randomBubbleColor();
                 createBubble(registry, "current_bubble",
                              gGameState.cannonX, CANNON_BASE_Y - 30.0f,
                              gGameState.nextBubbleColor, false);
             }
         } else {
             // Bubble is invalid, reset state
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
 
    // Periodic descend (affected by time scale via dt)
    // At 0.5x speed: descends slower, at 2.0x speed: descends faster
    gGameState.timeSinceDescend += dt;
    if (gGameState.timeSinceDescend >= DESCEND_INTERVAL) {
        gGameState.timeSinceDescend = 0.0f;
        gGameState.descendOffset += DESCEND_SPEED;

        // Update all static bubble positions
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
        std::cout << "[GAME] Bubbles descended! Offset: " << gGameState.descendOffset 
                  << "px (Time scale: " << timeline.scale() << "x)\n";
    }
 
      checkWinCondition(gGameState);
      checkLoseCondition(gGameState, registry);
 }
 
 // Optimized circle drawing
 void drawSimpleCircle(SDL_Renderer* renderer, float cx, float cy,
                       float radius, SDL_Color color) {
     SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
     int r = static_cast<int>(radius);
     int r2 = r * r;
     for (int dy = -r; dy <= r; dy++) {
         int dx = static_cast<int>(std::sqrt(std::max(r2 - dy * dy, 0)));
         SDL_RenderLine(renderer, cx - dx, cy + dy, cx + dx, cy + dy);
     }
 }
 
 // Render game
 void renderGame(SDL_Renderer* renderer, Engine::Registry& registry, GameState& state) {
     // Clear screen
     SDL_SetRenderDrawColor(renderer, 20, 20, 40, 255);
     SDL_RenderClear(renderer);
 
     // Draw grid bubbles
     for (const auto& [id, _] : registry) {
         auto* bubble = registry.get(id);
         if (!bubble || !bubble->get<bool>("alive")) continue;
         if (id == "current_bubble" && !state.firing) continue;
 
         float x      = bubble->get<float>("x");
         float y      = bubble->get<float>("y");
         float radius = bubble->get<float>("radius") * 0.9f; // Slight gap for visual clarity
         int   color  = bubble->get<int>("color");
 
         SDL_Color sdlColor = getColorFromEnum(static_cast<BubbleColor>(color));
         drawSimpleCircle(renderer, x, y, radius, sdlColor);
     }
 
     // Draw preview bubble
     if (!state.firing) {
         SDL_Color nextColor = getColorFromEnum(state.nextBubbleColor);
         drawSimpleCircle(renderer, state.cannonX, CANNON_BASE_Y,
                          BUBBLE_RADIUS * 0.9f, nextColor);
     }
 
    // Aim Line
    if (!state.firing) {
         SDL_SetRenderDrawColor(renderer, 50, 200, 50, 255);
         float angleRad = state.cannonAngle * M_PI / 180.0f;
         float cosAngle = std::cos(angleRad);
         float sinAngle = std::sin(angleRad);
         for (float dist = 25.0f; dist < 200.0f; dist += 10.0f) {
             float dotX = state.cannonX + cosAngle * dist;
             float dotY = CANNON_BASE_Y + sinAngle * dist;
             SDL_RenderPoint(renderer, dotX, dotY);
         }
     }
 
    // Draw Death Line (Red)
    SDL_SetRenderDrawColor(renderer, 255, 50, 50, 255);
    float deathLine = CANNON_BASE_Y - BUBBLE_RADIUS * 3.0f;
    SDL_RenderLine(renderer, 0, deathLine, SCREEN_WIDTH, deathLine);

    // Win indicator
    if (state.gameOver && state.won) {
        SDL_SetRenderDrawColor(renderer, 50, 255, 50, 255);
        float cx = SCREEN_WIDTH / 2.0f;
        float cy = SCREEN_HEIGHT / 2.0f;
        SDL_RenderLine(renderer, cx - 30, cy,     cx - 10, cy + 20);
        SDL_RenderLine(renderer, cx - 10, cy + 20, cx + 30, cy - 30);
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
 
     // Setup input chords
     InputChord fastLaunchLeft("fast_launch_left",  {SDL_SCANCODE_SPACE, SDL_SCANCODE_A});
     InputChord fastLaunchRight("fast_launch_right",{SDL_SCANCODE_SPACE, SDL_SCANCODE_D});
     Input::registerChord(fastLaunchLeft);
     Input::registerChord(fastLaunchRight);
 
     initializeGrid(registry);
 
    bool running = true;
    bool prevSpace = false;
    bool prevP     = false;
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

        // Pause
        bool pNow = Input::isKeyPressed(SDL_SCANCODE_P);
        if (pNow && !prevP) {
            gameTime.togglePause();
            std::cout << "[GAME] " << (gameTime.isPaused() ? "PAUSED" : "RESUMED") << "\n";
        }
        prevP = pNow;

        // Time scaling
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

        // Game controls
        bool aPress = Input::isKeyPressed(SDL_SCANCODE_A);
        bool dPress = Input::isKeyPressed(SDL_SCANCODE_D);
        float rotateSpeed = 120.0f;

        double dtSec = gameTime.tick();
        if (dtSec <= 0.0) { SDL_Delay(1); continue; }
        float dt = static_cast<float>(dtSec);
 
        bool fastLaunchLeftActive  = Input::isChordActive("fast_launch_left");
        bool fastLaunchRightActive = Input::isChordActive("fast_launch_right");

        // Fast launch logic
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
                    std::cout << "[CHORD] Fast launch " << (fastLaunchLeftActive ? "LEFT" : "RIGHT") << "!\n";
                }
            }
        }
        // Standard rotate
        else if (!Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
            if (aPress) gGameState.cannonAngle = std::max(gGameState.cannonAngle - rotateSpeed * dt, -160.0f);
            if (dPress) gGameState.cannonAngle = std::min(gGameState.cannonAngle + rotateSpeed * dt, -20.0f);
        }

        // Standard shoot
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
                    std::cout << "[ACTION] Bubble fired at " << gGameState.cannonAngle << "°\n";
                }
            }
        }
        if (!spaceNow) prevSpace = false;

        updateGame(registry, eventManager, gameTime, dt);
        eventManager.dispatchEvents();

        renderGame(renderer, registry, gGameState);
     }
 
    std::cout << "\n=== Game Ended ===\n";
    std::cout << "  Final Score: " << gGameState.score << "\n";

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}