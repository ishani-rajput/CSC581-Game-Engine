#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <unordered_map>
#include <string>
#include <sstream>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <zmq.h>

#include "entity.h"
#include "input.h"
#include "scaling.h"
#include "timeline.h"

const int WINDOW_WIDTH = 1920;
const int WINDOW_HEIGHT = 1080;

// Assets
const char* PLAYER_ASSET   = "../assets/player.png";
const char* GHOST_ASSET    = "../assets/ghost.png";
const char* PLATFORM_ASSET = "../assets/platform.png";
const char* GRAVE_ASSET    = "../assets/grave.png";
const char* BG_SKY_ASSET   = "../assets/background_sky.png";

static inline std::vector<std::string> split_ws(const std::string& s) {
    std::istringstream iss(s);
    std::vector<std::string> out;
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

struct Pos { float x, y; };

int main(int argc, char** argv) {
    const char* serverHost = (argc >= 2) ? argv[1] : "127.0.0.1";

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Witch Runner (Client)", WINDOW_WIDTH, WINDOW_HEIGHT,
                                     SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_Log("Window creation failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    Scaling::setMode(ScaleMode::Pixel);
    srand(static_cast<unsigned>(time(nullptr)));

    SDL_Texture* bgSky = IMG_LoadTexture(renderer, BG_SKY_ASSET);
    Entity platform(renderer, PLATFORM_ASSET, 0, 950, 1920, 130, 1, 0);
    Entity grave(renderer, GRAVE_ASSET, 700, 700, 256, 256, 1, 0);
    Entity ghost(renderer, GHOST_ASSET, 1500, 600, 256, 256, 1, 0);

    std::unordered_map<int, Entity*> players;

    // ---- Networking ----
    void* ctx = zmq_ctx_new();
    void* req = zmq_socket(ctx, ZMQ_REQ);
    void* sub = zmq_socket(ctx, ZMQ_SUB);
    std::string repAddr = std::string("tcp://")+serverHost+":5555";
    std::string pubAddr = std::string("tcp://")+serverHost+":5556";
    zmq_connect(req, repAddr.c_str());
    zmq_connect(sub, pubAddr.c_str());
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, "", 0);

    // JOIN
    std::string join = "JOIN me";
    zmq_send(req, join.c_str(), (int)join.size(), 0);
    char buf[128];
    int n = zmq_recv(req, buf, sizeof(buf)-1, 0);
    buf[n] = 0;
    auto toks = split_ws(buf);
    int myId = (toks.size() == 2 && toks[0] == "ASSIGN") ? std::stoi(toks[1]) : -1;
    if (myId < 0) {
        std::cerr << "Join failed\n";
        return 2;
    }

    // Create my sprite
    players[myId] = new Entity(renderer, PLAYER_ASSET, 100, WINDOW_HEIGHT-322.f, 256, 256, 1, 0);

    Pos ghostPos{1500, 600};
    std::unordered_map<int, Pos> latestPlayers;

    // ---- Local time controls ----
    Timeline gameTime;
    gameTime.anchorToRealTime();
    bool prevT = false;

    SDL_Event ev;
    bool running = true;

    while (running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
        }

        Input::poll();

        // === Scaling toggle ===
        bool tNow = Input::isKeyPressed(SDL_SCANCODE_T);
        if (tNow && !prevT) {
            ScaleMode current = Scaling::mode();
            Scaling::setMode(current == ScaleMode::Pixel ? ScaleMode::Proportional : ScaleMode::Pixel);
            SDL_Log("Scaling mode changed to: %s",
                (Scaling::mode() == ScaleMode::Pixel ? "Pixel" : "Proportional"));
        }
        prevT = tNow;

        // === Local pause & speed ===
        if (Input::isKeyPressed(SDL_SCANCODE_P)) {
            gameTime.togglePause();
            SDL_Log("Game %s", gameTime.isPaused() ? "paused" : "resumed");
        }
        if (Input::isKeyPressed(SDL_SCANCODE_1)) { gameTime.setScale(0.5); SDL_Log("Speed 0.5x"); }
        if (Input::isKeyPressed(SDL_SCANCODE_2)) { gameTime.setScale(1.0); SDL_Log("Speed 1.0x"); }
        if (Input::isKeyPressed(SDL_SCANCODE_3)) { gameTime.setScale(2.0); SDL_Log("Speed 2.0x"); }

        float delta = static_cast<float>(gameTime.tick());

        // === Input to server ===
        float vx = 0.f; int jump = 0;
        if (Input::isKeyPressed(SDL_SCANCODE_A)) vx = -400.f;
        else if (Input::isKeyPressed(SDL_SCANCODE_D)) vx = 400.f;
        if (Input::isKeyPressed(SDL_SCANCODE_SPACE)) jump = 1;

        std::ostringstream oss;
        oss << "INPUT " << myId << " " << vx << " " << jump;
        std::string msg = oss.str();
        zmq_send(req, msg.c_str(), (int)msg.size(), 0);
        zmq_recv(req, buf, sizeof(buf)-1, 0);

        // === Receive state ===
        for (;;) {
            char sbuf[2048];
            int m = zmq_recv(sub, sbuf, sizeof(sbuf)-1, ZMQ_DONTWAIT);
            if (m < 0) break;
            sbuf[m] = 0;
            auto tk = split_ws(sbuf);
            if (tk.size() >= 4 && tk[0] == "STATE") {
                ghostPos.x = std::stof(tk[1]);
                ghostPos.y = std::stof(tk[2]);
                int count = std::stoi(tk[3]);
                latestPlayers.clear();
                int idx = 4;
                for (int i = 0; i < count && idx + 2 <= (int)tk.size(); i++) {
                    int id = std::stoi(tk[idx++]);
                    float x = std::stof(tk[idx++]);
                    float y = std::stof(tk[idx++]);
                    latestPlayers[id] = {x, y};
                    if (!players.count(id)) {
                        players[id] = new Entity(renderer, PLAYER_ASSET, x, y, 256, 256, 1, 0);
                    }
                }
            }
        }

        // === Apply state (respect pause/scale) ===
        if (!gameTime.isPaused()) {
            for (auto& kv : latestPlayers) {
                if (players.count(kv.first)) {
                    players[kv.first]->setPosition(kv.second.x, kv.second.y);
                    players[kv.first]->update();
                }
            }
            ghost.setPosition(ghostPos.x, ghostPos.y);
            ghost.update();
        }

        // === Rendering ===
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        if (bgSky) SDL_RenderTexture(renderer, bgSky, nullptr, nullptr);
        platform.render(renderer, window);
        grave.render(renderer, window);
        ghost.render(renderer, window);
        for (auto& kv : players) {
            kv.second->render(renderer, window);
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    for (auto& kv : players) delete kv.second;
    if (bgSky) SDL_DestroyTexture(bgSky);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    zmq_close(sub); zmq_close(req); zmq_ctx_term(ctx);
    SDL_Quit();
    return 0;
}
