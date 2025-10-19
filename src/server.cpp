// server.cpp
// Headless ghost authority server with NetworkServer + ghost PUB
// - REP on 5555: "CONNECT" -> "CONNECTED client_####"; anything else -> "GHOST gx gy"
// - PUB on 5556: broadcasts "GHOST gx gy" at ~30Hz
// - Engine object model is used on this endpoint via Registry (players as GameObjects)

#include <SDL3/SDL.h>
#include <cstdlib>
#include <ctime>
#include <string>
#include <sstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <zmq.h>

#include "physics.h"
#include "timeline.h"
#include "network_server.h"

// Engine model on server endpoint (Part 1A requirement)
#include "object_model.h"
#include "registry.h"

static const int WINDOW_WIDTH  = 1920;
static const int WINDOW_HEIGHT = 1080;

struct Ghost {
    float x = 1500.f;
    float y = 600.f;
    float vx = -200.f;
    float vy = 0.f;
    float timer = 0.f;
    float interval = 2.f;
};

class GhostServer : public Engine::NetworkServer {
public:
    GhostServer() {
        // Initialize SDL so SDL_GetTicks works (SDL3: pass a valid flag like VIDEO)
        if (!SDL_WasInit(0)) SDL_Init(SDL_INIT_VIDEO);
        srand(static_cast<unsigned>(time(nullptr)));
        Physics::setGravity(2000.f);
        ghostTime.anchorToRealTime();
        setWorldUpdateRate(60); // server worker cadence (for per-client IO)
    }

    ~GhostServer() override {
        stopBroadcaster();
        stopPub();
        stopServer();
        SDL_Quit();
    }

    // Start REP server and PUB broadcaster
    void startAll(int repPort = 5555, int pubPort = 5556) {
        startServer(repPort);   // from Engine::NetworkServer
        startPub(pubPort);      // ZMQ PUB for ghost broadcast
        startBroadcaster();     // ~30 Hz broadcast loop
        std::cout << "Server started. CONNECT on " << repPort << ", GHOST PUB on " << pubPort << "\n";
    }

protected:
    // ===== Engine::NetworkServer hooks ====================================

    // Called by a worker thread when a client message arrives on REP
    void handleClientMessage(const std::string& clientKey, const std::string& msg) override {
        // Protocol: "CONNECT" -> assign an ID; else reply with current ghost pose
        if (msg.rfind("CONNECT", 0) == 0) {
            const std::string newId = allocateClientId();
            {
                std::lock_guard<std::mutex> lk(regMutex);
                auto& go = registry.upsert(newId);
                go.set<Engine::Vec2>("pos", {spawnX(), spawnY()});
                activeClients.insert(newId);
                keyToId[clientKey] = newId;
            }
            std::ostringstream oss;
            oss << "CONNECTED " << newId;
            sendToClient(clientKey, oss.str());
            std::cout << "Client connected: " << newId << "\n";
            return;
        }

        // Fallback: reply with current ghost state
        std::ostringstream oss;
        {
            std::lock_guard<std::mutex> lk(ghostMutex);
            oss << "GHOST " << ghost.x << " " << ghost.y;
        }
        sendToClient(clientKey, oss.str());
    }

    // Not used (we reply directly in handleClientMessage), but required by base.
    std::string generateWorldState() override {
        std::ostringstream oss;
        std::lock_guard<std::mutex> lk(ghostMutex);
        oss << "GHOST " << ghost.x << " " << ghost.y;
        return oss.str();
    }

private:
    // ===== Engine-side object model (Part 1A on server) ===================
    Engine::Registry registry;
    std::mutex regMutex;
    std::unordered_set<std::string> activeClients;          // connected logical ids
    std::unordered_map<std::string,std::string> keyToId;    // REP routing key -> logical id

    static float spawnX() { return 100.f + (rand() % (WINDOW_WIDTH - 356)); }
    static float spawnY() { return WINDOW_HEIGHT - 322.f; }

    std::string allocateClientId() {
        int nid = (rand() % 9000) + 1000;
        return "client_" + std::to_string(nid);
    }

    // ===== Ghost simulation ===============================================
    Ghost ghost;
    Timeline ghostTime;
    std::mutex ghostMutex;

    // ===== PUB socket for ghost broadcast =================================
    void* ctx = nullptr;
    void* pub = nullptr;

    // ===== Broadcaster thread (~30 Hz) ====================================
    std::atomic<bool> running{false};
    std::thread broadcaster;

    void startPub(int port) {
        ctx = zmq_ctx_new();
        if (!ctx) { std::cerr << "ZMQ ctx failed\n"; return; }
        pub = zmq_socket(ctx, ZMQ_PUB);
        if (!pub) { std::cerr << "ZMQ pub failed\n"; return; }
        std::ostringstream ep; ep << "tcp://*:" << port;
        if (zmq_bind(pub, ep.str().c_str()) != 0) {
            std::cerr << "PUB bind " << port << " failed: " << zmq_strerror(errno) << "\n";
            zmq_close(pub); pub = nullptr;
        }
        int hwm = 1;
        if (pub) zmq_setsockopt(pub, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    }

    void stopPub() {
        if (pub) { zmq_close(pub); pub = nullptr; }
        if (ctx) { zmq_ctx_term(ctx); ctx = nullptr; }
    }

    void startBroadcaster() {
        if (running.exchange(true)) return;
        broadcaster = std::thread([this](){
            double accum = 0.0;
            Uint64 prev = SDL_GetTicks();
            while (running.load()) {
                Uint64 now = SDL_GetTicks();
                double dt = (now - prev) / 1000.0;
                prev = now;
                accum += dt;

                // step ghost using server timeline
                float gdt = (float)ghostTime.tick();
                {
                    std::lock_guard<std::mutex> lk(ghostMutex);
                    ghost.x += ghost.vx * gdt;
                    ghost.y += ghost.vy * gdt;

                    // wrap left, randomize Y band
                    if (ghost.x < -150) {
                        ghost.x = WINDOW_WIDTH;
                        ghost.y = static_cast<float>(rand() % (WINDOW_HEIGHT - 256));
                    }
                    if (ghost.y < 0) ghost.y = 0;
                    if (ghost.y > WINDOW_HEIGHT - 256) ghost.y = WINDOW_HEIGHT - 256;

                    // random vertical nudge every interval
                    ghost.timer += gdt;
                    if (ghost.timer >= ghost.interval) {
                        ghost.timer = 0;
                        ghost.vy = static_cast<float>((rand() % 301) - 150); // [-150, 150]
                    }
                }

                // broadcast ~30Hz
                if (accum >= 1.0/30.0) {
                    accum = 0.0;
                    if (pub) {
                        std::ostringstream oss;
                        {
                            std::lock_guard<std::mutex> lk(ghostMutex);
                            oss << "GHOST " << ghost.x << " " << ghost.y;
                        }
                        const std::string msg = oss.str();
                        zmq_send(pub, msg.c_str(), (int)msg.size(), 0);
                    }
                }

                SDL_Delay(1);
            }
        });
    }

    void stopBroadcaster() {
        if (!running.exchange(false)) return;
        if (broadcaster.joinable()) broadcaster.join();
    }
};

int main() {
    GhostServer server;
    server.startAll(5555, 5556);

    std::cout << "Headless server running.\n";
    std::cout << "Press Enter to stop.\n";
    std::cin.get();
    return 0;
}
