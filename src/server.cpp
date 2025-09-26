#include <SDL3/SDL.h>
#include <unordered_map>
#include <string>
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <ctime>
#include <zmq.h>

#include "physics.h"
#include "collision.h"

const int WINDOW_WIDTH = 1920;
const int WINDOW_HEIGHT = 1080;

static inline std::vector<std::string> split_ws(const std::string& s) {
    std::istringstream iss(s);
    std::vector<std::string> out;
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

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

inline bool AABB(float ax,float ay,float aw,float ah,float bx,float by,float bw,float bh){
    return (ax < bx + bw) && (ax + aw > bx) && (ay < by + bh) && (ay + ah > by);
}

int main() {
    if (!SDL_Init(0)) {
        SDL_Log("SDL init failed: %s", SDL_GetError());
        return 1;
    }
    srand((unsigned)time(nullptr));
    Physics::setGravity(2000.f);

    void* ctx = zmq_ctx_new();
    void* rep = zmq_socket(ctx, ZMQ_REP);
    void* pub = zmq_socket(ctx, ZMQ_PUB);
    zmq_bind(rep, "tcp://*:5555");
    zmq_bind(pub, "tcp://*:5556");

    int hwm = 1;
    zmq_setsockopt(pub, ZMQ_SNDHWM, &hwm, sizeof(hwm));

    std::unordered_map<int, Player> players;
    int nextId = 1;

    Uint64 prev = SDL_GetTicks();
    double accum = 0.0;

    while (true) {
        Uint64 now = SDL_GetTicks();
        double dt = (now - prev) / 1000.0;
        prev = now;

        // Handle input
        char buf[256];
        int n = zmq_recv(rep, buf, sizeof(buf)-1, ZMQ_DONTWAIT);
        if (n >= 0) {
            buf[n] = 0;
            auto toks = split_ws(buf);
            std::string reply = "ERR";
            if (toks.size() >= 1 && toks[0] == "JOIN") {
                int id = nextId++;
                players[id] = Player{};
                reply = "ASSIGN " + std::to_string(id);
            } else if (toks.size() == 4 && toks[0] == "INPUT") {
                int id = std::stoi(toks[1]);
                float vx = std::stof(toks[2]);
                int jump = std::stoi(toks[3]);
                if (players.count(id)) {
                    auto& p = players[id];
                    p.vx = vx;
                    if (jump && p.onGround) {
                        p.vy = -900.f;
                        p.onGround = false;
                    }
                    reply = "OK";
                }
            }
            zmq_send(rep, reply.c_str(), (int)reply.size(), 0);
        }

        // Simulate players
        for (auto& kv : players) {
            auto& p = kv.second;
            p.vy += Physics::gravity() * dt;
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            p.onGround = false;

            if (p.y < 0) { p.y = 0; p.vy = 0; }
            if (p.x < 0) { p.x = 0; p.vx = 0; }
            if (p.x > WINDOW_WIDTH - 256) { p.x = WINDOW_WIDTH - 256; p.vx = 0; }

            if (AABB(p.x,p.y,256,256, 0,950,1920,130)) {
                p.vy = 0;
                p.y = 950 - 256;
                p.onGround = true;
            }
            if (AABB(p.x,p.y,256,256, 700,700,256,256)) {
                p = Player{};
            }
            if (AABB(p.x,p.y,256,256, ghost.x,ghost.y,256,256)) {
                p = Player{};
            }
        }

        // Ghost AI
        ghost.x += ghost.vx * dt;
        ghost.y += ghost.vy * dt;
        if (ghost.x < -150) {
            ghost.x = WINDOW_WIDTH;
            ghost.y = (float)(rand() % (WINDOW_HEIGHT - 256));
        }
        if (ghost.y < 0) ghost.y = 0;
        if (ghost.y > WINDOW_HEIGHT - 256) ghost.y = WINDOW_HEIGHT - 256;
        ghost.timer += dt;
        if (ghost.timer >= ghost.interval) {
            ghost.timer = 0;
            ghost.vy = (float)((rand() % 301) - 150);
        }

        // Broadcast state @ 30Hz
        accum += dt;
        if (accum >= 1.0/30.0) {
            accum = 0;
            std::ostringstream oss;
            oss << "STATE " << ghost.x << " " << ghost.y << " " << players.size();
            for (auto& kv : players) {
                oss << " " << kv.first << " " << kv.second.x << " " << kv.second.y;
            }
            std::string msg = oss.str();
            zmq_send(pub, msg.c_str(), (int)msg.size(), 0);
        }

        SDL_Delay(1);
    }

    zmq_close(pub);
    zmq_close(rep);
    zmq_ctx_term(ctx);
    SDL_Quit();
    return 0;
}
