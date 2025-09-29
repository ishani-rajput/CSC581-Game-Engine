#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <random>
#include <sstream>
#include "entity.h"
#include "input.h"
#include "scaling.h"
#include "physics.h"
#include "collision.h"
#include "timeline.h"
#include "peer_manager.h"

const int DESIGN_WIDTH = 1720;
const int DESIGN_HEIGHT = 1080;
const float PLAYER_SPEED = 300.f;
const float JUMP_VELOCITY = -1000.f;
const float STATIC_PLATFORM_W = 400.f;
const float STATIC_PLATFORM_H = 500.f;

static inline bool checkCollision(const SDL_FRect& a, const SDL_FRect& b) {
    return aabbIntersect(a, b);
}

static inline void clampPlayerPosition(float& px, float& py, float w, float h) {
    if (px < 0) px = 0;
    if (px + w > DESIGN_WIDTH) px = DESIGN_WIDTH - w;
    if (py > DESIGN_HEIGHT) py = DESIGN_HEIGHT - h;
}

std::string generateClientId() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    return "client_" + std::to_string(dis(gen));
}

int main(int, char**) {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("Spikey - Section 5 Hybrid P2P",
                                          DESIGN_WIDTH, DESIGN_HEIGHT, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    Scaling::setMode(ScaleMode::Proportional);
    Physics::setGravity(2000.f);

    // Timeline
    Timeline gameTimeline;
    gameTimeline.anchorToRealTime();

    // Initialize PeerManager for Section 5
    std::string clientId = generateClientId();
    PeerManager peerManager(clientId);
    
    // Generate random port for this peer's PUB socket
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> portDis(6000, 6999);
    int myPubPort = portDis(gen);
    std::string myPubEndpoint = "tcp://*:" + std::to_string(myPubPort);
    std::string myPubAddress = "tcp://localhost:" + std::to_string(myPubPort);
    
    SDL_Log("Client ID: %s", clientId.c_str());
    SDL_Log("My PUB endpoint: %s", myPubAddress.c_str());
    
    // Setup hybrid networking
    peerManager.connectToServer("tcp://localhost:5555");
    peerManager.startPeerListener(myPubEndpoint);
    
    // Register with server
    std::string regMsg = "REGISTER_PEER " + clientId + " " + myPubAddress;
    peerManager.sendToServer(regMsg);
    
    // Get initial peer list and connect to them
    std::string serverResponse = peerManager.receiveFromServer();
    std::istringstream iss(serverResponse);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("PEER") == 0) {
            std::istringstream peerStream(line);
            std::string cmd, peerId, peerEndpoint;
            if (peerStream >> cmd >> peerId >> peerEndpoint && peerId != clientId) {
                SDL_Log("Connecting to peer: %s at %s", peerId.c_str(), peerEndpoint.c_str());
                peerManager.connectToPeerNetwork(peerEndpoint);
            }
        }
    }

    // Entities
    Entity groundBottom(renderer, "../assets/ground_bottom.png", 0, 0, 64, 64, 1, 0);
    Entity groundTop(renderer, "../assets/ground.png", 0, 0, 64, 64, 1, 0);
    Entity platformTex(renderer, "../assets/platform.png", 0, 0, 384, 128, 1, 0);
    Entity spikeTex(renderer, "../assets/spikes.png", 0, 0, 256, 256, 1, 0);

    const int frameCount = 8, frameWidth = 128, frameHeight = 128;
    const float playerScale = 2.0f;
    const float playerW = frameWidth * playerScale;
    const float playerH = frameHeight * playerScale;
    Entity localPlayer(renderer, "../assets/player.png", 0, 0, frameWidth, frameHeight, frameCount, 150);

    // Track remote players
    std::unordered_map<std::string, Entity*> remotePlayerEntities;

    const float leftX = 0.f, leftY = DESIGN_HEIGHT - STATIC_PLATFORM_H;
    const float rightX = DESIGN_WIDTH - STATIC_PLATFORM_W, rightY = DESIGN_HEIGHT - STATIC_PLATFORM_H;

    // Server-controlled moving platform
    SDL_FRect movingPlat{DESIGN_WIDTH / 2.f - 192, DESIGN_HEIGHT - STATIC_PLATFORM_H - 200, 384, 128};
    int platDir = 1;
    float prevPlatX = movingPlat.x;

    // Local player
    float px = leftX + 100.f, py = leftY - playerH;
    Body pbody{0.f, 0.f, true};
    bool running = true, wasGrounded = false, grounded = false;

    float serverTimer = 0.0f;
    float peerTimer = 0.0f;
    float cleanupTimer = 0.0f;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_WINDOW_RESIZED) {
                clampPlayerPosition(px, py, playerW, playerH);
            }

            if (e.type == SDL_EVENT_KEY_DOWN) {
                switch (e.key.scancode) {
                    case SDL_SCANCODE_P:
                        gameTimeline.togglePause();
                        SDL_Log(gameTimeline.isPaused() ? "PAUSED" : "RESUMED");
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

        double dt = gameTimeline.tick();
        if (dt > 0.05) dt = 0.05;
        
        serverTimer += dt;
        peerTimer += dt;
        cleanupTimer += dt;

        // Get moving platform from server (less frequent)
        if (serverTimer >= 1.0f / 30.0f) {
            peerManager.sendToServer("GET_STATE");
            std::string response = peerManager.receiveFromServer();
            
            std::istringstream respStream(response);
            std::string rline;
            while (std::getline(respStream, rline)) {
                if (rline.find("PLATFORM") == 0) {
                    std::istringstream platStream(rline);
                    std::string cmd;
                    float x, y, w, h;
                    int dir;
                    if (platStream >> cmd >> x >> y >> w >> h >> dir) {
                        prevPlatX = movingPlat.x;
                        movingPlat = {x, y, w, h};
                        platDir = dir;
                    }
                } else if (rline.find("PEER") == 0) {
                    // New peer discovered
                    std::istringstream peerStream(rline);
                    std::string cmd, peerId, peerEndpoint;
                    if (peerStream >> cmd >> peerId >> peerEndpoint && peerId != clientId) {
                        // Connect to new peer if not already connected
                        peerManager.connectToPeerNetwork(peerEndpoint);
                        SDL_Log("New peer discovered: %s", peerId.c_str());
                    }
                }
            }
            serverTimer = 0.0f;
        }

        // Broadcast player position to peers (P2P, more frequent)
        if (peerTimer >= 1.0f / 60.0f) {
            peerManager.updateMyPlayerData(px, py, gameTimeline.isPaused(), gameTimeline.scale());
            peerTimer = 0.0f;
        }

        // Cleanup stale peers
        if (cleanupTimer >= 1.0f) {
            peerManager.cleanupStalePeers();
            cleanupTimer = 0.0f;
        }

        // Input + Physics
        Input::poll();
        pbody.vx = 0.f;
        if (Input::isKeyPressed(SDL_SCANCODE_A)) pbody.vx = -PLAYER_SPEED;
        if (Input::isKeyPressed(SDL_SCANCODE_D)) pbody.vx = PLAYER_SPEED;
        if ((Input::isKeyPressed(SDL_SCANCODE_W) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) && wasGrounded) {
            pbody.vy = JUMP_VELOCITY;
        }

        float prevPX = px, prevPY = py;
        Physics::step(dt * 1000, px, py, pbody);
        clampPlayerPosition(px, py, playerW, playerH);

        SDL_FRect playerRect{px, py, playerW, playerH};
        grounded = false;

        // Static platforms
        SDL_FRect leftRect{leftX, leftY, STATIC_PLATFORM_W, STATIC_PLATFORM_H};
        SDL_FRect rightRect{rightX, rightY, STATIC_PLATFORM_W, STATIC_PLATFORM_H};
        if (checkCollision(playerRect, leftRect) && pbody.vy >= 0 && prevPY + playerH <= leftY + 20) {
            py = leftY - playerH; pbody.vy = 0; grounded = true;
        }
        if (checkCollision(playerRect, rightRect) && pbody.vy >= 0 && prevPY + playerH <= rightY + 20) {
            py = rightY - playerH; pbody.vy = 0; grounded = true;
        }

        // Moving platform (server-controlled)
        SDL_FRect mpRect{movingPlat.x, movingPlat.y, movingPlat.w, movingPlat.h};
        if (checkCollision(playerRect, mpRect)) {
            if (pbody.vy >= 0 && prevPY + playerH <= movingPlat.y + 20) {
                py = movingPlat.y - playerH;
                pbody.vy = 0;
                float platDX = movingPlat.x - prevPlatX;
                px += platDX;
                grounded = true;
            }
        }

        // Spikes
        float spikeW = 300.f, spikeH = 389.f, spikeY = DESIGN_HEIGHT - spikeH;
        for (float x = STATIC_PLATFORM_W; x < DESIGN_WIDTH - STATIC_PLATFORM_W; x += spikeW) {
            SDL_FRect spikeRect{x, spikeY, spikeW, spikeH};
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

        // Get peer player data (P2P)
        auto peerPlayers = peerManager.getPeerPlayerData();
        for (const auto& [peerId, data] : peerPlayers) {
            if (remotePlayerEntities.find(peerId) == remotePlayerEntities.end()) {
                remotePlayerEntities[peerId] = new Entity(renderer, "../assets/player.png",
                                                         0, 0, frameWidth, frameHeight, frameCount, 150);
            }
        }
        
        // Remove disconnected peers
        for (auto it = remotePlayerEntities.begin(); it != remotePlayerEntities.end();) {
            if (peerPlayers.find(it->first) == peerPlayers.end()) {
                delete it->second;
                it = remotePlayerEntities.erase(it);
            } else {
                ++it;
            }
        }

        // Rendering
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

        // Remote players (from P2P)
        for (const auto& [peerId, data] : peerPlayers) {
            if (remotePlayerEntities[peerId]) {
                remotePlayerEntities[peerId]->setPosition(data.x, data.y);
                remotePlayerEntities[peerId]->setSize(playerW, playerH);
                remotePlayerEntities[peerId]->render(renderer, window);
            }
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    // Cleanup
    for (auto& [id, entity] : remotePlayerEntities) {
        delete entity;
    }
    
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}