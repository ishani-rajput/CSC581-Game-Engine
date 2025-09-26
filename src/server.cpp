#include <zmq.h>
#include <unordered_map>
#include <string>
#include <iostream>
#include <cstdio>
#include <cstring>
#include <chrono>

static constexpr int DESIGN_WIDTH  = 1720;
static constexpr int DESIGN_HEIGHT = 1080;

// Level layout from your single-player
static constexpr float STATIC_PLATFORM_W = 400.f;   // left/right ground column width
static constexpr float STATIC_PLATFORM_H = 500.f;   // left/right ground column height

// Moving platform shape (matches your asset and main.cpp)
static constexpr float MP_W = 384.f;
static constexpr float MP_H = 128.f;

// Same vertical placement as your single-player: 1080 - 500 - 200 = 380
static constexpr float MP_Y = DESIGN_HEIGHT - STATIC_PLATFORM_H - 200.f; // 380

// Horizontal bounds so it moves in the middle corridor
static constexpr float MP_MIN_X = STATIC_PLATFORM_W;                            // 400
static constexpr float MP_MAX_X = DESIGN_WIDTH - STATIC_PLATFORM_W - MP_W;      // 1720 - 400 - 384 = 936

struct Vec2 { float x=0, y=0; };

struct Platform {
    float x, y, w, h;
    float speed;
    int dir; // +1 right, -1 left
};

int main() {
    void* ctx = zmq_ctx_new();
    void* rep = zmq_socket(ctx, ZMQ_REP);
    if (zmq_bind(rep, "tcp://*:5555") != 0) {
        std::cerr << "Bind failed: " << zmq_strerror(errno) << "\n";
        return 1;
    }

    std::unordered_map<std::string, Vec2> players;

    Platform moving{ (MP_MIN_X + MP_MAX_X) * 0.5f, MP_Y, MP_W, MP_H, 200.f, 1 };

    auto start    = std::chrono::steady_clock::now();
    auto lastTick = start;

    char inbuf[512];
    char outbuf[8192];

    std::cout << "Server listening on 5555...\n";
    while (true) {
        int rb = zmq_recv(rep, inbuf, sizeof(inbuf)-1, 0);
        if (rb <= 0) {
            if (errno == ETERM) break;
            continue;
        }
        inbuf[rb] = '\0';

        // Expect: "ID <id> X <x> Y <y>"
        char id[256]; float x=0, y=0;
        if (sscanf(inbuf, "ID %255s X %f Y %f", id, &x, &y) == 3) {
            players[id] = Vec2{ x, y };
        }

        // Advance world time & simulate platform
        auto now       = std::chrono::steady_clock::now();
        float serverT  = std::chrono::duration<float>(now - start).count();
        float dt       = std::chrono::duration<float>(now - lastTick).count();
        lastTick       = now;

        moving.x += moving.speed * moving.dir * dt;
        if (moving.x < MP_MIN_X) {
            moving.x = MP_MIN_X; moving.dir = +1;
        } else if (moving.x + moving.w > MP_MAX_X + moving.w) {
            moving.x = MP_MAX_X; moving.dir = -1;
        }

        // Build response: time, players, platform (send w/h too!)
        int offset = snprintf(outbuf, sizeof(outbuf), "T %.3f\nN %zu\n", serverT, players.size());
        for (auto& kv : players) {
            offset += snprintf(outbuf + offset, sizeof(outbuf) - offset,
                               "%s %.3f %.3f\n", kv.first.c_str(), kv.second.x, kv.second.y);
        }
        offset += snprintf(outbuf + offset, sizeof(outbuf) - offset,
                           "P %.3f %.3f %.3f %.3f %d\n",
                           moving.x, moving.y, moving.w, moving.h, moving.dir);

        zmq_send(rep, outbuf, offset, 0);
    }

    zmq_close(rep);
    zmq_ctx_destroy(ctx);
    return 0;
}
