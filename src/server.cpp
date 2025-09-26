#include <zmq.h>
#include <unordered_map>
#include <string>
#include <iostream>
#include <cstdio>
#include <cstring>

// Simple 2D vector
struct Vec2 { float x=0, y=0; };

int main(){
    void* ctx = zmq_ctx_new();
    void* rep = zmq_socket(ctx, ZMQ_REP);
    if (zmq_bind(rep, "tcp://*:5555") != 0) {
        std::cerr << "Bind failed: " << zmq_strerror(errno) << "\n";
        return 1;
    }

    std::unordered_map<std::string, Vec2> players;

    char inbuf[512];
    char outbuf[8192];

    std::cout << "Server listening on 5555..." << std::endl;
    while (true) {
        int rb = zmq_recv(rep, inbuf, sizeof(inbuf)-1, 0);
        if (rb <= 0) {
            if (errno == ETERM) break; // shutdown
            continue;
        }
        inbuf[rb] = '\0';

        // Expect: "ID <id> X <x> Y <y>"
        char id[256]; float x=0,y=0;
        if (sscanf(inbuf, "ID %255s X %f Y %f", id, &x, &y) == 3) {
            players[id] = Vec2{ x,y };
            std::cout << "Updated player " << id << " at (" << x << ", " << y << ")" << std::endl;
        }

        // Build response: all players
        int offset = snprintf(outbuf, sizeof(outbuf), "N %zu\n", players.size());
        for (auto& kv : players) {
            offset += snprintf(outbuf + offset, sizeof(outbuf) - offset,
                               "%s %.3f %.3f\n",
                               kv.first.c_str(), kv.second.x, kv.second.y);
        }

        zmq_send(rep, outbuf, strlen(outbuf), 0);
    }

    zmq_close(rep);
    zmq_ctx_destroy(ctx);
    return 0;
}