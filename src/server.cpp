// Server with per-client asynchronous handling
// Each client has its own thread and timeline

#include <SDL3/SDL.h>
#include <unordered_map>
#include <string>
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <vector>
#include <zmq.h>

#include "physics.h"
#include "collision.h"
#include "timeline.h"

const int WINDOW_WIDTH = 1920;
const int WINDOW_HEIGHT = 1080;

struct Player {
    float x = 100.f;
    float y = WINDOW_HEIGHT - 322.f;
    float vx = 0.f;
    float vy = 0.f;
    bool onGround = false;
    bool graveTouched = false;
};

struct Ghost {
    float x = 1500.f;
    float y = 600.f;
    float vx = -200.f;
    float vy = 0.f;
    float timer = 0.f;
    float interval = 2.f;
} ghost;

static inline std::vector<std::string> split_ws(const std::string& s) {
    std::istringstream iss(s);
    std::vector<std::string> out;
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

inline bool AABB(float ax,float ay,float aw,float ah,float bx,float by,float bw,float bh){
    return (ax < bx + bw) && (ax + aw > bx) && (ay < by + bh) && (ay + ah > by);
}

std::mutex mtx;
std::unordered_map<int, Player> players;
std::atomic<bool> serverRunning(true);

void client_thread(void* ctx, int id) {
    void* rep = zmq_socket(ctx, ZMQ_REP);
    std::string bindAddr = "tcp://*:" + std::to_string(6000 + id);
    zmq_bind(rep, bindAddr.c_str());

    int linger0 = 0;
    zmq_setsockopt(rep, ZMQ_LINGER, &linger0, sizeof(linger0));

    Timeline timeline;
    timeline.anchorToRealTime();
    timeline.setScale(1.0);

    char buf[256];
    while (serverRunning) {
        int n = zmq_recv(rep, buf, sizeof(buf)-1, ZMQ_DONTWAIT);
        if (n > 0) {
            buf[n] = 0;
            auto toks = split_ws(buf);
            std::string reply = "ERR";
            if (toks.size() >= 1) {
                if (toks[0] == "INPUT" && toks.size() == 4) {
                    float vx = std::stof(toks[2]);
                    int jump = std::stoi(toks[3]);
                    std::lock_guard<std::mutex> lk(mtx);
                    Player& p = players[id];
                    p.vx = vx;
                    if (jump) {   // Flappy Bird style: always flap
                        p.vy = -900.f;
                        p.onGround = false;
                    }
                    reply = "OK";
                } else if (toks[0] == "SPEED" && toks.size() == 2) {
                    timeline.setScale(std::stod(toks[1]));
                    reply = "OK";
                } else if (toks[0] == "PAUSE" && toks.size() == 2) {
                    timeline.pause(toks[1] == "ON");
                    reply = "OK";
                }
            }
            zmq_send(rep, reply.c_str(), (int)reply.size(), 0);
        }

        if (timeline.isPaused()) {
            SDL_Delay(1);
            continue;
        }

        float dt = static_cast<float>(timeline.tick());
        {
            std::lock_guard<std::mutex> lk(mtx);
            Player& p = players[id];

            // Integrate
            p.vy += Physics::gravity() * dt;
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            p.onGround = false;

            // World bounds
            if (p.y < 0) { p.y = 0; p.vy = 0; }
            if (p.x < 0) { p.x = 0; p.vx = 0; }
            if (p.x > WINDOW_WIDTH - 256) { p.x = WINDOW_WIDTH - 256; p.vx = 0; }

            // Platform (0,950,1920,130)
            if (AABB(p.x,p.y,256,256, 0,950,1920,130)) {
                p.vy = 0;
                p.y = 950 - 256;
                p.onGround = true;
            }

            // Collisions with grave or ghost reset player
            if (AABB(p.x,p.y,256,256, 700,700,256,256) ||
                AABB(p.x,p.y,256,256, ghost.x,ghost.y,256,256)) {
                p = Player{};
            }
        }

        SDL_Delay(1);
    }

    zmq_close(rep);
}

int main() {
    Physics::setGravity(2000.f);
    srand(static_cast<unsigned>(time(nullptr)));

    void* ctx = zmq_ctx_new();

    // PUB for world snapshots
    void* pub = zmq_socket(ctx, ZMQ_PUB);
    zmq_bind(pub, "tcp://*:5556");
    int hwm = 1;
    zmq_setsockopt(pub, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    int linger0 = 0;
    zmq_setsockopt(pub, ZMQ_LINGER, &linger0, sizeof(linger0));

    int nextId = 1;
    std::map<int, std::thread> clientThreads;

    // JOIN listener
    void* joinSock = zmq_socket(ctx, ZMQ_REP);
    zmq_bind(joinSock, "tcp://*:5555");
    zmq_setsockopt(joinSock, ZMQ_LINGER, &linger0, sizeof(linger0));

    // Ghost timeline
    Timeline ghostTime;
    ghostTime.anchorToRealTime();

    double accum = 0.0;
    Uint64 prev = SDL_GetTicks();

    std::cout << "Server started. Listening for JOINs on port 5555, publishing STATE on port 5556...\n";

    while (serverRunning) {
        Uint64 now = SDL_GetTicks();
        double dt = (now - prev) / 1000.0;
        prev = now;
        ghost.timer += static_cast<float>(dt);

        // JOIN requests
        char buf[128];
        int n = zmq_recv(joinSock, buf, sizeof(buf)-1, ZMQ_DONTWAIT);
        if (n > 0) {
            buf[n] = 0;
            auto toks = split_ws(buf);
            if (!toks.empty() && toks[0] == "JOIN") {
                int id = nextId++;
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    players[id] = Player{};
                }
                clientThreads[id] = std::thread(client_thread, ctx, id);
                std::string reply = "ASSIGN " + std::to_string(id);
                zmq_send(joinSock, reply.c_str(), (int)reply.size(), 0);
                std::cout << "Client joined with id=" << id << "\n";
            }
        }

        // Ghost AI step
        float gdt = static_cast<float>(ghostTime.tick());
        ghost.x += ghost.vx * gdt;
        ghost.y += ghost.vy * gdt;

        if (ghost.x < -150) {
            ghost.x = WINDOW_WIDTH;
            ghost.y = static_cast<float>(rand() % (WINDOW_HEIGHT - 256));
        }
        if (ghost.y < 0) ghost.y = 0;
        if (ghost.y > WINDOW_HEIGHT - 256) ghost.y = WINDOW_HEIGHT - 256;

        if (ghost.timer >= ghost.interval) {
            ghost.timer = 0;
            ghost.vy = static_cast<float>((rand() % 301) - 150);
        }

        // Broadcast state ~30Hz
        accum += dt;
        if (accum >= 1.0 / 30.0) {
            accum = 0.0;
            std::ostringstream oss;
            {
                std::lock_guard<std::mutex> lk(mtx);
                oss << "STATE " << ghost.x << " " << ghost.y << " " << players.size();
                for (auto& kv : players) {
                    oss << " " << kv.first << " " << kv.second.x << " " << kv.second.y;
                }
            }
            std::string msg = oss.str();
            zmq_send(pub, msg.c_str(), (int)msg.size(), 0);
        }

        SDL_Delay(1);
    }

    serverRunning = false;
    for (auto& kv : clientThreads) {
        if (kv.second.joinable()) kv.second.join();
    }

    zmq_close(pub);
    zmq_close(joinSock);
    zmq_ctx_term(ctx);
    SDL_Quit();
    return 0;
}
