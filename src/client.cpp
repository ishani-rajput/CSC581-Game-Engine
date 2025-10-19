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
#include <zmq.h>

using Engine::NetStrategy;

static const int WINDOW_WIDTH  = 1920;
static const int WINDOW_HEIGHT = 1080;

static const char* PLAYER_ASSET   = "../assets/player.png";
static const char* GHOST_ASSET    = "../assets/ghost.png";
static const char* PLATFORM_ASSET = "../assets/platform.png";
static const char* GRAVE_ASSET    = "../assets/grave.png";
static const char* BG_SKY_ASSET   = "../assets/background_sky.png";

// --- helpers ---------------------------------------------------------------

static inline bool AABB(float ax,float ay,float aw,float ah,
                        float bx,float by,float bw,float bh) {
    return (ax < bx + bw) && (ax + aw > bx) &&
           (ay < by + bh) && (ay + ah > by);
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

// --- main -----------------------------------------------------------------

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
        peerManager.startPeerListener(oss.str());  // binds a PUB socket internally
    }
    for (int i = 1; i <= 20; ++i) {
        if (i == myNum) continue;
        std::ostringstream ep; ep << "tcp://localhost:" << (7000 + i);
        peerManager.connectToPeerNetwork(ep.str()); // connects to those PUBs internally
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
    Entity platformE(renderer, PLATFORM_ASSET, 0, 950, 1920, 130, 1, 0);
    Entity graveE(renderer, GRAVE_ASSET, 700, 700, 256, 256, 1, 0);
    Entity ghostE(renderer, GHOST_ASSET, 1500, 600, 256, 256, 1, 0);
    Entity playerE(renderer, PLAYER_ASSET, 100, WINDOW_HEIGHT - 322.f, 256, 256, 1, 0);

    // map of on-screen entities for each player id
    std::unordered_map<std::string, Entity*> players;
    players[myId] = &playerE;

    // last-heard timestamps for disconnect culling
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastHeard;
    lastHeard[myId] = std::chrono::steady_clock::now();

    // gameplay state
    LocalPlayer me;
    Timeline myTime; myTime.anchorToRealTime(); myTime.setScale(1.0);

    // engine-side registry (Part 1A used on endpoint)
    static Engine::Registry gRegistry;
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
    float ghostX = 1500.f, ghostY = 600.f;
    bool running = true;
    SDL_Event ev;

    auto sendPose = [&](float px, float py) {
        peerManager.updateMyPlayerData(px, py, paused, (float)myTime.scale());
    };

    auto sendInputDelta = [&](bool L, bool R, bool J) {
        peerManager.sendInputDelta(L, R, J, 0.f, 0.f, SDL_GetTicks());
    };

    auto lastCull = std::chrono::steady_clock::now();
    std::unordered_map<std::string, GhostState> ghostSim; // remote integrators

    // --- main loop ---------------------------------------------------------
    while (running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
        }
        Input::poll();

        // toggle pixel/proportional scaling
        bool tNow = Input::isKeyPressed(SDL_SCANCODE_T);
        if (tNow && !prevT) {
            auto current = Scaling::mode();
            Scaling::setMode(current == ScaleMode::Pixel ? ScaleMode::Proportional : ScaleMode::Pixel);
        }
        prevT = tNow;

        // pause & time scaling
        if (Input::isKeyPressed(SDL_SCANCODE_P)) { paused = !paused; myTime.pause(paused); }
        if (Input::isKeyPressed(SDL_SCANCODE_1)) myTime.setScale(0.5);
        if (Input::isKeyPressed(SDL_SCANCODE_2)) myTime.setScale(1.0);
        if (Input::isKeyPressed(SDL_SCANCODE_3)) myTime.setScale(2.0);

        // local controls
        float desiredVx = 0.f;
        if (Input::isKeyPressed(SDL_SCANCODE_A)) desiredVx = -400.f;
        else if (Input::isKeyPressed(SDL_SCANCODE_D)) desiredVx = 400.f;

        bool jumpNow = Input::isKeyPressed(SDL_SCANCODE_SPACE);
        bool wantJump = (jumpNow && !prevJump);
        prevJump = jumpNow;

        // step local player via Timeline
        float dt = (float)myTime.tick();
        me.vx = desiredVx;
        if (wantJump) { me.vy = -900.f; me.onGround = false; }
        me.vy += Physics::gravity() * dt;
        me.x  += me.vx * dt;
        me.y  += me.vy * dt;

        // bounds & ground
        if (me.y < 0) { me.y = 0; me.vy = 0; }
        if (me.x < 0) { me.x = 0; me.vx = 0; }
        if (me.x > WINDOW_WIDTH - 256) { me.x = WINDOW_WIDTH - 256; me.vx = 0; }
        if (AABB(me.x,me.y,256,256, 0,950,1920,130)) { me.vy = 0; me.y = 950-256; me.onGround = true; }

        // collisions with grave or ghost reset
        if (AABB(me.x,me.y,256,256,700,700,256,256) ||
            AABB(me.x,me.y,256,256,ghostX,ghostY,256,256)) {
            me = LocalPlayer{};
        }

        // apply to render entity
        playerE.setPosition(me.x, me.y);

        // keep engine object in sync (prove object-model usage on endpoint)
        {
            auto& go = gRegistry.upsert(myId);
            go.set<Engine::Vec2>("pos", {me.x, me.y});
            go.set<Engine::Vec2>("vel", {me.vx, me.vy});
        }

        // send network update according to strategy
        if (strat == NetStrategy::FullState) {
            sendPose(me.x, me.y);
        } else {
            bool L = Input::isKeyPressed(SDL_SCANCODE_A);
            bool R = Input::isKeyPressed(SDL_SCANCODE_D);
            bool J = wantJump;
            sendInputDelta(L, R, J);
        }

        // --- consume ghost from server (PUB/SUB) ---------------------------
        {
            char gbuf[128];
            int n = zmq_recv(ghostSub, gbuf, sizeof(gbuf)-1, ZMQ_DONTWAIT);
            if (n > 0) {
                gbuf[n] = 0;
                std::istringstream iss{std::string(gbuf)};
                std::string tag; iss >> tag;
                if (tag == "GHOST") {
                    iss >> ghostX >> ghostY;
                    ghostE.setPosition(ghostX, ghostY);
                }
            }
        }

        // --- consume peer updates (PUB/SUB) --------------------------------
        {
            for (int i = 0; i < 32; ++i) {
                char mbuf[512];
                int n = zmq_recv(rawPeerSub, mbuf, sizeof(mbuf)-1, ZMQ_DONTWAIT);
                if (n <= 0) break;
                mbuf[n] = 0;
                std::istringstream iss{std::string(mbuf)};
                std::string tag; iss >> tag;

                if (tag == "POSE" || tag == "PLAYER") {
                    // Strategy A messages: "POSE pid x y [paused scale]" (the client’s sender keeps it short)
                    std::string pid; float px=0, py=0;
                    iss >> pid >> px >> py;
                    if (!pid.empty() && pid != myId) {
                        auto now = std::chrono::steady_clock::now();
                        // ensure engine GameObject exists
                        auto& go = gRegistry.upsert(pid);
                        go.set<Engine::Vec2>("pos", {px, py});

                        // ensure render entity exists
                        if (players.find(pid) == players.end()) {
                            players[pid] = new Entity(renderer, PLAYER_ASSET, px, py, 256, 256, 1, 0);
                        }
                        players[pid]->setPosition(px, py);
                        lastHeard[pid] = now;
                    }
                }
                else if (tag == "INPUT") {
                    // Strategy B messages: "INPUT pid L R J ax ay ts"
                    std::string pid; int L,R,J; float ax,ay; uint64_t ts;
                    iss >> pid >> L >> R >> J >> ax >> ay >> ts;
                    if (!pid.empty() && pid != myId) {
                        auto now = std::chrono::steady_clock::now();

                        // remote integrator (very light)
                        GhostState& st = ghostSim[pid];
                        float rdt = 0.016f; // can improve via Timeline or ts
                        st.vx = (L?-400.f:0.f) + (R?400.f:0.f);
                        if (J && st.onGround) { st.vy = -900.f; st.onGround = false; }
                        st.vy += Physics::gravity() * rdt;
                        st.x  += st.vx * rdt;
                        st.y  += st.vy * rdt;

                        if (st.x < 0) st.x = 0;
                        if (st.x > WINDOW_WIDTH-256) st.x = WINDOW_WIDTH-256;

                        // ground against platform
                        if (AABB(st.x,st.y,256,256, 0,950,1920,130)) { st.vy=0; st.y=950-256; st.onGround=true; }

                        // keep engine object updated
                        auto& go = gRegistry.upsert(pid);
                        go.set<Engine::Vec2>("pos", {st.x, st.y});
                        go.set<Engine::Vec2>("vel", {st.vx, st.vy});

                        // ensure render entity exists
                        if (players.find(pid) == players.end()) {
                            players[pid] = new Entity(renderer, PLAYER_ASSET, st.x, st.y, 256, 256, 1, 0);
                        } else {
                            players[pid]->setPosition(st.x, st.y);
                        }
                        lastHeard[pid] = now;
                    }
                }
            }
        }

        // --- disconnect culling (graceful removal) -------------------------
        {
            auto now = std::chrono::steady_clock::now();
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
                    gRegistry.erase(pid);  // keep engine model consistent on disconnect
                    lastHeard.erase(pid);
                    ghostSim.erase(pid);
                }
                lastCull = now;
            }
        }

        // --- render ---------------------------------------------------------
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (bgSky) SDL_RenderTexture(renderer, bgSky, nullptr, nullptr);
        platformE.render(renderer, window);
        graveE.render(renderer, window);
        ghostE.render(renderer, window);

        for (auto& kv : players) {
            kv.second->update();
            kv.second->render(renderer, window);
        }
        SDL_RenderPresent(renderer);

        // keep ~60fps
        SDL_Delay(16);
    }

    // cleanup
    for (auto& kv : players) if (kv.first != myId) delete kv.second;
    if (bgSky) SDL_DestroyTexture(bgSky);

    zmq_close(rawPeerSub);
    zmq_close(ghostSub);
    zmq_ctx_term(ctx);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
