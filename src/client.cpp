// Client with hybrid P2P player sync (PeerManager) and ghost SUB (unchanged)
// - REQ to 5555 via PeerManager: CONNECT -> parse
// - SUB to 5556 (raw ZMQ): ghost updates, exactly like your original
// - PUB/SUB peers via PeerManager (one client per port 7000+i)
// - Local entity never overwritten; remote players from PeerManager peer data

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

#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <chrono>
#include <zmq.h>

// ---------- Constants ----------
const int WINDOW_WIDTH = 1920;
const int WINDOW_HEIGHT = 1080;

const char* PLAYER_ASSET   = "../assets/player.png";
const char* GHOST_ASSET    = "../assets/ghost.png";
const char* PLATFORM_ASSET = "../assets/platform.png";
const char* GRAVE_ASSET    = "../assets/grave.png";
const char* BG_SKY_ASSET   = "../assets/background_sky.png";

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

struct PlayerState {
    float x = 100.f;
    float y = WINDOW_HEIGHT - 322.f;
    float vx = 0.f;
    float vy = 0.f;
    bool onGround = false;
};

// ---------- Main ----------
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

    // --- PeerManager networking ---
    const std::string myId = "client_" + std::to_string(1000 + (rand()%9000));
    PeerManager peerManager(myId);

    // REQ: CONNECT via PeerManager
    peerManager.connectToServer("tcp://127.0.0.1:5555");
    peerManager.sendToServer("CONNECT");
    std::string resp = peerManager.receiveFromServer();
    if (resp.find("CONNECTED") == std::string::npos && resp.find("GHOST") == std::string::npos) {
        std::cerr << "CONNECT failed\n"; return 1;
    }
    std::cout << "Connected as " << myId << "\n";

    // Ghost SUB (raw ZMQ – preserved exactly like your original)
    void* ctx = zmq_ctx_new();
    void* ghostSub = zmq_socket(ctx, ZMQ_SUB);
    zmq_connect(ghostSub, "tcp://127.0.0.1:5556");
    zmq_setsockopt(ghostSub, ZMQ_SUBSCRIBE, "", 0);

    // P2P via PeerManager
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

    // --- Assets ---
    SDL_Texture* bgSky = IMG_LoadTexture(renderer, BG_SKY_ASSET);
    Entity platformE(renderer, PLATFORM_ASSET, 0, 950, 1920, 130, 1, 0);
    Entity graveE(renderer, GRAVE_ASSET, 700, 700, 256, 256, 1, 0);
    Entity ghostE(renderer, GHOST_ASSET, 1500, 600, 256, 256, 1, 0);
    Entity playerE(renderer, PLAYER_ASSET, 100, WINDOW_HEIGHT - 322.f, 256, 256, 1, 0);

    std::unordered_map<std::string, Entity*> players;
    players[myId] = &playerE; // ✅ never overwrite my own entity

    PlayerState me;
    Timeline myTime; myTime.anchorToRealTime(); myTime.setScale(1.0);

    bool paused = false, prevT = false, prevJump = false;
    float ghostX = 1500.f, ghostY = 600.f;
    bool running = true;
    SDL_Event ev;

    auto sendPose = [&](float px, float py) {
        // Publish my pose via PeerManager (wrapped inside)
        peerManager.updateMyPlayerData(px, py, paused, (float)myTime.scale());
    };

    while (running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
        }
        Input::poll();

        // Toggle scaling view
        bool tNow = Input::isKeyPressed(SDL_SCANCODE_T);
        if (tNow && !prevT) {
            auto current = Scaling::mode();
            Scaling::setMode(current == ScaleMode::Pixel ? ScaleMode::Proportional : ScaleMode::Pixel);
        }
        prevT = tNow;

        // Pause/speed (local only)
        if (Input::isKeyPressed(SDL_SCANCODE_P)) { paused = !paused; myTime.pause(paused); }
        if (Input::isKeyPressed(SDL_SCANCODE_1)) myTime.setScale(0.5);
        if (Input::isKeyPressed(SDL_SCANCODE_2)) myTime.setScale(1.0);
        if (Input::isKeyPressed(SDL_SCANCODE_3)) myTime.setScale(2.0);

        // Input
        float desiredVx = 0.f;
        if (Input::isKeyPressed(SDL_SCANCODE_A)) desiredVx = -400.f;
        else if (Input::isKeyPressed(SDL_SCANCODE_D)) desiredVx = 400.f;

        bool jumpNow = Input::isKeyPressed(SDL_SCANCODE_SPACE);
        bool wantJump = (jumpNow && !prevJump);
        prevJump = jumpNow;

        // Step local player
        float dt = (float)myTime.tick();
        me.vx = desiredVx;
        if (wantJump) { me.vy = -900.f; me.onGround = false; }
        me.vy += Physics::gravity() * dt;
        me.x  += me.vx * dt;
        me.y  += me.vy * dt;

        if (me.y < 0) { me.y = 0; me.vy = 0; }
        if (me.x < 0) { me.x = 0; me.vx = 0; }
        if (me.x > WINDOW_WIDTH - 256) { me.x = WINDOW_WIDTH - 256; me.vx = 0; }

        if (AABB(me.x,me.y,256,256,0,950,1920,130)) { me.vy = 0; me.y = 950-256; me.onGround = true; }
        if (AABB(me.x,me.y,256,256,700,700,256,256) ||
            AABB(me.x,me.y,256,256,ghostX,ghostY,256,256)) {
            me = PlayerState{};
        }

        playerE.setPosition(me.x, me.y);
        sendPose(me.x, me.y);

        // Ghost update from PUB (unchanged)
        {
            char gbuf[128];
            int n = zmq_recv(ghostSub, gbuf, sizeof(gbuf)-1, ZMQ_DONTWAIT);
            if (n > 0) {
                gbuf[n] = 0;
                std::istringstream iss{std::string(gbuf)};
                std::string tag; iss >> tag;
                if (tag == "GHOST") { iss >> ghostX >> ghostY; ghostE.setPosition(ghostX, ghostY); }
            }
        }

        // Get peers from PeerManager and update remote entities
        {
            auto peerData = peerManager.getPeerPlayerData();
            for (const auto& [pid, pd] : peerData) {
                if (pid == myId) continue; // never overwrite self
                if (players.find(pid) == players.end())
                    players[pid] = new Entity(renderer, PLAYER_ASSET, pd.x, pd.y, 256, 256, 1, 0);
                players[pid]->setPosition(pd.x, pd.y);
            }
        }

        // Optional: ping server for a ghost snapshot (kept for parity with your original fallback)
        {
            peerManager.sendToServer("PING");
            std::string s = peerManager.receiveFromServer();
            if (!s.empty() && s.rfind("GHOST ", 0) == 0) {
                std::istringstream iss{ s };
                std::string tag; iss >> tag >> ghostX >> ghostY;
                ghostE.setPosition(ghostX, ghostY);
            }
        }

        // Render
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
        SDL_Delay(16);
    }

    for (auto& kv : players) if (kv.first != myId) delete kv.second;
    if (bgSky) SDL_DestroyTexture(bgSky);
    zmq_close(ghostSub); zmq_ctx_term(ctx);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
