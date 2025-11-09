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

#include "net_strategy.h"
#include "registry.h"
#include "object_model.h"
#include "event_manager.h"

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

struct ReplayFrame {
    float t;       
    float px, py;  
    float camx;    
};

struct Camera { float x = 0.f; };

struct StaticPlat {
    float wx, wy, ww, wh;      
    Entity* sprite = nullptr; 
};

enum class MoveKind { HorizontalSine, Circular };

struct MovingPlat {
    float wx, wy, ww, wh;   
    MoveKind kind;
    float cx=0, cy=0, amp=0, speed=1.0f, radius=0; 
    Entity* sprite = nullptr;

    float prevx=0.f, prevy=0.f;
};

struct Rect { float x,y,w,h; };

struct Tile {
    float baseX, baseY;
    Entity* e;
};
struct PlatSpan {
    Rect hitbox;                
    std::vector<Tile> tiles;    
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

static const float LEVEL_WIDTH = 3600.f;  
static const float PLAYER_W    = 256.f;
static const float PLAYER_H    = 256.f;

static const float PIT_X = 2400.f;                 
static const float PIT_W = 360.f;                  
static const float PIT_Y = (float)WINDOW_HEIGHT - 120.f;
static const float PIT_H = 120.f;

static std::vector<StaticPlat> gStatic;   
static std::vector<MovingPlat> gMoving;
static std::vector<PlatSpan>  gSpans;   
static std::vector<Engine::Vec2> gSpawns;
static int gSpawnIndex = 0;
static std::vector<Rect> gDeathZones;    
static Camera gCam;

static const float kScrollStep   = 640.f;               
static const float kRightOffset  = 1500.f;             
static const float kLeftOffset   = 200.f;              
static const float kTriggerWidth = 60.f;
static Rect gScrollRight;
static Rect gScrollLeft;

static Engine::Registry gRegistry;


int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Witch Runner (Hybrid P2P Client)",
                                     WINDOW_WIDTH, WINDOW_HEIGHT,
                                     SDL_WINDOW_RESIZABLE,
                                     &window, &renderer)) return 1;

    Scaling::setMode(ScaleMode::Pixel);
    Physics::setGravity(2000.f);
    srand((unsigned)time(nullptr));

    NetStrategy strat = NetStrategy::FullState;
    if (const char* s = std::getenv("NET_STRATEGY")) {
        if (std::string(s) == "input") strat = NetStrategy::InputDelta;
    }

    const std::string myId = "client_" + std::to_string(1000 + (rand()%9000));
    PeerManager peerManager(myId);

    peerManager.connectToServer("tcp://127.0.0.1:5555");
    peerManager.sendToServer("CONNECT");
    std::string resp = peerManager.receiveFromServer();
    if (resp.find("CONNECTED") == std::string::npos && resp.find("GHOST") == std::string::npos) return 1;
    std::cout << "Connected as " << myId << "\n";

    void* ctx = zmq_ctx_new();

    void* ghostSub = zmq_socket(ctx, ZMQ_SUB);
    zmq_connect(ghostSub, "tcp://127.0.0.1:5556");
    zmq_setsockopt(ghostSub, ZMQ_SUBSCRIBE, "", 0);

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

    void* rawPeerSub = zmq_socket(ctx, ZMQ_SUB);
    for (int i = 1; i <= 20; ++i) {
        if (i == myNum) continue;
        std::ostringstream ep; ep << "tcp://127.0.0.1:" << (7000 + i);
        zmq_connect(rawPeerSub, ep.str().c_str());
    }
    zmq_setsockopt(rawPeerSub, ZMQ_SUBSCRIBE, "", 0);

    void* eventPub = zmq_socket(ctx, ZMQ_PUB);
    {
        std::ostringstream ep; ep << "tcp://*:" << (8000 + myNum);
        if (zmq_bind(eventPub, ep.str().c_str()) != 0) {
            std::cerr << "Failed to bind eventPub on " << ep.str()
                      << ": " << zmq_strerror(errno) << "\n";
        }
    }
    void* eventSub = zmq_socket(ctx, ZMQ_SUB);
    for (int i = 1; i <= 20; ++i) {
        if (i == myNum) continue;
        std::ostringstream ep; ep << "tcp://127.0.0.1:" << (8000 + i);
        zmq_connect(eventSub, ep.str().c_str());
    }
    zmq_setsockopt(eventSub, ZMQ_SUBSCRIBE, "", 0);

    SDL_Texture* bgSky = IMG_LoadTexture(renderer, BG_SKY_ASSET);
    Entity groundE(renderer, PLATFORM_ASSET, 0, 950, 1920, 130, 1, 0);
    Entity graveE(renderer, GRAVE_ASSET, 700, 700, 256, 256, 1, 0);
    Entity ghostE(renderer, GHOST_ASSET, 1500, 600, 256, 256, 1, 0);
    Entity playerE(renderer, PLAYER_ASSET, 100, WINDOW_HEIGHT - 322.f, 256, 256, 1, 0);

    gStatic.push_back({ -100000.f, 950.f, 200000.f, 130.f, &groundE });

    const float midW = 512.f;
    const float midX = (WINDOW_WIDTH - midW) * 0.5f;
    const float midY = 520.f;
    gSpans.push_back(makeSpan(renderer, midX, midY, midW));

    const float rightW = 512.f;
    const float rightX = 1800.f;
    const float rightY = 460.f;
    gSpans.push_back(makeSpan(renderer, rightX, rightY, rightW));

    gMoving.push_back(MovingPlat{
        1500.f, (float)WINDOW_HEIGHT-420.f, 260.f, 60.f,
        MoveKind::HorizontalSine, 1500.f, (float)WINDOW_HEIGHT-420.f,
        180.f, 1.5f, 0.f,
        new Entity(renderer, PLATFORM_ASSET, 1500, WINDOW_HEIGHT-420, 260, 60, 1, 0)
    });
    gMoving.push_back(MovingPlat{
        3000.f, (float)WINDOW_HEIGHT-380.f, 260.f, 60.f,
        MoveKind::Circular, 3000.f, (float)WINDOW_HEIGHT-450.f,
        0.f, 1.3f, 120.f,
        new Entity(renderer, PLATFORM_ASSET, 3000, WINDOW_HEIGHT-380, 260, 60, 1, 0)
    });
    for (auto& m : gMoving) { m.prevx = m.wx; m.prevy = m.wy; }

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

    gDeathZones = {
        { PIT_X, PIT_Y, PIT_W, PIT_H },                                
        { -10000.f, (float)WINDOW_HEIGHT+5.f, 30000.f, 5000.f }        
    };

    gStatic.clear();
    const float GY = 950.f;    
    const float GH = 130.f;    
    const float pitStart = gDeathZones[0].x;
    const float pitEnd   = gDeathZones[0].x + gDeathZones[0].w;

    gStatic.push_back({ -100000.f, GY, pitStart - (-100000.f), GH, &groundE });
    gStatic.push_back({ pitEnd,    GY, 200000.f - pitEnd,      GH, nullptr });

    std::unordered_map<std::string, Entity*> players;
    players[myId] = &playerE;
    std::unordered_map<std::string, Engine::Vec2> playerWorld;

    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastHeard;
    lastHeard[myId] = std::chrono::steady_clock::now();

    LocalPlayer me;
    me.x = gSpawns[gSpawnIndex].x;
    me.y = gSpawns[gSpawnIndex].y;

    Timeline myTime; myTime.anchorToRealTime(); myTime.setScale(1.0);

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

    Engine::EventManager eventManager(&myTime);

    std::vector<ReplayFrame> replayFrames;
    bool   replayRecording   = false;
    bool   replayPlaying     = false;
    float  replayRecordTime  = 0.f;
    float  replayPlayTime    = 0.f;
    size_t replayPlayIndex   = 0;

    eventManager.registerListener(Engine::EventType::Collision,
        [](const Engine::Event& e){
            const std::string& a = std::get<std::string>(e.payload.at("A"));
            const std::string& b = std::get<std::string>(e.payload.at("B"));
            if (a == "Ground" || b == "Ground") return;
            std::cout << "[EVENT] Collision at t=" << e.timestamp
                      << " priority=" << e.priority << "\n";
        });

    eventManager.registerListener(Engine::EventType::Death,
        [](const Engine::Event&){
            std::cout << "[EVENT] Death\n";
        });

    eventManager.registerListener(Engine::EventType::Spawn,
        [](const Engine::Event&){
            std::cout << "[EVENT] Spawn\n";
        });

    eventManager.registerListener(Engine::EventType::Input,
        [](const Engine::Event&){
            std::cout << "[EVENT] Input\n";
        });

    eventManager.registerListener(Engine::EventType::Collision, [](const Engine::Event& e){
        const std::string& a = std::get<std::string>(e.payload.at("A"));
        const std::string& b = std::get<std::string>(e.payload.at("B"));

        if (a == "Ground" || b == "Ground") {
            return; 
        }

        std::cout << "[Event] Collision between " << a << " and " << b << "\n";
    });


    eventManager.registerListener(Engine::EventType::ReplayStop,
        [&](const Engine::Event&){
            std::cout << "[EVENT] ReplayStop\n";
            replayRecording = false;         
            eventManager.stopRecording();
        });

    eventManager.registerListener(Engine::EventType::ReplayStart,
        [&](const Engine::Event&){
            std::cout << "[EVENT] ReplayStart\n";
            replayFrames.clear();             
            replayRecording  = true;
            replayPlaying    = false;
            replayRecordTime = 0.f;
            eventManager.startRecording();
        });

    eventManager.registerListener(Engine::EventType::ReplayPlay,
        [&](const Engine::Event&){
            std::cout << "[EVENT] ReplayPlay\n";
            if (!replayFrames.empty()) {
                replayPlaying    = true;      
                replayRecording  = false;
                replayPlayTime   = 0.f;
                replayPlayIndex  = 0;
                eventManager.playReplay();
            }
        });


    auto sendEventNet = [&](const Engine::Event& e){
        if (!eventPub) return;
        std::string payload = e.serialize();
        std::string message = "EV " + myId + " " + payload;
        if (zmq_send(eventPub, message.c_str(), (int)message.size(), ZMQ_DONTWAIT) < 0) {
            std::cerr << "Failed to send EV: " << zmq_strerror(errno) << "\n";
        }
    };

    {
        Engine::Event spawnEv = Engine::Events::Spawn(myId, me.x, me.y, &myTime);
        eventManager.raiseEvent(spawnEv);
        sendEventNet(spawnEv);
    }

    bool paused = false, prevT = false, prevJump = false;
    bool running = true;

    bool showDebug = false;
    static bool prevF1 = false;
    bool prevR = false, prevY = false;

    float ghostX = 1500.f, ghostY = 600.f; 
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

    const float JumpImpulse = 1100.f;
    const float MaxUpSpeed  = -1500.f;

    auto clampCam = [&](){
        float maxCam = std::max(0.f, LEVEL_WIDTH - (float)WINDOW_WIDTH);
        if (gCam.x < 0.f) gCam.x = 0.f;
        if (gCam.x > maxCam) gCam.x = maxCam;
    };

    {
        float desiredCam = me.x - (WINDOW_WIDTH * 0.5f);
        if (desiredCam < 0.f) desiredCam = 0.f;
        gCam.x = snapToStep(desiredCam, kScrollStep);
        clampCam();
    }

    while (running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
        }
        Input::poll();

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

        bool rNow = Input::isKeyPressed(SDL_SCANCODE_R);
        if (rNow && !prevR) {
            if (!eventManager.isRecording()) {
                Engine::Event e = Engine::Events::ReplayStart(&myTime);
                eventManager.raiseEvent(e);
            } else {
                Engine::Event e = Engine::Events::ReplayStop(&myTime);
                eventManager.raiseEvent(e
                );
            }
        }
        prevR = rNow;

        bool yNow = Input::isKeyPressed(SDL_SCANCODE_Y);
        if (yNow && !prevY) {
            Engine::Event e = Engine::Events::ReplayPlay(&myTime);
            eventManager.raiseEvent(e);
        }
        prevY = yNow;

        float dt = (float)myTime.tick();

        float desiredVx = 0.f;
        if (Input::isKeyPressed(SDL_SCANCODE_A)) desiredVx = -300.f;
        else if (Input::isKeyPressed(SDL_SCANCODE_D)) desiredVx =  300.f;

        bool jumpNow = Input::isKeyPressed(SDL_SCANCODE_SPACE);
        bool wantJump = (jumpNow && !prevJump);
        prevJump = jumpNow;

        me.vx = desiredVx;
        if (wantJump) {
            Engine::Event e = Engine::Events::Input("Jump", true, &myTime);
            eventManager.raiseEvent(e);
            sendEventNet(e);

            me.vy = -JumpImpulse;
            if (me.vy < MaxUpSpeed) me.vy = MaxUpSpeed;
        }
        me.vy += Physics::gravity() * dt;
        me.x  += me.vx * dt;
        me.y  += me.vy * dt;

        if (me.y < 0) { me.y = 0; me.vy = 0; }

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

        me.onGround = false;

        auto collideRect = [&](float rx,float ry,float rw,float rh, MovingPlat* moving, bool isGround){
            float pw = PLAYER_W, ph = PLAYER_H;
            if (!AABB(me.x, me.y, pw, ph, rx, ry, rw, rh)) return;

            {
                std::string otherName;
                if (moving) {
                    otherName = "MovingPlatform";
                } else if (isGround) {
                    otherName = "Ground";
                } else {
                    otherName = "Platform";
                }

                Engine::Event collEv = Engine::Events::Collision(
                    "Player",
                    otherName,
                    &myTime,
                    1);
                eventManager.raiseEvent(collEv);
                sendEventNet(collEv);
            }

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
                if (moving) { 
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

        for (auto& s : gStatic) collideRect(s.wx, s.wy, s.ww, s.wh, nullptr, true);
        for (auto& span : gSpans) collideRect(span.hitbox.x, span.hitbox.y, span.hitbox.w, span.hitbox.h, nullptr, false);
        for (auto& m : gMoving) collideRect(m.wx, m.wy, m.ww, m.wh, &m, false);

        if (me.onGround) {
            const float F = 1500.f;
            if      (me.vx > 0.f) me.vx = std::max(0.f, me.vx - F*dt);
            else if (me.vx < 0.f) me.vx = std::min(0.f, me.vx + F*dt);
        }

        if (me.x < 0.f) me.x = 0.f;
        if (me.x > LEVEL_WIDTH - PLAYER_W) me.x = LEVEL_WIDTH - PLAYER_W;

        auto snapCameraToPlayer = [&](){
            float desiredCam = me.x - (WINDOW_WIDTH * 0.5f);
            if (desiredCam < 0.f) desiredCam = 0.f;
            gCam.x = snapToStep(desiredCam, kScrollStep);
            clampCam();
            scrollCooldown = 0.2f; 
        };

        if (AABB(me.x,me.y,PLAYER_W,PLAYER_H,700,700,256,256) ||
            AABB(me.x,me.y,PLAYER_W,PLAYER_H,ghostX,ghostY,256,256)) {
            {
                Engine::Event deathEv = Engine::Events::Death(myId, &myTime);
                eventManager.raiseEvent(deathEv);
                sendEventNet(deathEv);

                Engine::Event spawnEv = Engine::Events::Spawn(myId,
                                                              gSpawns[0].x,
                                                              gSpawns[0].y,
                                                              &myTime);
                eventManager.raiseEvent(spawnEv);
                sendEventNet(spawnEv);
            }

            gSpawnIndex = 0;
            me.x = gSpawns[gSpawnIndex].x;
            me.y = gSpawns[gSpawnIndex].y;
            me.vx = me.vy = 0;
            snapCameraToPlayer(); 
        }

        bool teleported = false;
        for (auto& dz : gDeathZones) {
            if (AABB(me.x, me.y, PLAYER_W,PLAYER_H, dz.x, dz.y, dz.w, dz.h)) {
                {
                    Engine::Event deathEv = Engine::Events::Death(myId, &myTime);
                    eventManager.raiseEvent(deathEv);
                    sendEventNet(deathEv);
                }

                gSpawnIndex = (gSpawnIndex + 1) % (int)gSpawns.size();
                me.x = gSpawns[gSpawnIndex].x;
                me.y = gSpawns[gSpawnIndex].y;
                me.vx = me.vy = 0;

                {
                    Engine::Event spawnEv = Engine::Events::Spawn(myId, me.x, me.y, &myTime);
                    eventManager.raiseEvent(spawnEv);
                    sendEventNet(spawnEv);
                }

                teleported = true;
                snapCameraToPlayer(); 
                break;
            }
        }
        if (!teleported) {
            Rect dynamicPit = { gCam.x + 1400.f, (float)WINDOW_HEIGHT - 120.f, 200.f, 120.f };
            if (!(dynamicPit.x + dynamicPit.w <= 0.f || dynamicPit.x >= LEVEL_WIDTH)) {
                if (AABB(me.x, me.y, PLAYER_W,PLAYER_H, dynamicPit.x, dynamicPit.y, dynamicPit.w, dynamicPit.h)) {
                    {
                        Engine::Event deathEv = Engine::Events::Death(myId, &myTime);
                        eventManager.raiseEvent(deathEv);
                        sendEventNet(deathEv);
                    }

                    gSpawnIndex = (gSpawnIndex + 1) % (int)gSpawns.size();
                    me.x = gSpawns[gSpawnIndex].x;
                    me.y = gSpawns[gSpawnIndex].y;
                    me.vx = me.vy = 0;

                    {
                        Engine::Event spawnEv = Engine::Events::Spawn(myId, me.x, me.y, &myTime);
                        eventManager.raiseEvent(spawnEv);
                        sendEventNet(spawnEv);
                    }

                    snapCameraToPlayer(); 
                }
            }
        }

        if (scrollCooldown > 0.f) scrollCooldown -= dt;

        gScrollRight = { gCam.x + kRightOffset, 0.f, kTriggerWidth, (float)WINDOW_HEIGHT };
        gScrollLeft  = { gCam.x + kLeftOffset  - kTriggerWidth, 0.f, kTriggerWidth, (float)WINDOW_HEIGHT };

        Rect pr = { me.x, me.y, PLAYER_W, PLAYER_H };

        if (scrollCooldown <= 0.f && AABB(pr.x, pr.y, pr.w, pr.h,
                                          gScrollRight.x, gScrollRight.y, gScrollRight.w, gScrollRight.h)) {
            gCam.x += kScrollStep;
            clampCam();
            scrollCooldown = 0.15f;
        }
        if (scrollCooldown <= 0.f && AABB(pr.x, pr.y, pr.w, pr.h,
                                          gScrollLeft.x, gScrollLeft.y, gScrollLeft.w, gScrollLeft.h)) {
            gCam.x -= kScrollStep;
            clampCam();
            scrollCooldown = 0.15f;
        }

        {
            auto& go = gRegistry.upsert(myId);
            go.set<Engine::Vec2>("pos", {me.x, me.y});
            go.set<Engine::Vec2>("vel", {me.vx, me.vy});
        }

        if (strat == NetStrategy::FullState) {
            peerManager.updateMyPlayerData(me.x, me.y, paused, (float)myTime.scale());
        } else {
            bool L = Input::isKeyPressed(SDL_SCANCODE_A);
            bool R = Input::isKeyPressed(SDL_SCANCODE_D);
            bool J = wantJump;
            peerManager.sendInputDelta(L, R, J, 0.f, 0.f, SDL_GetTicks());
        }

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

        {
            for (int i = 0; i < 32; ++i) {
                char eb[1024];
                int n = zmq_recv(eventSub, eb, sizeof(eb) - 1, ZMQ_DONTWAIT);
                if (n <= 0) break;
                eb[n] = 0;
                std::istringstream iss{std::string(eb)};
                std::string tag; iss >> tag;
                if (tag == "EV") {
                    std::string fromId;
                    iss >> fromId;
                    std::string rest;
                    std::getline(iss, rest);
                    if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
                    if (!rest.empty()) {
                        eventManager.raiseEventFromNetwork(rest);
                    }
                }
            }
        }

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

        if (replayRecording) {
            replayRecordTime += dt;
            replayFrames.push_back(ReplayFrame{
                replayRecordTime,
                me.x,
                me.y,
                gCam.x
            });
        }

        eventManager.dispatchEvents();

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (bgSky) SDL_RenderTexture(renderer, bgSky, nullptr, nullptr);

        {
            const int   groundTileW = 1920;
            const float groundY     = GY;

            float screenL = gCam.x;
            float firstTileX = std::floor(screenL / groundTileW) * groundTileW;

            const float pitStartWorld = gDeathZones[0].x;
            const float pitEndWorld   = gDeathZones[0].x + gDeathZones[0].w;

            for (int i = -1; i <= 2; ++i) {
                float gx = firstTileX + i * groundTileW;    
                float gxEnd = gx + groundTileW;

                if (gxEnd <= 0.f || gx >= LEVEL_WIDTH) continue;

                float dx = gx - gCam.x;                     
                groundE.setPosition(dx, groundY);
                groundE.update();
                groundE.render(renderer, window);

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

        for (auto& span : gSpans) {
            for (auto& t : span.tiles) {
                t.e->setPosition(t.baseX - gCam.x, t.baseY);
                t.e->update(); t.e->render(renderer, window);
            }
        }

        for (auto& m : gMoving) {
            m.sprite->setPosition(m.wx - gCam.x, m.wy);
            m.sprite->update(); m.sprite->render(renderer, window);
        }

        if (replayPlaying && !replayFrames.empty()) {
            replayPlayTime += dt;

            while (replayPlayIndex + 1 < replayFrames.size() &&
                   replayFrames[replayPlayIndex + 1].t <= replayPlayTime) {
                ++replayPlayIndex;
            }

            const ReplayFrame& fr = replayFrames[replayPlayIndex];
            me.x   = fr.px;
            me.y   = fr.py;
            gCam.x = fr.camx;
            clampCam();

            if (replayPlayIndex + 1 >= replayFrames.size()) {
                replayPlaying = false;
            }
        }

        graveE.setPosition(700.f - gCam.x, 700.f);
        ghostE.setPosition(ghostX - gCam.x, ghostY);
        graveE.update(); graveE.render(renderer, window);
        ghostE.update(); ghostE.render(renderer, window);

        playerE.setPosition(me.x - gCam.x, me.y);
        playerE.update(); playerE.render(renderer, window);

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

        if (showDebug) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

            SDL_SetRenderDrawColor(renderer, 0, 255, 0, 100);
            for (const auto& sp : gSpawns) {
                SDL_FRect r = { sp.x - gCam.x, sp.y, 50.f, 50.f };
                SDL_RenderFillRect(renderer, &r);
            }

            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 100);
            for (const auto& dz : gDeathZones) {
                SDL_FRect r = { dz.x - gCam.x, dz.y, dz.w, dz.h };
                SDL_RenderFillRect(renderer, &r);
            }

            SDL_SetRenderDrawColor(renderer, 0, 0, 255, 100);
            SDL_FRect dyn = { (gCam.x + 1400.f) - gCam.x, (float)WINDOW_HEIGHT - 120.f, 200.f, 120.f };
            SDL_RenderFillRect(renderer, &dyn);

            if (!gSpawns.empty()) {
                SDL_SetRenderDrawColor(renderer, 0, 200, 0, 255);
                SDL_FRect r = { gSpawns[gSpawnIndex].x - gCam.x, gSpawns[gSpawnIndex].y, 50.f, 50.f };
                SDL_RenderRect(renderer, &r);
            }
        }

        if (replayRecording || replayPlaying) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            if (replayRecording) {
                SDL_SetRenderDrawColor(renderer, 255, 0, 0, 160); 
                SDL_FRect r = { 10.f, 10.f, 30.f, 30.f };
                SDL_RenderFillRect(renderer, &r);
            }
            if (replayPlaying) {
                SDL_SetRenderDrawColor(renderer, 0, 0, 255, 160); 
                SDL_FRect r = { 50.f, 10.f, 30.f, 30.f };
                SDL_RenderFillRect(renderer, &r);
            }
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    for (auto& span : gSpans) for (auto& t : span.tiles) delete t.e;
    for (auto& m : gMoving) if (m.sprite) delete m.sprite;
    if (bgSky) SDL_DestroyTexture(bgSky);

    zmq_close(eventSub);
    zmq_close(eventPub);
    zmq_close(rawPeerSub);
    zmq_close(ghostSub);
    zmq_ctx_term(ctx);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
