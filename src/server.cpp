// Headless ghost authority server with NetworkServer + ghost PUB
// - REP (via Engine::NetworkServer) on 5555: CONNECT -> CONNECTED client_####; else -> GHOST gx gy
// - PUB on 5556: broadcasts GHOST gx gy at ~30Hz (unchanged from your game)

#include <SDL3/SDL.h>
#include <cstdlib>
#include <ctime>
#include <string>
#include <sstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <zmq.h>

#include "physics.h"
#include "timeline.h"
#include "network_server.h"

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
        srand(static_cast<unsigned>(time(nullptr)));
        Physics::setGravity(2000.f);
        ghostTime.anchorToRealTime();
        setWorldUpdateRate(60); // base server cadence (per-client worker/IO)
    }

    ~GhostServer() override {
        stopBroadcaster();
        stopPub();
    }

    // Start server (REP) and ghost broadcaster (PUB)
    void startAll(int repPort = 5555, int pubPort = 5556) {
        startServer(repPort);
        startPub(pubPort);
        startBroadcaster();
        std::cout << "Server started. CONNECT on " << repPort << ", GHOST PUB on " << pubPort << "\n";
    }

protected:
    // Called by a worker thread when a client message arrives
    void handleClientMessage(const std::string& clientKey, const std::string& msg) override {
        // We keep the same protocol you had:
        // "CONNECT" -> "CONNECTED client_####"
        // Anything else -> "GHOST gx gy"
        if (msg.rfind("CONNECT", 0) == 0) {
            int nid = (rand() % 9000) + 1000;
            std::ostringstream oss;
            oss << "CONNECTED client_" << nid;
            sendToClient(clientKey, oss.str());
            std::cout << "Client connected: client_" << nid << "\n";
            return;
        }

        // Fallback: reply with ghost state
        std::ostringstream oss;
        {
            std::lock_guard<std::mutex> lk(ghostMutex);
            oss << "GHOST " << ghost.x << " " << ghost.y;
        }
        sendToClient(clientKey, oss.str());
    }

    // Not used by this server, but must be present (we’re responding directly in handleClientMessage).
    std::string generateWorldState() override {
        std::ostringstream oss;
        std::lock_guard<std::mutex> lk(ghostMutex);
        oss << "GHOST " << ghost.x << " " << ghost.y;
        return oss.str();
    }

private:
    // --- Ghost state and timeline ---
    Ghost ghost;
    Timeline ghostTime;
    std::mutex ghostMutex;

    // --- PUB socket (for ghost broadcast) ---
    void* ctx = nullptr;
    void* pub = nullptr;

    // --- Broadcaster thread (~30Hz) ---
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
        if (pub) zmq_close(pub), pub = nullptr;
        if (ctx) zmq_ctx_term(ctx), ctx = nullptr;
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

                // step ghost with timeline tick
                float gdt = (float)ghostTime.tick();
                {
                    std::lock_guard<std::mutex> lk(ghostMutex);
                    ghost.x += ghost.vx * gdt;
                    ghost.y += ghost.vy * gdt;
                    if (ghost.x < -150) {
                        ghost.x = WINDOW_WIDTH;
                        ghost.y = static_cast<float>(rand() % (WINDOW_HEIGHT - 256));
                    }
                    if (ghost.y < 0) ghost.y = 0;
                    if (ghost.y > WINDOW_HEIGHT - 256) ghost.y = WINDOW_HEIGHT - 256;

                    ghost.timer += gdt;
                    if (ghost.timer >= ghost.interval) {
                        ghost.timer = 0;
                        ghost.vy = static_cast<float>((rand() % 301) - 150);
                    }
                }

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

    std::cout << "Press Enter to stop.\n";
    std::cin.get();

    server.stopServer();
    SDL_Quit();
    return 0;
}
