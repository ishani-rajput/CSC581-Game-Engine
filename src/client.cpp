// src/client.cpp
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>

#include "entity.h"
#include "input.h"
#include "scaling.h"
#include "physics.h"
#include "collision.h"
#include "timeline.h"
#include "peer_manager.h"

// Engine-side (Part 1A integration)
#include "net_strategy.h"
#include "registry.h"
#include "object_model.h"

#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <zmq.h>

using Engine::NetStrategy;

static const int WINDOW_WIDTH  = 1920;
static const int WINDOW_HEIGHT = 1080;

static const char* PLAYER_ASSET   = "../assets/player.png";
static const char* GHOST_ASSET    = "../assets/ghost.png";
static const char* PLATFORM_ASSET = "../assets/platform.png";
static const char* GRAVE_ASSET    = "../assets/grave.png";
static const char* BG_SKY_ASSET   = "../assets/background_sky.png";

// ------------------------- helpers -----------------------------------------

static inline bool AABB(float ax,float ay,float aw,float ah,
                        float bx,float by,float bw,float bh) {
    return (ax < bx + bw) && (ax + aw > bx) &&
           (ay < by + bh) && (ay + ah > by);
}

static inline float snapToStep(float x, float step) {
    return std::floor(x / step) * step;
}

static int numericIdFrom(const std::string& s) {
    int n = 0; bool any = false;
    for (char c : s) if (std::isdigit((unsigned char)c)) { any = true; n = n*10 + (c - '0'); }
    if (!any) n = 1 + (rand() % 20);
    if (n < 1) n = 1;
    if (n > 20) n = 1 + (n % 20);
    return n;
}

struct LocalPlayer {
    float x = 100.f;
    float y = WINDOW_HEIGHT - 322.f;
    float vx = 0.f;
    float vy = 0.f;
    bool onGround = false;
};

struct GhostState {
    float x=100.f, y=WINDOW_HEIGHT-322.f, vx=0.f, vy=0.f;
    bool onGround=true;
};

// ------------------------- Part 2 additions --------------------------------

struct Camera { float x = 0.f; };

struct StaticPlat {
    float wx, wy, ww, wh;      // world rect for collision
    Entity* sprite = nullptr;  // optional visual (used for ground tiling base)
};

enum class MoveKind { HorizontalSine, Circular };

struct MovingPlat {
    float wx, wy, ww, wh;   // world coords (updated per frame)
    MoveKind kind;
    float cx=0, cy=0, amp=0, speed=1.0f, radius=0; // motion params
    Entity* sprite = nullptr;

    // track previous world position to carry player
    float prevx=0.f, prevy=0.f;
};

struct Rect { float x,y,w,h; };

// ---- Tiled visual platforms (no stretch) ----
struct Tile {
    float baseX, baseY;
    Entity* e;
};
struct PlatSpan {
    Rect hitbox;                 // single collision rect
    std::vector<Tile> tiles;     // visual tiles (256x60)
};

static PlatSpan makeSpan(SDL_Renderer* r, float wx, float wy, float widthPx) {
    PlatSpan span;
    span.hitbox = { wx, wy, widthPx, 60.f };
    int tiles = (int)std::ceil(widthPx / 256.f);
    for (int i = 0; i < tiles; ++i) {
        float tx = wx + i * 256.f;
        Entity* e = new Entity(r, PLATFORM_ASSET, (int)tx, (int)wy, 256, 60, 1, 0);
        span.tiles.push_back(Tile{tx, wy, e});
    }
    return span;
}

// ----------------- Level extents & player size (NEW) -----------------------
static const float LEVEL_WIDTH = 3600.f;   // fits spawns up to ~3300
static const float PLAYER_W    = 256.f;
static const float PLAYER_H    = 256.f;

// ---- Pit position moved away from upper platforms (NEW) ----
static const float PIT_X = 2400.f;                 // was 1800.f
static const float PIT_W = 360.f;                  // was 150.f
static const float PIT_Y = (float)WINDOW_HEIGHT - 120.f;
static const float PIT_H = 120.f;

