// Client with hybrid P2P player sync and fixed self-entity handling
// - REQ to 5555: CONNECT (get id)
// - SUB to 5556: ghost updates from server
// - PUB on 7000+id: send my pose
// - SUB peers: receive other players' poses
// Own playerE is never recreated or overwritten.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <zmq.h>

#include "entity.h"
#include "input.h"
#include "scaling.h"
#include "physics.h"
#include "collision.h"
#include "timeline.h"

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

// Extract numeric suffix from "client_####"
static int numericIdFrom(const std::string& s) {
    int n = 0; bool any = false;
    for (char c : s) {
        if (std::isdigit((unsigned char)c)) { any = true; n = n*10 + (c - '0'); }
    }
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

    // --- Networking setup ---
    void* ctx = zmq_ctx_new();

    // REQ: CONNECT to server (id)
    void* req = zmq_socket(ctx, ZMQ_REQ);
    zmq_connect(req, "tcp://localhost:5555");
    const std::string connectMsg = "CONNECT";
    zmq_send(req, connectMsg.c_str(), (int)connectMsg.size(), 0);

    char rbuf[256]; int rn = zmq_recv(req, rbuf, sizeof(rbuf)-1, 0);
    if (rn <= 0) { std::cerr << "CONNECT failed\n"; return 1; }
    rbuf[rn] = 0;

    std::string myId;
    {
        std::istringstream iss(rbuf);
        std::string tag; iss >> tag >> myId;
        if (tag != "CONNECTED" || myId.empty()) myId = "client_1";
    }
    std::cout << "Connected as " << myId << "\n";
    const int myNum = numericIdFrom(myId);
    const int myPubPort = 7000 + myNum;

    // SUB: ghost updates from server
    void* ghostSub = zmq_socket(ctx, ZMQ_SUB);
    zmq_connect(ghostSub, "tcp://localhost:5556");
    zmq_setsockopt(ghostSub, ZMQ_SUBSCRIBE, "", 0);

    // PUB: my pose
    void* peerPub = zmq_socket(ctx, ZMQ_PUB);
    {
        std::ostringstream oss; oss << "tcp://*:" << myPubPort;
        if (zmq_bind(peerPub, oss.str().c_str()) != 0) {
            std::cerr << "PUB bind failed: " << zmq_strerror(errno) << "\n";
        }
    }

    // SUB: peers
    void* peerSub = zmq_socket(ctx, ZMQ_SUB);
    for (int i = 1; i <= 20; ++i) {
        if (i == myNum) continue;
        std::ostringstream ep; ep << "tcp://localhost:" << (7000 + i);
        zmq_connect(peerSub, ep.str().c_str());
    }
    zmq_setsockopt(peerSub, ZMQ_SUBSCRIBE, "", 0);

    // --- Assets ---
    SDL_Texture* bgSky = IMG_LoadTexture(renderer, BG_SKY_ASSET);
    Entity platformE(renderer, PLATFORM_ASSET, 0, 950, 1920, 130, 1, 0);
    Entity graveE(renderer, GRAVE_ASSET, 700, 700, 256, 256, 1, 0);
    Entity ghostE(renderer, GHOST_ASSET, 1500, 600, 256, 256, 1, 0);
    Entity playerE(renderer, PLAYER_ASSET, 100, WINDOW_HEIGHT - 322.f, 256, 256, 1, 0);

    std::unordered_map<std::string, Entity*> players;
    players[myId] = &playerE; // ✅ my entity is fixed

    PlayerState me;
    Timeline myTime; myTime.anchorToRealTime(); myTime.setScale(1.0);

    bool paused = false, prevT = false, prevJump = false;
    float ghostX = 1500.f, ghostY = 600.f;
    bool running = true;
    SDL_Event ev;

    auto sendPose = [&](float px, float py) {
        std::ostringstream oss;
        oss << "POSE " << myId << " " << px << " " << py;
        std::string msg = oss.str();
        zmq_send(peerPub, msg.c_str(), (int)msg.size(), 0);
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

        // Ghost update
        {
            char gbuf[128]; int n = zmq_recv(ghostSub, gbuf, sizeof(gbuf)-1, ZMQ_DONTWAIT);
            if (n > 0) {
                gbuf[n] = 0;
                std::istringstream iss(gbuf);
                std::string tag; iss >> tag;
                if (tag == "GHOST") { iss >> ghostX >> ghostY; ghostE.setPosition(ghostX, ghostY); }
            }
        }

        // Peer poses
        {
            char pbuf[256]; int n;
            for (int i=0;i<8;i++) {
                n = zmq_recv(peerSub, pbuf, sizeof(pbuf)-1, ZMQ_DONTWAIT);
                if (n <= 0) break;
                pbuf[n] = 0;
                std::istringstream iss(pbuf);
                std::string tag; iss >> tag;
                if (tag == "POSE") {
                    std::string pid; float px, py;
                    iss >> pid >> px >> py;
                    if (pid != myId) { // ✅ never overwrite my own entity
                        if (players.find(pid) == players.end())
                            players[pid] = new Entity(renderer, PLAYER_ASSET, px, py, 256, 256, 1, 0);
                        players[pid]->setPosition(px, py);
                    }
                }
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
    zmq_close(req); zmq_close(peerPub); zmq_close(peerSub); zmq_close(ghostSub); zmq_ctx_term(ctx);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
