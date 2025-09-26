#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <zmq.h>

#include "entity.h"
#include "input.h"
#include "scaling.h"
#include "physics.h"

const int WINDOW_WIDTH = 1920;
const int WINDOW_HEIGHT = 1080;

const char* PLAYER_ASSET   = "../assets/player.png";
const char* GHOST_ASSET    = "../assets/ghost.png";
const char* PLATFORM_ASSET = "../assets/platform.png";
const char* GRAVE_ASSET    = "../assets/grave.png";
const char* BG_SKY_ASSET   = "../assets/background_sky.png";

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Witch Runner (Client)",
                                     WINDOW_WIDTH, WINDOW_HEIGHT,
                                     SDL_WINDOW_RESIZABLE,
                                     &window, &renderer)) return 1;

    Scaling::setMode(ScaleMode::Pixel);
    Physics::setGravity(2000.f);

    void* ctx = zmq_ctx_new();
    void* req = zmq_socket(ctx, ZMQ_REQ);
    zmq_connect(req, "tcp://localhost:5555");

    // CONNECT handshake
    std::string connectMsg = "CONNECT";
    zmq_send(req, connectMsg.c_str(), connectMsg.size(), 0);
    char buf[1024]; int n = zmq_recv(req, buf, sizeof(buf)-1, 0); buf[n] = 0;

    std::string clientId;
    { std::istringstream iss(buf); std::string tag; iss >> tag >> clientId; }
    std::cout << "Connected as " << clientId << std::endl;

    // Assets
    SDL_Texture* bgSky = IMG_LoadTexture(renderer, BG_SKY_ASSET);
    Entity platformE(renderer, PLATFORM_ASSET, 0, 950, 1920, 130, 1, 0);
    Entity graveE(renderer, GRAVE_ASSET, 700, 700, 256, 256, 1, 0);
    Entity ghostE(renderer, GHOST_ASSET, 1500, 600, 256, 256, 1, 0);
    Entity playerE(renderer, PLAYER_ASSET, 100, WINDOW_HEIGHT - 322.f, 256, 256, 1, 0);

    std::unordered_map<std::string, Entity*> players;
    players[clientId] = &playerE;

    bool running = true;
    bool prevT = false;
    bool paused = false;
    bool prevJump = false;
    SDL_Event ev;

    auto send_cmd = [&](const std::string& msg) {
        zmq_send(req, msg.c_str(), msg.size(), 0);
        int r = zmq_recv(req, buf, sizeof(buf)-1, 0);
        if (r > 0) buf[r] = 0;
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

        // Pause / Speed (affects only my server-side timeline)
        if (Input::isKeyPressed(SDL_SCANCODE_P)) {
            paused = !paused;
            std::ostringstream oss;
            oss << "PAUSE " << clientId << " " << (paused ? "ON" : "OFF");
            send_cmd(oss.str());
        }
        if (Input::isKeyPressed(SDL_SCANCODE_1)) {
            std::ostringstream oss; oss << "SPEED " << clientId << " 0.5";
            send_cmd(oss.str());
        }
        if (Input::isKeyPressed(SDL_SCANCODE_2)) {
            std::ostringstream oss; oss << "SPEED " << clientId << " 1.0";
            send_cmd(oss.str());
        }
        if (Input::isKeyPressed(SDL_SCANCODE_3)) {
            std::ostringstream oss; oss << "SPEED " << clientId << " 2.0";
            send_cmd(oss.str());
        }

        // Movement input (send every frame)
        float vx = 0;
        if (Input::isKeyPressed(SDL_SCANCODE_A)) vx = -400.f;
        else if (Input::isKeyPressed(SDL_SCANCODE_D)) vx = 400.f;

        bool jumpNow = Input::isKeyPressed(SDL_SCANCODE_SPACE);
        bool jump = jumpNow && !prevJump;  // ✅ edge trigger
        prevJump = jumpNow;

        {
            std::ostringstream oss;
            oss << "INPUT " << clientId << " " << vx << " " << (jump ? 1 : 0);
            send_cmd(oss.str());
        }

        // Parse world state (authoritative — apply to everyone, including me)
        std::string worldMsg(buf);
        if (worldMsg.rfind("STATE", 0) == 0) {
            std::istringstream iss(worldMsg);
            std::string header; iss >> header;
            float gx, gy; int count;
            iss >> gx >> gy >> count;
            ghostE.setPosition(gx, gy);

            for (int i=0;i<count;i++) {
                std::string pid; float px, py;
                iss >> pid >> px >> py;
                if (players.find(pid) == players.end()) {
                    players[pid] = new Entity(renderer, PLAYER_ASSET, px, py, 256, 256, 1, 0);
                }
                players[pid]->setPosition(px, py);  // ✅ update self + others
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

    for (auto& kv : players) if (kv.first != clientId) delete kv.second;
    if (bgSky) SDL_DestroyTexture(bgSky);
    zmq_close(req); zmq_ctx_term(ctx);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