// Runtime containers (file-scope)
static std::vector<StaticPlat> gStatic;   // ground collision (pieces)
static std::vector<MovingPlat> gMoving;
static std::vector<PlatSpan>  gSpans;     // fixed tiled platforms
static std::vector<Engine::Vec2> gSpawns; // hidden spawn points (object model)
static int gSpawnIndex = 0;
static std::vector<Rect> gDeathZones;     // hidden death zones
static Camera gCam;

// Camera-relative scroll triggers (computed each frame)
static const float kScrollStep   = 640.f;               // camera hop per trigger
static const float kRightOffset  = 1500.f;              // px from camera-left
static const float kLeftOffset   = 200.f;               // px from camera-left
static const float kTriggerWidth = 60.f;
static Rect gScrollRight;
static Rect gScrollLeft;

// Engine-side runtime registry
static Engine::Registry gRegistry;

// ------------------------- main --------------------------------------------

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Witch Runner (Hybrid P2P Client)",
                                     WINDOW_WIDTH, WINDOW_HEIGHT,
                                     SDL_WINDOW_RESIZABLE,
                                     &window, &renderer)) return 1;

    // Engine setup
    Scaling::setMode(ScaleMode::Pixel);
    Physics::setGravity(2000.f);
    srand((unsigned)time(nullptr));

    // choose network strategy from env
    NetStrategy strat = NetStrategy::FullState;
    if (const char* s = std::getenv("NET_STRATEGY")) {
        if (std::string(s) == "input") strat = NetStrategy::InputDelta;
    }

    // networking bootstrap
    const std::string myId = "client_" + std::to_string(1000 + (rand()%9000));
    PeerManager peerManager(myId);

    // REQ/REP handshake with host
    peerManager.connectToServer("tcp://127.0.0.1:5555");
    peerManager.sendToServer("CONNECT");
    std::string resp = peerManager.receiveFromServer();
    if (resp.find("CONNECTED") == std::string::npos && resp.find("GHOST") == std::string::npos) return 1;
    std::cout << "Connected as " << myId << "\n";

    // raw ZMQ context for SUB sockets (ghost + peer bus)
    void* ctx = zmq_ctx_new();

    // subscribe to server-auth ghost
    void* ghostSub = zmq_socket(ctx, ZMQ_SUB);
    zmq_connect(ghostSub, "tcp://127.0.0.1:5556");
    zmq_setsockopt(ghostSub, ZMQ_SUBSCRIBE, "", 0);

    // start our PUB and connect to others' PUB
    const int myNum = numericIdFrom(myId);
    const int myPubPort = 7000 + myNum;
    {
        std::ostringstream oss; oss << "tcp://*:" << myPubPort;
        peerManager.startPeerListener(oss.str());
    }
    for (int i = 1; i <= 20; ++i) {
        if (i == myNum) continue;
        std::ostringstream ep; ep << "tcp://localhost:" << (7000 + i);
        peerManager.connectToPeerNetwork(ep.str());
    }

    // raw SUB to receive peers' messages (POSE/PLAYER/INPUT)
    void* rawPeerSub = zmq_socket(ctx, ZMQ_SUB);
    for (int i = 1; i <= 20; ++i) {
        if (i == myNum) continue;
        std::ostringstream ep; ep << "tcp://127.0.0.1:" << (7000 + i);
        zmq_connect(rawPeerSub, ep.str().c_str());
    }
    zmq_setsockopt(rawPeerSub, ZMQ_SUBSCRIBE, "", 0);

    // scene assets
    SDL_Texture* bgSky = IMG_LoadTexture(renderer, BG_SKY_ASSET);
    Entity groundE(renderer, PLATFORM_ASSET, 0, 950, 1920, 130, 1, 0);
    Entity graveE(renderer, GRAVE_ASSET, 700, 700, 256, 256, 1, 0);
    Entity ghostE(renderer, GHOST_ASSET, 1500, 600, 256, 256, 1, 0);
    Entity playerE(renderer, PLAYER_ASSET, 100, WINDOW_HEIGHT - 322.f, 256, 256, 1, 0);

    // ===== World setup ======================================================
    // (initial ground collider will be replaced after death zones are defined)
    gStatic.push_back({ -100000.f, 950.f, 200000.f, 130.f, &groundE });

    // Fixed tiled platforms (higher & centered)
    const float midW = 512.f;
    const float midX = (WINDOW_WIDTH - midW) * 0.5f;
    const float midY = 520.f;
    gSpans.push_back(makeSpan(renderer, midX, midY, midW));

    const float rightW = 512.f;
    const float rightX = 1800.f;
    const float rightY = 460.f;
    gSpans.push_back(makeSpan(renderer, rightX, rightY, rightW));

    // Moving platforms
    gMoving.push_back(MovingPlat{
        /*wx*/1500.f, /*wy*/(float)WINDOW_HEIGHT-420.f, /*ww*/260.f, /*wh*/60.f,
        MoveKind::HorizontalSine, /*cx*/1500.f, /*cy*/(float)WINDOW_HEIGHT-420.f,
        /*amp*/180.f, /*speed*/1.5f, /*radius*/0.f,
        new Entity(renderer, PLATFORM_ASSET, 1500, WINDOW_HEIGHT-420, 260, 60, 1, 0)
    });
    gMoving.push_back(MovingPlat{
        /*wx*/3000.f, /*wy*/(float)WINDOW_HEIGHT-380.f, /*ww*/260.f, /*wh*/60.f,
        MoveKind::Circular, /*cx*/3000.f, /*cy*/(float)WINDOW_HEIGHT-450.f,
        /*amp*/0.f, /*speed*/1.3f, /*radius*/120.f,
        new Entity(renderer, PLATFORM_ASSET, 3000, WINDOW_HEIGHT-380, 260, 60, 1, 0)
    });
    for (auto& m : gMoving) { m.prevx = m.wx; m.prevy = m.wy; }

    // Hidden spawn points (object-model, not rendered)
    gSpawns = {
        {  300.f, (float)WINDOW_HEIGHT-350.f },
        { 1400.f, (float)WINDOW_HEIGHT-350.f },
        { 2500.f, (float)WINDOW_HEIGHT-350.f },
        { 3300.f, (float)WINDOW_HEIGHT-350.f },
    };
    for (int i=0;i<(int)gSpawns.size();++i) {
        auto& sp = gRegistry.upsert("spawn_"+std::to_string(i));
        sp.set<Engine::Vec2>("pos", gSpawns[i]);
        sp.onUpdate([](Engine::GameObject&, float){});
    }
    gSpawnIndex = 0;

    // Hidden death zones (fixed world zones + global bottom)  --- UPDATED PIT
    gDeathZones = {
        { PIT_X, PIT_Y, PIT_W, PIT_H },                                  // pit moved to 2400
        { -10000.f, (float)WINDOW_HEIGHT+5.f, 30000.f, 5000.f }          // global bottom
    };

    // --- REPLACE the old ground collider with two pieces around the pit (NEW)
    gStatic.clear();
    const float GY = 950.f;     // ground Y
    const float GH = 130.f;     // ground H
    const float pitStart = gDeathZones[0].x;
    const float pitEnd   = gDeathZones[0].x + gDeathZones[0].w;

    // Left ground piece (up to pit)
    gStatic.push_back({ -100000.f, GY, pitStart - (-100000.f), GH, &groundE });
    // Right ground piece (after pit)
    gStatic.push_back({ pitEnd,    GY, 200000.f - pitEnd,      GH, nullptr });

    // map of on-screen entities for each player id
    std::unordered_map<std::string, Entity*> players;
    players[myId] = &playerE;
    std::unordered_map<std::string, Engine::Vec2> playerWorld;

    // last-heard timestamps for disconnect culling
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastHeard;
    lastHeard[myId] = std::chrono::steady_clock::now();

    // gameplay state
    LocalPlayer me;
    me.x = gSpawns[gSpawnIndex].x;
    me.y = gSpawns[gSpawnIndex].y;

    Timeline myTime; myTime.anchorToRealTime(); myTime.setScale(1.0);

    // seed engine-side object for this player
    {
        auto& meGO = gRegistry.upsert(myId);
        meGO.set<Engine::Vec2>("pos", {me.x, me.y});
        meGO.set<Engine::Vec2>("vel", {0.f, 0.f});
        meGO.onUpdate([](Engine::GameObject& o, float dt){
            auto p = o.get<Engine::Vec2>("pos");
            auto v = o.get<Engine::Vec2>("vel");
            p.x += v.x * dt; p.y += v.y * dt;
            o.set("pos", p);
        });
    }

    bool paused = false, prevT = false, prevJump = false;
    bool running = true;

    // Debug overlay toggle
    bool showDebug = false;
    static bool prevF1 = false;

    float ghostX = 1500.f, ghostY = 600.f; // world coords
    SDL_Event ev;

    auto sendPose = [&](float px, float py) {
        peerManager.updateMyPlayerData(px, py, paused, (float)myTime.scale());
    };
    auto sendInputDelta = [&](bool L, bool R, bool J) {
        peerManager.sendInputDelta(L, R, J, 0.f, 0.f, SDL_GetTicks());
    };

    auto lastCull = std::chrono::steady_clock::now();
    std::unordered_map<std::string, GhostState> ghostSim;

    float scrollCooldown = 0.f;

    // flappy-style params
    const float JumpImpulse = 1100.f;
    const float MaxUpSpeed  = -1500.f;

    // Camera clamp helper (NEW)
    auto clampCam = [&](){
        float maxCam = std::max(0.f, LEVEL_WIDTH - (float)WINDOW_WIDTH);
        if (gCam.x < 0.f) gCam.x = 0.f;
        if (gCam.x > maxCam) gCam.x = maxCam;
    };

    // --- NEW: Initialize camera snapped to the starting spawn frame
    {
        float desiredCam = me.x - (WINDOW_WIDTH * 0.5f);
        if (desiredCam < 0.f) desiredCam = 0.f;
        gCam.x = snapToStep(desiredCam, kScrollStep);
        clampCam();
    }

    // ----------------------- main loop -------------------------------------
    while (running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
        }
        Input::poll();

        // Toggle debug overlay with F1
        {
            bool f1Now = Input::isKeyPressed(SDL_SCANCODE_F1);
            if (f1Now && !prevF1) showDebug = !showDebug;
            prevF1 = f1Now;
        }

        bool tNow = Input::isKeyPressed(SDL_SCANCODE_T);
        if (tNow && !prevT) {
            auto current = Scaling::mode();
            Scaling::setMode(current == ScaleMode::Pixel ? ScaleMode::Proportional : ScaleMode::Pixel);
        }
        prevT = tNow;

        if (Input::isKeyPressed(SDL_SCANCODE_P)) { paused = !paused; myTime.pause(paused); }
        if (Input::isKeyPressed(SDL_SCANCODE_1)) myTime.setScale(0.5);
        if (Input::isKeyPressed(SDL_SCANCODE_2)) myTime.setScale(1.0);
        if (Input::isKeyPressed(SDL_SCANCODE_3)) myTime.setScale(2.0);

        float dt = (float)myTime.tick();

        // Flappy-style movement
        float desiredVx = 0.f;
        if (Input::isKeyPressed(SDL_SCANCODE_A)) desiredVx = -300.f;
        else if (Input::isKeyPressed(SDL_SCANCODE_D)) desiredVx =  300.f;

        bool jumpNow = Input::isKeyPressed(SDL_SCANCODE_SPACE);
        bool wantJump = (jumpNow && !prevJump);
        prevJump = jumpNow;

        me.vx = desiredVx;
        if (wantJump) {
            me.vy = -JumpImpulse;
            if (me.vy < MaxUpSpeed) me.vy = MaxUpSpeed;
        }
        me.vy += Physics::gravity() * dt;
        me.x  += me.vx * dt;
        me.y  += me.vy * dt;

        if (me.y < 0) { me.y = 0; me.vy = 0; }

        // Move moving platforms, track deltas
        for (auto& m : gMoving) { m.prevx = m.wx; m.prevy = m.wy; }
        static float tAccum = 0.f;
        tAccum += dt;
        for (auto& m : gMoving) {
            if (m.kind == MoveKind::HorizontalSine) {
                float phase = tAccum * m.speed;
                m.wx = m.cx + std::sinf(phase) * m.amp;
            } else if (m.kind == MoveKind::Circular) {
                float ang = tAccum * m.speed;
                m.wx = m.cx + std::cosf(ang) * m.radius;
                m.wy = m.cy + std::sinf(ang) * m.radius;
            }
        }

        // Collisions
        me.onGround = false;

        auto collideRect = [&](float rx,float ry,float rw,float rh, MovingPlat* moving){
            float pw = PLAYER_W, ph = PLAYER_H;
            if (!AABB(me.x, me.y, pw, ph, rx, ry, rw, rh)) return;

            float prevY = me.y - me.vy * dt;
            float prevBottom = prevY + ph;
            float platTop = ry;
            const float EPS = 1.0f;

            bool falling = me.vy > 0.f;
            bool wasAbove = (prevBottom <= platTop + 4.f);
            if (falling && wasAbove) {
                me.y = platTop - ph;
                me.vy = 0.f;
                me.onGround = true;
                if (moving) { // carry with moving platform
                    me.x += (moving->wx - moving->prevx);
                    me.y += (moving->wy - moving->prevy);
                }
                return;
            }

            float overlapLeft   = (me.x + pw) - rx;
            float overlapRight  = (rx + rw) - me.x;
            float overlapTop    = (me.y + ph) - ry;
            float overlapBottom = (ry + rh) - me.y;

            float minX = std::min(overlapLeft, overlapRight);
            float minY = std::min(overlapTop, overlapBottom);

            if (minX < minY) {
                if (overlapLeft < overlapRight) me.x = rx - pw - EPS;
                else                             me.x = rx + rw + EPS;
                me.vx = 0.f;
            } else {
                if (overlapTop < overlapBottom) {
                    me.y = ry - ph - EPS; me.vy = 0.f; me.onGround = true;
                } else {
                    me.y = ry + rh + EPS; if (me.vy < 0.f) me.vy = 0.f;
                }
            }
        };

        // ground collision (two pieces; true gap over pit)
        for (auto& s : gStatic) collideRect(s.wx, s.wy, s.ww, s.wh, nullptr);
        // fixed spans
        for (auto& span : gSpans) collideRect(span.hitbox.x, span.hitbox.y, span.hitbox.w, span.hitbox.h, nullptr);
        // movers
        for (auto& m : gMoving) collideRect(m.wx, m.wy, m.ww, m.wh, &m);

        // gentle friction when grounded
        if (me.onGround) {
            const float F = 1500.f;
            if      (me.vx > 0.f) me.vx = std::max(0.f, me.vx - F*dt);
            else if (me.vx < 0.f) me.vx = std::min(0.f, me.vx + F*dt);
        }

        // Clamp player inside finite level (NEW)
        if (me.x < 0.f) me.x = 0.f;
        if (me.x > LEVEL_WIDTH - PLAYER_W) me.x = LEVEL_WIDTH - PLAYER_W;

        auto snapCameraToPlayer = [&](){
            float desiredCam = me.x - (WINDOW_WIDTH * 0.5f); // center player
            if (desiredCam < 0.f) desiredCam = 0.f;
            gCam.x = snapToStep(desiredCam, kScrollStep);
            clampCam();
            scrollCooldown = 0.2f; // prevent immediate retrigger
        };

        // Grave/Ghost reset to spawn 0 (with camera snap)
        if (AABB(me.x,me.y,PLAYER_W,PLAYER_H,700,700,256,256) ||
            AABB(me.x,me.y,PLAYER_W,PLAYER_H,ghostX,ghostY,256,256)) {
            gSpawnIndex = 0;
            me.x = gSpawns[gSpawnIndex].x;
            me.y = gSpawns[gSpawnIndex].y;
            me.vx = me.vy = 0;
            snapCameraToPlayer(); // NEW
        }

        // Death zones -> next spawn (fixed zones) + camera snap
        bool teleported = false;
        for (auto& dz : gDeathZones) {
            if (AABB(me.x, me.y, PLAYER_W,PLAYER_H, dz.x, dz.y, dz.w, dz.h)) {
                gSpawnIndex = (gSpawnIndex + 1) % (int)gSpawns.size();
                me.x = gSpawns[gSpawnIndex].x;
                me.y = gSpawns[gSpawnIndex].y;
                me.vx = me.vy = 0;
                teleported = true;
                snapCameraToPlayer(); // NEW
                break;
            }
        }
        // Optional: camera-following pit near right side of view (keep within level)
        if (!teleported) {
            Rect dynamicPit = { gCam.x + 1400.f, (float)WINDOW_HEIGHT - 120.f, 200.f, 120.f };
            if (!(dynamicPit.x + dynamicPit.w <= 0.f || dynamicPit.x >= LEVEL_WIDTH)) {
                if (AABB(me.x, me.y, PLAYER_W,PLAYER_H, dynamicPit.x, dynamicPit.y, dynamicPit.w, dynamicPit.h)) {
                    gSpawnIndex = (gSpawnIndex + 1) % (int)gSpawns.size();
                    me.x = gSpawns[gSpawnIndex].x;
                    me.y = gSpawns[gSpawnIndex].y;
                    me.vx = me.vy = 0;
                    snapCameraToPlayer(); // NEW
                }
            }
        }

        // ===== Side-scrolling boundaries (camera-relative, both directions) =
        if (scrollCooldown > 0.f) scrollCooldown -= dt;

        // Recompute trigger rects from camera each frame (world-space)
        gScrollRight = { gCam.x + kRightOffset, 0.f, kTriggerWidth, (float)WINDOW_HEIGHT };
        gScrollLeft  = { gCam.x + kLeftOffset  - kTriggerWidth, 0.f, kTriggerWidth, (float)WINDOW_HEIGHT };

        // Player’s world rect
        Rect pr = { me.x, me.y, PLAYER_W, PLAYER_H };

        // Right scroll
        if (scrollCooldown <= 0.f && AABB(pr.x, pr.y, pr.w, pr.h,
                                          gScrollRight.x, gScrollRight.y, gScrollRight.w, gScrollRight.h)) {
            gCam.x += kScrollStep;
            clampCam();
            scrollCooldown = 0.15f;
        }
        // Left scroll
        if (scrollCooldown <= 0.f && AABB(pr.x, pr.y, pr.w, pr.h,
                                          gScrollLeft.x, gScrollLeft.y, gScrollLeft.w, gScrollLeft.h)) {
            gCam.x -= kScrollStep;
            clampCam();
            scrollCooldown = 0.15f;
        }

        // keep engine object in sync
        {
            auto& go = gRegistry.upsert(myId);
            go.set<Engine::Vec2>("pos", {me.x, me.y});
            go.set<Engine::Vec2>("vel", {me.vx, me.vy});
        }

        // network update (world coords)
        if (strat == NetStrategy::FullState) {
            peerManager.updateMyPlayerData(me.x, me.y, paused, (float)myTime.scale());
        } else {
            bool L = Input::isKeyPressed(SDL_SCANCODE_A);
            bool R = Input::isKeyPressed(SDL_SCANCODE_D);
            bool J = wantJump;
            peerManager.sendInputDelta(L, R, J, 0.f, 0.f, SDL_GetTicks());
        }

        // consume ghost
        {
            char gbuf[128];
            int n = zmq_recv(ghostSub, gbuf, sizeof(gbuf)-1, ZMQ_DONTWAIT);
            if (n > 0) {
                gbuf[n] = 0;
                std::istringstream iss{std::string(gbuf)};
                std::string tag; iss >> tag;
                if (tag == "GHOST") iss >> ghostX >> ghostY;
            }
        }

        // consume peer updates
        {
            for (int i = 0; i < 32; ++i) {
                char mbuf[512];
                int n = zmq_recv(rawPeerSub, mbuf, sizeof(mbuf)-1, ZMQ_DONTWAIT);
                if (n <= 0) break;
                mbuf[n] = 0;
                std::istringstream iss{std::string(mbuf)};
                std::string tag; iss >> tag;

                if (tag == "POSE" || tag == "PLAYER") {
                    std::string pid; float px=0, py=0;
                    iss >> pid >> px >> py;
                    if (!pid.empty() && pid != myId) {
                        auto now = std::chrono::steady_clock::now();
                        playerWorld[pid] = {px, py};
                        auto& go = gRegistry.upsert(pid);
                        go.set<Engine::Vec2>("pos", {px, py});
                        if (players.find(pid) == players.end()) {
                            players[pid] = new Entity(renderer, PLAYER_ASSET, px, py, 256, 256, 1, 0);
                        }
                        lastHeard[pid] = now;
                    }
                } else if (tag == "INPUT") {
                    std::string pid; int L,R,J; float ax,ay; uint64_t ts;
                    iss >> pid >> L >> R >> J >> ax >> ay >> ts;
                    if (!pid.empty() && pid != myId) {
                        auto now = std::chrono::steady_clock::now();
                        GhostState& st = ghostSim[pid];
                        float rdt = 0.016f;
                        st.vx = (L?-400.f:0.f) + (R?400.f:0.f);
                        if (J && st.onGround) { st.vy = -900.f; st.onGround = false; }
                        st.vy += Physics::gravity() * rdt;
                        st.x  += st.vx * rdt;
                        st.y  += st.vy * rdt;
                        // collide with huge ground pieces
                        for (auto& s : gStatic) {
                            if (AABB(st.x,st.y,PLAYER_W,PLAYER_H, s.wx,s.wy,s.ww,s.wh)) {
                                st.vy=0; st.y=s.wy-PLAYER_H; st.onGround=true;
                            }
                        }
                        if (st.x < 0) st.x = 0;
                        if (st.x > LEVEL_WIDTH-PLAYER_W) st.x = LEVEL_WIDTH-PLAYER_W;

                        playerWorld[pid] = {st.x, st.y};
                        auto& go = gRegistry.upsert(pid);
                        go.set<Engine::Vec2>("pos", {st.x, st.y});
                        go.set<Engine::Vec2>("vel", {st.vx, st.vy});
                        if (players.find(pid) == players.end()) {
                            players[pid] = new Entity(renderer, PLAYER_ASSET, st.x, st.y, 256, 256, 1, 0);
                        }
                        lastHeard[pid] = now;
                    }
                }
            }
        }

        // disconnect culling
        {
            auto now = std::chrono::steady_clock::now();
            static auto lastCullTick = now;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCull).count() > 500) {
                std::vector<std::string> toErase;
                for (auto& kv : players) {
                    const std::string& pid = kv.first;
                    if (pid == myId) continue;
                    auto it = lastHeard.find(pid);
                    if (it == lastHeard.end()) { toErase.push_back(pid); continue; }
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() > 2) {
                        toErase.push_back(pid);
                    }
                }
                for (const auto& pid : toErase) {
                    if (players.count(pid)) { delete players[pid]; players.erase(pid); }
                    gRegistry.erase(pid);
                    playerWorld.erase(pid);
                    ghostSim.erase(pid);
                    lastHeard.erase(pid);
                }
                lastCull = now;
            }
        }

        // --- render ---------------------------------------------------------
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (bgSky) SDL_RenderTexture(renderer, bgSky, nullptr, nullptr);

        // Ground (finite, with pit visual hole)
        {
            const int   groundTileW = 1920;
            const float groundY     = GY;

            // Only draw tiles that overlap the visible screen and the level bounds
            float screenL = gCam.x;
            float firstTileX = std::floor(screenL / groundTileW) * groundTileW;

            // Pit visual range (world)
            const float pitStartWorld = gDeathZones[0].x;
            const float pitEndWorld   = gDeathZones[0].x + gDeathZones[0].w;

            for (int i = -1; i <= 2; ++i) {
                float gx = firstTileX + i * groundTileW;     // world tile x
                float gxEnd = gx + groundTileW;

                // Skip tiles completely outside level bounds
                if (gxEnd <= 0.f || gx >= LEVEL_WIDTH) continue;

                float dx = gx - gCam.x;                      // screen x
                groundE.setPosition(dx, groundY);
                groundE.update();
                groundE.render(renderer, window);

                // Punch the pit hole visually
                if (bgSky) {
                    float holeX0 = std::max(dx,               pitStartWorld - gCam.x);
                    float holeX1 = std::min(dx + groundTileW, pitEndWorld   - gCam.x);
                    if (holeX1 > holeX0) {
                        SDL_FRect dst = { holeX0, groundY, holeX1 - holeX0, GH };
                        SDL_RenderTexture(renderer, bgSky, nullptr, &dst);
                    }
                }
            }
        }

        // Fixed tiled platforms
        for (auto& span : gSpans) {
            for (auto& t : span.tiles) {
                t.e->setPosition(t.baseX - gCam.x, t.baseY);
                t.e->update(); t.e->render(renderer, window);
            }
        }

        // Moving platforms
        for (auto& m : gMoving) {
            m.sprite->setPosition(m.wx - gCam.x, m.wy);
            m.sprite->update(); m.sprite->render(renderer, window);
        }

        // Grave & ghost
        graveE.setPosition(700.f - gCam.x, 700.f);
        ghostE.setPosition(ghostX - gCam.x, ghostY);
        graveE.update(); graveE.render(renderer, window);
        ghostE.update(); ghostE.render(renderer, window);

        // Local player
        playerE.setPosition(me.x - gCam.x, me.y);
        playerE.update(); playerE.render(renderer, window);

        // Remote players
        for (auto& kv : players) {
            const std::string& pid = kv.first;
            if (pid == myId) continue;
            auto it = playerWorld.find(pid);
            if (it != playerWorld.end()) {
                kv.second->setPosition(it->second.x - gCam.x, it->second.y);
            }
            kv.second->update();
            kv.second->render(renderer, window);
        }

        // ---------------- Debug overlay -------------------------------------
        if (showDebug) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

            // Spawns: green semi-transparent 50x50 squares
            SDL_SetRenderDrawColor(renderer, 0, 255, 0, 100);
            for (const auto& sp : gSpawns) {
                SDL_FRect r = { sp.x - gCam.x, sp.y, 50.f, 50.f };
                SDL_RenderFillRect(renderer, &r);
            }

            // Fixed death zones: red semi-transparent (pit now at PIT_X..PIT_X+PIT_W)
            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 100);
            for (const auto& dz : gDeathZones) {
                SDL_FRect r = { dz.x - gCam.x, dz.y, dz.w, dz.h };
                SDL_RenderFillRect(renderer, &r);
            }

            // Dynamic camera-following pit: blue semi-transparent
            SDL_SetRenderDrawColor(renderer, 0, 0, 255, 100);
            SDL_FRect dyn = { (gCam.x + 1400.f) - gCam.x, (float)WINDOW_HEIGHT - 120.f, 200.f, 120.f };
            SDL_RenderFillRect(renderer, &dyn);

            // Outline current spawn
            if (!gSpawns.empty()) {
                SDL_SetRenderDrawColor(renderer, 0, 200, 0, 255);
                SDL_FRect r = { gSpawns[gSpawnIndex].x - gCam.x, gSpawns[gSpawnIndex].y, 50.f, 50.f };
                SDL_RenderRect(renderer, &r);
            }
        }
        // --------------------------------------------------------------------

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    // cleanup
    for (auto& span : gSpans) for (auto& t : span.tiles) delete t.e;
    for (auto& m : gMoving) if (m.sprite) delete m.sprite;
    if (bgSky) SDL_DestroyTexture(bgSky);

    zmq_close(rawPeerSub);
    zmq_close(ghostSub);
    zmq_ctx_term(ctx);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

