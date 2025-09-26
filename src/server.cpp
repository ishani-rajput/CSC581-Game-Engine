#include "network_server.h"
#include "physics.h"
#include "collision.h"
#include "timeline.h"

#include <SDL3/SDL.h>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <mutex>
#include <cstdlib>
#include <ctime>

using namespace Engine;

const int WINDOW_WIDTH = 1920;
const int WINDOW_HEIGHT = 1080;

struct Player {
    float x = 100.f;
    float y = WINDOW_HEIGHT - 322.f;
    float vx = 0.f;
    float vy = 0.f;
    bool onGround = false;
};

struct Ghost {
    float x = 1500.f;
    float y = 600.f;
    float vx = -200.f;
    float vy = 0.f;
    float timer = 0.f;
    float interval = 2.f;
} ghost;

static inline bool AABB(float ax,float ay,float aw,float ah,
                        float bx,float by,float bw,float bh) {
    return (ax < bx + bw) && (ax + aw > bx) &&
           (ay < by + bh) && (ay + ah > by);
}

class GameServer : public NetworkServer {
public:
    GameServer() {
        Physics::setGravity(2000.f);
        srand((unsigned)time(nullptr));
        ghostTime.anchorToRealTime();
        ghostTime.setScale(1.0);
        setWorldUpdateRate(30);   // broadcast ~30 Hz
    }

protected:
    void handleClientMessage(const std::string& /*clientId*/,
                             const std::string& message) override {
        std::istringstream iss(message);
        std::string cmd; iss >> cmd;

        std::lock_guard<std::mutex> lk(mtx);

        if (cmd == "INPUT") {
            std::string id; float vx; int jump;
            iss >> id >> vx >> jump;
            auto& p = players[id];

            p.vx = vx;
            if (jump) {         
                p.vy = -900.f;
            }
            stepPlayer(p, playerTimes[id]);    
        }
        else if (cmd == "SPEED") {
            std::string id; double s; iss >> id >> s;
            playerTimes[id].setScale(s);
        }
        else if (cmd == "PAUSE") {
            std::string id, on; iss >> id >> on;
            playerTimes[id].pause(on == "ON");
        }
    }

    std::string generateWorldState() override {
        // Ghost AI (global, unaffected by client speeds)
        float gdt = (float)ghostTime.tick();
        ghost.x += ghost.vx * gdt;
        ghost.y += ghost.vy * gdt;
        if (ghost.x < -150) {
            ghost.x = WINDOW_WIDTH;
            ghost.y = (float)(rand() % (WINDOW_HEIGHT - 256));
        }
        if (ghost.y < 0) ghost.y = 0;
        if (ghost.y > WINDOW_HEIGHT - 256) ghost.y = WINDOW_HEIGHT - 256;
        ghost.timer += gdt;
        if (ghost.timer >= ghost.interval) {
            ghost.timer = 0;
            ghost.vy = (float)((rand() % 301) - 150);
        }

        // Serialize state
        std::ostringstream oss;
        std::lock_guard<std::mutex> lk(mtx);
        oss << "STATE " << ghost.x << " " << ghost.y << " " << players.size();
        for (auto& kv : players) {
            oss << " " << kv.first << " " << kv.second.x << " " << kv.second.y;
        }
        return oss.str();
    }

    void onClientConnected(const std::string& clientId) override {
        std::lock_guard<std::mutex> lk(mtx);
        players[clientId] = Player{};
        Timeline t; t.anchorToRealTime(); t.setScale(1.0);
        playerTimes[clientId] = t;
        std::cout << "Client " << clientId << " initialized\n";
    }

    void onClientDisconnected(const std::string& clientId) override {
        std::lock_guard<std::mutex> lk(mtx);
        players.erase(clientId);
        playerTimes.erase(clientId);
        std::cout << "Client " << clientId << " removed\n";
    }

private:
    std::unordered_map<std::string, Player> players;
    std::unordered_map<std::string, Timeline> playerTimes;
    Timeline ghostTime;
    std::mutex mtx;

    void stepPlayer(Player& p, Timeline& t) {
        float dt = (float)t.tick();   // ✅ per-client time
        p.vy += Physics::gravity() * dt;
        p.x  += p.vx * dt;
        p.y  += p.vy * dt;
        p.onGround = false;

        if (p.y < 0) { p.y = 0; p.vy = 0; }
        if (p.x < 0) { p.x = 0; p.vx = 0; }
        if (p.x > WINDOW_WIDTH - 256) { p.x = WINDOW_WIDTH - 256; p.vx = 0; }

        if (AABB(p.x,p.y,256,256, 0,950,1920,130)) {
            p.vy = 0; p.y = 950 - 256; p.onGround = true;
        }
        if (AABB(p.x,p.y,256,256, 700,700,256,256) ||
            AABB(p.x,p.y,256,256, ghost.x,ghost.y,256,256)) {
            p = Player{}; // reset on collision
        }
    }
};

int main() {
    GameServer server;
    server.startServer(5555);
    while (server.isRunning()) SDL_Delay(100);
    return 0;
}
