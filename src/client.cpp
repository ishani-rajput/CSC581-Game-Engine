#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <zmq.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstdio>
#include <cstring>
#include <random>
#include "entity.h"
#include "input.h"
#include "scaling.h"
#include "physics.h"
#include "collision.h"
#include "timeline.h"

const int DESIGN_WIDTH  = 1720;
const int DESIGN_HEIGHT = 1080;

const float PLAYER_SPEED   = 300.f;
const float JUMP_VELOCITY  = -1000.f;
const float STATIC_PLATFORM_W = 400.f;
const float STATIC_PLATFORM_H = 500.f;

struct RemotePlayer {
    float x=0, y=0;
    Entity* entity=nullptr;
    bool active=false;
};

static inline bool checkCollision(const SDL_FRect& a, const SDL_FRect& b) {
    return aabbIntersect(a, b);
}

static inline void clampPlayerPosition(float& playerX, float& playerY, float w, float h) {
    if (playerX < 0) playerX = 0;
    if (playerX + w > DESIGN_WIDTH) playerX = DESIGN_WIDTH - w;
    if (playerY > DESIGN_HEIGHT) playerY = DESIGN_HEIGHT - h;
}

static std::string generateClientId() {
    std::random_device rd; std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    return "client_" + std::to_string(dis(gen));
}

