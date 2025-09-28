// Headless ghost authority server (hybrid P2P Option 1)
// - REP on 5555: minimal CONNECT handshake (returns CONNECTED <client_####>)
// - PUB on 5556: broadcasts GHOST gx gy at ~30Hz

#include <SDL3/SDL.h>
#include <cstdlib>
#include <ctime>
#include <string>
#include <sstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <zmq.h>

#include "physics.h"
#include "timeline.h"

static const int WINDOW_WIDTH = 1920;
static const int WINDOW_HEIGHT = 1080;

struct Ghost {
    float x = 1500.f;
    float y = 600.f;
    float vx = -200.f;
    float vy = 0.f;
    float timer = 0.f;
    float interval = 2.f;
} ghost;

int main() {
    srand(static_cast<unsigned>(time(nullptr)));
    Physics::setGravity(2000.f);

    void* ctx = zmq_ctx_new();
    if (!ctx) { std::cerr << "ZMQ ctx failed\n"; return 1; }

    // PUB for ghost state
    void* pub = zmq_socket(ctx, ZMQ_PUB);
    if (!pub) { std::cerr << "ZMQ pub failed\n"; return 1; }
    if (zmq_bind(pub, "tcp://*:5556") != 0) {
        std::cerr << "PUB bind 5556 failed: " << zmq_strerror(errno) << "\n";
        return 1;
    }
    int hwm = 1;
    zmq_setsockopt(pub, ZMQ_SNDHWM, &hwm, sizeof(hwm));

    // REP for simple CONNECT handshake (assign pseudo id)
    void* rep = zmq_socket(ctx, ZMQ_REP);
    if (!rep) { std::cerr << "ZMQ rep failed\n"; return 1; }
    if (zmq_bind(rep, "tcp://*:5555") != 0) {
        std::cerr << "REP bind 5555 failed: " << zmq_strerror(errno) << "\n";
        return 1;
    }
    int rcvto = 0; // blocking
    zmq_setsockopt(rep, ZMQ_RCVTIMEO, &rcvto, sizeof(rcvto));

    std::cout << "Server started. CONNECT on 5555, GHOST PUB on 5556\n";

    // Ghost timeline — global (not affected by any client speed)
    Timeline ghostTime; ghostTime.anchorToRealTime();

    std::atomic<bool> running(true);

    // Ghost broadcast thread (~30Hz)
    std::thread broadcaster([&](){
        double accum = 0.0;
        Uint64 prev = SDL_GetTicks();
        while (running.load()) {
            Uint64 now = SDL_GetTicks();
            double dt = (now - prev) / 1000.0;
            prev = now;
            accum += dt;

            // step ghost with timeline tick
            float gdt = (float)ghostTime.tick();
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

            if (accum >= 1.0/30.0) {
                accum = 0.0;
                std::ostringstream oss;
                oss << "GHOST " << ghost.x << " " << ghost.y;
                const std::string msg = oss.str();
                zmq_send(pub, msg.c_str(), (int)msg.size(), 0);
            }

            SDL_Delay(1);
        }
    });

    // Handshake loop
    char buf[128];
    while (true) {
        int n = zmq_recv(rep, buf, sizeof(buf)-1, 0);
        if (n < 0) continue;
        buf[n] = 0;
        std::string req(buf);
        if (req.rfind("CONNECT", 0) == 0) {
            // generate a simple pseudo-id for the client: client_#### (1..9999)
            int nid = (rand() % 9000) + 1000;
            std::ostringstream oss;
            oss << "CONNECTED client_" << nid;
            const std::string reply = oss.str();
            zmq_send(rep, reply.c_str(), (int)reply.size(), 0);
            std::cout << "Client connected: client_" << nid << "\n";
        } else {
            // Fallback — reply with ghost state anyway
            std::ostringstream oss;
            oss << "GHOST " << ghost.x << " " << ghost.y;
            const std::string reply = oss.str();
            zmq_send(rep, reply.c_str(), (int)reply.size(), 0);
        }
    }

    running = false;
    broadcaster.join();

    zmq_close(rep);
    zmq_close(pub);
    zmq_ctx_term(ctx);
    SDL_Quit();
    return 0;
}