int main(int, char**) {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("Feeling Spikey - Multiplayer Client",
                                          DESIGN_WIDTH, DESIGN_HEIGHT, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    Scaling::setMode(ScaleMode::Proportional);

    Physics::setGravity(2000.f);

    // ---- Timeline for local player ----
    Timeline gameTimeline; 
    gameTimeline.anchorToRealTime();

    // ---- Networking (REQ/REP) ----
    void* ctx  = zmq_ctx_new();
    void* sock = zmq_socket(ctx, ZMQ_REQ);
    if (zmq_connect(sock, "tcp://localhost:5555") != 0) {
        SDL_Log("Failed to connect: %s", zmq_strerror(errno));
        return 1;
    }
    std::string clientId = generateClientId();
    SDL_Log("Client ID: %s", clientId.c_str());

    // Entities
    Entity groundBottom(renderer, "../assets/ground_bottom.png", 0, 0, 64, 64, 1, 0);
    Entity groundTop   (renderer, "../assets/ground.png",        0, 0, 64, 64, 1, 0);
    Entity platformTex (renderer, "../assets/platform.png",      0, 0, 384, 128, 1, 0);
    Entity spikeTex    (renderer, "../assets/spikes.png",        0, 0, 256, 256, 1, 0);

    const int frameCount = 8, frameWidth = 128, frameHeight = 128;
    const float playerScale = 2.0f;
    const float playerW = frameWidth * playerScale;
    const float playerH = frameHeight * playerScale;

    Entity localPlayer(renderer, "../assets/player.png", 0, 0, frameWidth, frameHeight, frameCount, 150);

    std::unordered_map<std::string, RemotePlayer> remotes;

    // Static platforms
    const float leftX = 0.f, leftY = DESIGN_HEIGHT - STATIC_PLATFORM_H;
    const float rightX = DESIGN_WIDTH - STATIC_PLATFORM_W, rightY = DESIGN_HEIGHT - STATIC_PLATFORM_H;

    // Server-controlled moving platform
    SDL_FRect movingPlat { (STATIC_PLATFORM_W + (DESIGN_WIDTH - STATIC_PLATFORM_W - 384.f))*0.5f,   
                           DESIGN_HEIGHT - STATIC_PLATFORM_H - 200.f,
                           384.f, 128.f };
    int   platDir = 1;
    float prevPlatX = movingPlat.x;

    // Local player
    float px = leftX + 100.f;
    float py = leftY - playerH;
    Body  pbody { 0.f, 0.f, true };
    float prevPX = px, prevPY = py;

    bool running=true, wasGrounded=false, grounded=false;

    while (running) {
        // -------- Events --------
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_WINDOW_RESIZED) {
                clampPlayerPosition(px, py, playerW, playerH);
            }

            // ---- Timeline Controls ----
            if (e.type == SDL_EVENT_KEY_DOWN) {
                switch (e.key.scancode) {
                    case SDL_SCANCODE_P:
                        gameTimeline.togglePause();
                        SDL_Log(gameTimeline.isPaused() ? "Paused" : "Resumed");
                        break;
                    case SDL_SCANCODE_1:
                        gameTimeline.setScale(0.5);
                        SDL_Log("Speed: 0.5x");
                        break;
                    case SDL_SCANCODE_2:
                        gameTimeline.setScale(1.0);
                        SDL_Log("Speed: 1.0x");
                        break;
                    case SDL_SCANCODE_3:
                        gameTimeline.setScale(2.0);
                        SDL_Log("Speed: 2.0x");
                        break;
                }
            }
        }

        // -------- Networking FIRST --------
        {
            char msg[256];
            snprintf(msg, sizeof(msg), "ID %s X %.3f Y %.3f", clientId.c_str(), px, py);
            zmq_send(sock, msg, strlen(msg), 0);

            char rx[8192];
            int rb = zmq_recv(sock, rx, sizeof(rx)-1, 0);
            if (rb > 0) {
                rx[rb] = '\0';

                for (auto& kv : remotes) kv.second.active = false;

                const char* line = std::strtok(rx, "\n");
                while (line) {
                    if (line[0] == 'P') {
                        float x, y, w, h; int dir;
                        if (sscanf(line, "P %f %f %f %f %d", &x, &y, &w, &h, &dir) == 5) {
                            prevPlatX    = movingPlat.x;
                            movingPlat.x = x;
                            movingPlat.y = y;
                            movingPlat.w = w;
                            movingPlat.h = h;
                            platDir = dir;
                        }
                    } else if (line[0] != 'T' && line[0] != 'N') {
                        char id[256]; float rx_, ry_;
                        if (sscanf(line, "%255s %f %f", id, &rx_, &ry_) == 3 && clientId != id) {
                            auto& r = remotes[id];
                            r.x = rx_; r.y = ry_; r.active = true;
                            if (!r.entity) {
                                r.entity = new Entity(renderer, "../assets/player.png",
                                                      0,0,frameWidth,frameHeight,frameCount,150);
                            }
                        }
                    }
                    line = std::strtok(nullptr, "\n");
                }

                for (auto it = remotes.begin(); it != remotes.end();) {
                    if (!it->second.active) {
                        if (it->second.entity) delete it->second.entity;
                        it = remotes.erase(it);
                    } else ++it;
                }
            }
        }

        // -------- Input + Physics --------
        Input::poll();

        double dt = gameTimeline.tick();
        if (dt > 0.05) dt = 0.05;

        prevPX = px; prevPY = py;
        pbody.vx = 0.f;

        if (Input::isKeyPressed(SDL_SCANCODE_A)) pbody.vx = -PLAYER_SPEED;
        if (Input::isKeyPressed(SDL_SCANCODE_D)) pbody.vx =  PLAYER_SPEED;
        if ((Input::isKeyPressed(SDL_SCANCODE_W) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) && wasGrounded) {
            pbody.vy = JUMP_VELOCITY;
        }

        Physics::step(dt * 1000, px, py, pbody);
        clampPlayerPosition(px, py, playerW, playerH);

        SDL_FRect playerRect { px, py, playerW, playerH };
        grounded = false;

        // Static platforms
        SDL_FRect leftRect  { leftX, leftY, STATIC_PLATFORM_W, STATIC_PLATFORM_H };
        SDL_FRect rightRect { rightX, rightY, STATIC_PLATFORM_W, STATIC_PLATFORM_H };
        if (checkCollision(playerRect, leftRect) && pbody.vy >= 0 && prevPY + playerH <= leftY + 20) {
            py = leftY - playerH; pbody.vy = 0; grounded = true;
        }
        if (checkCollision(playerRect, rightRect) && pbody.vy >= 0 && prevPY + playerH <= rightY + 20) {
            py = rightY - playerH; pbody.vy = 0; grounded = true;
        }

        // Moving platform
        SDL_FRect mpRect { movingPlat.x, movingPlat.y, movingPlat.w, movingPlat.h };
        if (checkCollision(playerRect, mpRect)) {
    if (pbody.vy >= 0 && prevPY + playerH <= movingPlat.y + 20) {
        py = movingPlat.y - playerH;
        pbody.vy = 0;
        // Use server deltaTime for platform movement, not client timeline
        float platDX = movingPlat.x - prevPlatX;
        px += platDX; // This is correct - you already have this
        grounded = true;
    }
}

        // Spikes
        float spikeW = 300.f, spikeH = 389.f, spikeY = DESIGN_HEIGHT - spikeH;
        for (float x = STATIC_PLATFORM_W; x < DESIGN_WIDTH - STATIC_PLATFORM_W; x += spikeW) {
            SDL_FRect spikeRect{ x, spikeY, spikeW, spikeH };
            if (checkCollision(playerRect, spikeRect)) {
                px = leftX + 100.f; py = leftY - playerH;
                pbody.vx = pbody.vy = 0;
            }
        }

        if (py > DESIGN_HEIGHT + 100) {
            px = leftX + 100.f; py = leftY - playerH;
            pbody.vx = pbody.vy = 0;
        }

        wasGrounded = grounded;
        localPlayer.update();

        // -------- Rendering --------
        SDL_SetRenderDrawColor(renderer, 135, 206, 235, 255);
        SDL_RenderClear(renderer);

        // Spikes
        for (float x = STATIC_PLATFORM_W; x < DESIGN_WIDTH - STATIC_PLATFORM_W; x += spikeW) {
            spikeTex.setPosition(x, spikeY);
            spikeTex.setSize(spikeW, spikeH);
            spikeTex.render(renderer, window);
        }

        // Static platforms
        groundBottom.setPosition(leftX, leftY);
        groundBottom.setSize(STATIC_PLATFORM_W, STATIC_PLATFORM_H);
        groundBottom.render(renderer, window);
        groundTop.setPosition(leftX, leftY - 64);
        groundTop.setSize(STATIC_PLATFORM_W, 64);
        groundTop.render(renderer, window);

        groundBottom.setPosition(rightX, rightY);
        groundBottom.setSize(STATIC_PLATFORM_W, STATIC_PLATFORM_H);
        groundBottom.render(renderer, window);
        groundTop.setPosition(rightX, rightY - 64);
        groundTop.setSize(STATIC_PLATFORM_W, 64);
        groundTop.render(renderer, window);

        // Moving platform
        platformTex.setPosition(movingPlat.x, movingPlat.y);
        platformTex.setSize(movingPlat.w, movingPlat.h);
        platformTex.render(renderer, window);

        // Local player
        localPlayer.setPosition(px, py);
        localPlayer.setSize(playerW, playerH);
        localPlayer.render(renderer, window);

        // Remote players
        for (auto& kv : remotes) {
            if (kv.second.entity) {
                kv.second.entity->setPosition(kv.second.x, kv.second.y);
                kv.second.entity->setSize(playerW, playerH);
                kv.second.entity->render(renderer, window);
            }
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    for (auto& kv : remotes) if (kv.second.entity) delete kv.second.entity;
    zmq_close(sock); zmq_ctx_destroy(ctx);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
