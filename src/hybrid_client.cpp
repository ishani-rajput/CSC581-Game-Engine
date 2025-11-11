#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <random>
#include <sstream>
#include <unordered_set>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <cmath>
#include "entity.h"
#include "input.h"
#include "scaling.h"
#include "physics.h"
#include "collision.h"
#include "timeline.h"
#include "peer_manager.h"
#include "event_manager.h"
#include "object_model.h"
#include "registry.h"

const int DESIGN_WIDTH = 1720;
const int DESIGN_HEIGHT = 1080;
const int WORLD_WIDTH = 3440;

const float PLAYER_SPEED = 300.f;
const float JUMP_VELOCITY = -750.f;

// Platform dimensions
const float leftWidth = 180.f;
const float leftHeight = 300.f;
const float middleWidth = 200.f;
const float middleHeight = 350.f;
const float platform3Width = 220.f;
const float platform3Height = 550.f;
const float bridgeWidth = 135.f;
const float bridgeHeight = 54.f;
const float finalWidth = 240.f;
const float finalHeight = 380.f;

// Platform positions in WORLD SPACE
const float leftX = 0.f;
const float leftY = DESIGN_HEIGHT - leftHeight - 70.f;
const float middleX = DESIGN_WIDTH / 2.f - middleWidth / 2.f - 200.f;
const float middleY = DESIGN_HEIGHT - middleHeight;
const float platform3X = DESIGN_WIDTH - platform3Width - 470.f;
const float platform3TopY = DESIGN_HEIGHT - platform3Height - 200.f;
const float platform3FullHeight = DESIGN_HEIGHT - platform3TopY;
const float bridge1X = platform3X + platform3Width + 70.f;
const float bridge1Y = platform3TopY + 100.f;
const float finalX = DESIGN_WIDTH - finalWidth;
const float finalY = DESIGN_HEIGHT - finalHeight;

struct RemotePlayerEntity {
    Entity* entity = nullptr;
};

static inline bool checkCollision(const SDL_FRect& a, const SDL_FRect& b) {
    return aabbIntersect(a, b);
}

static inline void clampPlayerPosition(float& playerX, float& playerY, float w, float h) {
    if (playerX < 0) playerX = 0;
    if (playerX + w > WORLD_WIDTH) playerX = WORLD_WIDTH - w;
    if (playerY > DESIGN_HEIGHT) playerY = DESIGN_HEIGHT - h;
}

static void renderSimpleText(SDL_Renderer* renderer, const std::string& text, float x, float y, 
                             uint8_t r = 255, uint8_t g = 255, uint8_t b = 255) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
    float textWidth = text.length() * 9.f;
    SDL_FRect bgRect = {x - 4, y - 2, textWidth, 16};
    SDL_RenderFillRect(renderer, &bgRect);
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    SDL_RenderDebugText(renderer, x, y, text.c_str());
}

static std::string generateClientId() {
    std::random_device rd; 
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    return "client_" + std::to_string(dis(gen));
}

// ===== SPIKE DETECTIVE: Death Analytics Structures =====
namespace Analytics {
struct DeathAnalytics {
    float speed = 0.f;
    float timeAlive = 0.f;
    std::string causeOfDeath;
    Engine::Vec2 deathLocation{0.f,0.f};
    std::vector<std::string> inputHistory;
    std::vector<Engine::Vec2> lastPositions;
    double deathTime = 0.0;
};
struct DeathMarker {
    Engine::Vec2 position{0.f,0.f};
    std::string playerId;
    std::string cause;
    float alpha = 1.0f;
    double timestamp = 0.0;
};
} // namespace Analytics
// ===== END SPIKE DETECTIVE STRUCTURES =====

int main(int, char**) {
    srand(time(nullptr));
    
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("Feeling Spikey - Hybrid P2P",
                                          DESIGN_WIDTH, DESIGN_HEIGHT, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    Scaling::setMode(ScaleMode::Proportional);

    Physics::setGravity(2000.f);
    Timeline gameTimeline; 
    gameTimeline.anchorToRealTime();

    // EVENT MANAGER SETUP
    Engine::EventManager eventManager(&gameTimeline);
    std::string statusMessage = "";
    int statusMessageTimer = 0;

    // Initialize PeerManager
    std::string clientId = generateClientId();
    PeerManager peerManager(clientId);

    // Generate random port for this peer's PUB socket
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> portDis(6000, 6999);
    int myPubPort = portDis(gen);
    std::string myPubEndpoint = "tcp://*:" + std::to_string(myPubPort);
    std::string myPubAddress  = "tcp://localhost:" + std::to_string(myPubPort);

    SDL_Log("[HYBRID P2P] Client ID: %s", clientId.c_str());
    SDL_Log("[HYBRID P2P] My PUB endpoint: %s", myPubAddress.c_str());

    // ===== SPIKE DETECTIVE: Analytics State =====
    Analytics::DeathAnalytics currentRun;
    std::vector<Analytics::DeathAnalytics> deathHistory;
    std::vector<Analytics::DeathMarker> networkDeathMarkers;
    std::mutex deathMarkersMutex;

    double spawnTime = 0.0;
    bool showingDeathAnalytics = false;
    int  analyticsDisplayTimer = 0;
    Analytics::DeathAnalytics lastDeath;

    std::vector<std::string> recentInputs;
    const int MAX_INPUT_HISTORY = 10;

    std::vector<Engine::Vec2> positionTrail;
    const int MAX_TRAIL_LENGTH = 180;
    // ===== END ANALYTICS STATE =====

    // Helper: publish serialized events / death markers to peers (P2P)
    auto sendEventP2P = [&](const Engine::Event& ev) {
        // Prefix "EV " → peers call raiseEventFromNetwork(rest)
        std::string msg = "EV " + ev.serialize();
        peerManager.publishToPeers(msg); // <-- ensure PeerManager exposes this
    };
    auto sendDeathMarkerP2P = [&](const Engine::Event& deathEv) {
        // Prefix "DM " → peers draw fading red X
        std::string msg = "DM " + deathEv.serialize();
        peerManager.publishToPeers(msg);
    };

    // REGISTER EVENT LISTENERS
    eventManager.registerListener(Engine::EventType::Collision, [&](const Engine::Event& e) {
        std::string objA = std::get<std::string>(e.payload.at("A"));
        std::string objB = std::get<std::string>(e.payload.at("B"));
        SDL_Log("[EVENT] Collision: %s <-> %s at time %.3f", 
                objA.c_str(), objB.c_str(), e.timestamp);
        statusMessage = "Collision: " + objA + " & " + objB;
        statusMessageTimer = 60;
    });

    eventManager.registerListener(Engine::EventType::Death, [&](const Engine::Event& e) {
        std::string who = std::get<std::string>(e.payload.at("entity"));
        SDL_Log("[EVENT] Death: %s at time %.3f", who.c_str(), e.timestamp);

        // Compute death analytics
        currentRun.timeAlive = gameTimeline.time() - spawnTime;
        // vx/vy recorded from latest physics step below (approx via trail if needed)
        // Here we just approximate from last two trail points:
        if (positionTrail.size() >= 2) {
            auto a = positionTrail[positionTrail.size()-2];
            auto b = positionTrail[positionTrail.size()-1];
            float dx = b.x - a.x, dy = b.y - a.y;
            currentRun.speed = std::sqrt(dx*dx + dy*dy) * 60.0f; // px/frame → px/s approx
            currentRun.deathLocation = b;
        }
        currentRun.inputHistory = recentInputs;
        currentRun.lastPositions = positionTrail;
        currentRun.deathTime = e.timestamp;

        // Cause of death (prefer payload["cause"] if present)
        if (e.payload.count("cause")) currentRun.causeOfDeath = std::get<std::string>(e.payload.at("cause"));
        else if (who.find("spike") != std::string::npos) currentRun.causeOfDeath = "Spikes";
        else if (who.find("fall")  != std::string::npos) currentRun.causeOfDeath = "Fell off map";
        else if (who.find("zone")  != std::string::npos) currentRun.causeOfDeath = "Death zone";
        else currentRun.causeOfDeath = "Unknown";

        deathHistory.push_back(currentRun);
        lastDeath = currentRun;
        showingDeathAnalytics = true;
        analyticsDisplayTimer = 300;

        SDL_Log("[SPIKE DETECTIVE] Death #%d - Cause: %s, Time Alive: %.1fs, Speed: %.1f", 
                (int)deathHistory.size(), currentRun.causeOfDeath.c_str(), 
                currentRun.timeAlive, currentRun.speed);

        // Auto stop + delayed replay
        if (eventManager.isRecording()) {
            eventManager.stopRecording();
            statusMessage = "Analyzing death... Replay in 2s";
            statusMessageTimer = 120;
            std::thread([&eventManager]() {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                eventManager.playReplay();
            }).detach();
        }
    });

    eventManager.registerListener(Engine::EventType::Spawn, [&](const Engine::Event& e) {
        std::string who = std::get<std::string>(e.payload.at("entity"));
        float x = std::get<float>(e.payload.at("x"));
        float y = std::get<float>(e.payload.at("y"));
        SDL_Log("[EVENT] Spawn: %s at (%.1f, %.1f) time %.3f", 
                who.c_str(), x, y, e.timestamp);
        statusMessage = "Spawned: " + who;
        statusMessageTimer = 60;

        // Reset analytics for new life (this peer only)
        if (who == clientId) {
            currentRun = Analytics::DeathAnalytics();
            currentRun.deathTime = 0;
            spawnTime = gameTimeline.time();
            recentInputs.clear();
            positionTrail.clear();
            showingDeathAnalytics = false;
        }
    });

    eventManager.registerListener(Engine::EventType::Input, [&](const Engine::Event& e) {
        std::string key = std::get<std::string>(e.payload.at("key"));
        bool pressed = std::get<bool>(e.payload.at("pressed"));
        SDL_Log("[EVENT] Input: %s %s at time %.3f", 
                key.c_str(), pressed ? "pressed" : "released", e.timestamp);

        if (pressed) {
            recentInputs.push_back(key);
            if ((int)recentInputs.size() > MAX_INPUT_HISTORY) {
                recentInputs.erase(recentInputs.begin());
            }
        }
    });

    eventManager.registerListener(Engine::EventType::ReplayStart, [&](const Engine::Event& e) {
        SDL_Log("[EVENT] Replay recording started at time %.3f", e.timestamp);
        statusMessage = "RECORDING STARTED";
        statusMessageTimer = 120;
    });

    eventManager.registerListener(Engine::EventType::ReplayStop, [&](const Engine::Event& e) {
        SDL_Log("[EVENT] Replay recording stopped at time %.3f", e.timestamp);
        statusMessage = "RECORDING STOPPED";
        statusMessageTimer = 120;
    });

    eventManager.registerListener(Engine::EventType::ReplayPlay, [&](const Engine::Event& e) {
        SDL_Log("[EVENT] Replay playback started at time %.3f", e.timestamp);
        statusMessage = "REPLAYING...";
        statusMessageTimer = 180;
    });

    SDL_Log("[EVENT SYSTEM] All listeners registered");

    // Setup hybrid networking
    peerManager.connectToServer("tcp://localhost:5556");  // Different port for hybrid server
    peerManager.startPeerListener(myPubEndpoint);         // start PUB/SUB for peers

    // Register with server
    std::string regMsg = "REGISTER_PEER " + clientId + " " + myPubAddress;
    peerManager.sendToServer(regMsg);

    // Get initial peer list
    std::string serverResponse = peerManager.receiveFromServer();
    std::unordered_set<std::string> connectedPeers;

    std::istringstream iss(serverResponse);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("PEER") == 0) {
            std::istringstream peerStream(line);
            std::string cmd, peerId, peerEndpoint;
            if (peerStream >> cmd >> peerId >> peerEndpoint && peerId != clientId) {
                SDL_Log("Connecting to peer: %s at %s", peerId.c_str(), peerEndpoint.c_str());
                peerManager.connectToPeerNetwork(peerEndpoint);
                connectedPeers.insert(peerId);
            }
        }
    }

    Entity groundBottom(renderer, "../assets/ground_bottom.png", 0, 0, 64, 64, 1, 0);
    Entity groundTop(renderer, "../assets/ground.png", 0, 0, 64, 64, 1, 0);
    Entity platformTex(renderer, "../assets/platform.png", 0, 0, 384, 128, 1, 0);
    Entity spikeTex(renderer, "../assets/spikes.png", 0, 0, 256, 256, 1, 0);
    Entity flagTex(renderer, "../assets/flag.png", 0, 0, 256, 256, 1, 0);

    const int frameCount = 8, frameWidth = 128, frameHeight = 128;
    const float playerScale = 1.1f;
    const float playerW = frameWidth * playerScale;
    const float playerH = frameHeight * playerScale;

    Entity localPlayer(renderer, "../assets/player.png", 0, 0, frameWidth, frameHeight, frameCount, 150);

    Engine::Registry gameObjectRegistry;
    std::mutex gameObjectMutex;

    // Create spawn points
    SDL_Log("[Req 4] Creating spawn points...");
    auto& spawn1 = gameObjectRegistry.upsert("spawn_left");
    spawn1.set<Engine::Vec2>("pos", {leftX + 80.f, leftY - playerH});
    spawn1.set<bool>("visible", false);

    auto& spawn2 = gameObjectRegistry.upsert("spawn_middle");
    spawn2.set<Engine::Vec2>("pos", {middleX + 80.f, middleY - playerH});
    spawn2.set<bool>("visible", false);

    auto& spawn3 = gameObjectRegistry.upsert("spawn_platform3");
    spawn3.set<Engine::Vec2>("pos", {platform3X + 80.f, platform3TopY - playerH});
    spawn3.set<bool>("visible", false);

    std::vector<std::string> spawnIds = {"spawn_left", "spawn_middle", "spawn_platform3"};
    std::string currentSpawnId = "spawn_left";

    // Create death zones
    SDL_Log("[Req 5] Creating death zones...");
    auto& dz1 = gameObjectRegistry.upsert("death_zone_gap1");
    dz1.set<Engine::Vec2>("pos", {leftX + leftWidth, DESIGN_HEIGHT - 280});
    dz1.set<Engine::Vec2>("size", {middleX - (leftX + leftWidth), 280});

    auto& dz2 = gameObjectRegistry.upsert("death_zone_gap2");
    dz2.set<Engine::Vec2>("pos", {middleX + middleWidth, DESIGN_HEIGHT - 280});
    dz2.set<Engine::Vec2>("size", {platform3X - (middleX + middleWidth), 280});

    auto& dz3 = gameObjectRegistry.upsert("death_zone_gap3");
    dz3.set<Engine::Vec2>("pos", {platform3X + platform3Width, DESIGN_HEIGHT - 280});
    dz3.set<Engine::Vec2>("size", {finalX - (platform3X + platform3Width), 280});

    auto& dz4 = gameObjectRegistry.upsert("death_zone_fall");
    dz4.set<Engine::Vec2>("pos", {0, DESIGN_HEIGHT});
    dz4.set<Engine::Vec2>("size", {WORLD_WIDTH, 200});

    std::vector<std::string> deathZoneIds = {"death_zone_gap1", "death_zone_gap2", "death_zone_gap3", "death_zone_fall"};

    float cameraX = 0.f;

    std::unordered_map<std::string, RemotePlayerEntity> remoteEntities;
    std::mutex remotePlayersMutex;

    // Server-controlled moving platforms (from hybrid server)
    SDL_FRect horizontalPlatform = {leftX + leftWidth + 20.f, DESIGN_HEIGHT - middleHeight - 150.f, 144.f, 72.f};
    int horizontalDir = 1;
    SDL_FRect verticalPlatform = {middleX + middleWidth + 40.f, middleY - 180.f, 144.f, 68.f};
    int verticalDir = 1;
    bool bridge1Visible = true;

    float px = leftX + 80.f;
    float py = leftY - playerH;
    Body playerBody = {0.f, 0.f, true};
    bool running = true;
    bool gameWon = false;
    bool grounded = false;
    bool wasGrounded = false;

    float prevHorizX = horizontalPlatform.x;
    float prevVertY = verticalPlatform.y;
    
    float serverTimer = 0.0f;
    float peerTimer = 0.0f;
    float cleanupTimer = 0.0f;

    // Generate initial spawn event for THIS peer
    auto spawnEv = Engine::Events::Spawn(clientId, px, py, &gameTimeline, 1);
    eventManager.raiseEvent(spawnEv);
    spawnTime = gameTimeline.time();

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
                        statusMessage = gameTimeline.isPaused() ? "PAUSED" : "RESUMED";
                        statusMessageTimer = 120;
                        break;
                    case SDL_SCANCODE_1:
                        gameTimeline.setScale(0.5);
                        statusMessage = "Speed: 0.5x";
                        statusMessageTimer = 120;
                        break;
                    case SDL_SCANCODE_2:
                        gameTimeline.setScale(1.0);
                        statusMessage = "Speed: 1.0x";
                        statusMessageTimer = 120;
                        break;
                    case SDL_SCANCODE_3:
                        gameTimeline.setScale(2.0);
                        statusMessage = "Speed: 2.0x";
                        statusMessageTimer = 120;
                        break;
                    case SDL_SCANCODE_R:
                        if (gameWon) {
                            gameWon = false;
                            currentSpawnId = "spawn_left";
                            auto respawnEv = Engine::Events::Spawn(clientId, leftX + 80.f, leftY - playerH, &gameTimeline, 1);
                            eventManager.raiseEvent(respawnEv);
                            px = leftX + 80.f;
                            py = leftY - playerH;
                            playerBody.vx = playerBody.vy = 0.f;
                            statusMessage = "Restarted";
                            statusMessageTimer = 120;
                        }
                        break;
                    case SDL_SCANCODE_S:
                        if (Scaling::mode() == ScaleMode::Pixel) {
                            Scaling::setMode(ScaleMode::Proportional);
                            statusMessage = "Proportional Scaling";
                        } else {
                            Scaling::setMode(ScaleMode::Pixel);
                            statusMessage = "Pixel Scaling";
                        }
                        statusMessageTimer = 120;
                        break;
                    case SDL_SCANCODE_F1:
                        eventManager.startRecording();
                        eventManager.raiseEvent(Engine::Events::ReplayStart(&gameTimeline));
                        break;
                    case SDL_SCANCODE_F2:
                        eventManager.stopRecording();
                        eventManager.raiseEvent(Engine::Events::ReplayStop(&gameTimeline));
                        break;
                    case SDL_SCANCODE_F3:
                        eventManager.playReplay();
                        eventManager.raiseEvent(Engine::Events::ReplayPlay(&gameTimeline));
                        break;
                    // Optional: toggle analytics overlay manually
                    case SDL_SCANCODE_F4:
                        if (!showingDeathAnalytics && !deathHistory.empty()) {
                            showingDeathAnalytics = true;
                            analyticsDisplayTimer = 300;
                            lastDeath = deathHistory.back();
                            statusMessage = "Showing last death analysis";
                            statusMessageTimer = 60;
                        } else {
                            showingDeathAnalytics = false;
                            statusMessage = "Analytics hidden";
                            statusMessageTimer = 60;
                        }
                        break;
                }
            }
        }

        double dt = gameTimeline.tick();
        if (dt > 0.05) dt = 0.05;
        Input::poll();

        serverTimer += dt;
        peerTimer += dt;
        cleanupTimer += dt;

        // Dispatch events every frame
        eventManager.dispatchEvents();

        // Get server-controlled platforms (less frequent)
        if (serverTimer >= 1.0f / 30.0f) {
            peerManager.sendToServer("GET_STATE");
            std::string response = peerManager.receiveFromServer();

            std::istringstream respStream(response);
            std::string rline;
            while (std::getline(respStream, rline)) {
                if (rline.find("HP") == 0) {
                    std::istringstream hpStream(rline);
                    std::string cmd;
                    float x, y, w, h;
                    int dir;
                    if (hpStream >> cmd >> x >> y >> w >> h >> dir) {
                        prevHorizX = horizontalPlatform.x;
                        horizontalPlatform = {x, y, w, h};
                        horizontalDir = dir;
                    }
                }
                else if (rline.find("VP") == 0) {
                    std::istringstream vpStream(rline);
                    std::string cmd;
                    float x, y, w, h;
                    int dir;
                    if (vpStream >> cmd >> x >> y >> w >> h >> dir) {
                        prevVertY = verticalPlatform.y;
                        verticalPlatform = {x, y, w, h};
                        verticalDir = dir;
                    }
                }
                else if (rline.find("B1") == 0) {
                    std::istringstream b1Stream(rline);
                    std::string cmd;
                    int visible;
                    if (b1Stream >> cmd >> visible) {
                        bridge1Visible = (visible != 0);
                    }
                }
                else if (rline.find("PEER") == 0) {
                    std::istringstream peerStream(rline);
                    std::string cmd, peerId, peerEndpoint;
                    if (peerStream >> cmd >> peerId >> peerEndpoint && peerId != clientId) {
                        if (connectedPeers.find(peerId) == connectedPeers.end()) {
                            peerManager.connectToPeerNetwork(peerEndpoint);
                            connectedPeers.insert(peerId);
                            SDL_Log("New peer discovered: %s", peerId.c_str());
                        }
                    }
                }
                else if (rline.find("EVENT") == 0) {
                    std::string eventData = rline.substr(6);
                    eventManager.raiseEventFromNetwork(eventData);
                }
            }
            serverTimer = 0.0f;
        }

        // P2P: handle messages from peers (events + death markers)
        {
            auto peerMsgs = peerManager.drainPeerMessages(); // <-- ensure PeerManager exposes this
            for (const auto& msg : peerMsgs) {
                if (msg.rfind("EV ", 0) == 0) {
                    // peer serialized event
                    std::string data = msg.substr(3);
                    eventManager.raiseEventFromNetwork(data);
                } else if (msg.rfind("DM ", 0) == 0 || msg.rfind("DEATH_MARKER ", 0) == 0) {
                    // peer death marker
                    std::string data = (msg.rfind("DM ",0)==0) ? msg.substr(3) : msg.substr(13);
                    try {
                        Engine::Event deathEv = Engine::Event::deserialize(data);
                        // Build/fade marker if not from self
                        std::string deadPlayer = std::get<std::string>(deathEv.payload.at("entity"));
                        if (deadPlayer != clientId) {
                            float mx = std::get<float>(deathEv.payload.at("x"));
                            float my = std::get<float>(deathEv.payload.at("y"));
                            std::string cause = std::get<std::string>(deathEv.payload.at("cause"));
                            std::lock_guard<std::mutex> lock(deathMarkersMutex);
                            Analytics::DeathMarker marker;
                            marker.position = {mx, my};
                            marker.playerId = deadPlayer;
                            marker.cause = cause;
                            marker.alpha = 1.0f;
                            marker.timestamp = gameTimeline.time();
                            networkDeathMarkers.push_back(marker);
                            SDL_Log("[P2P MARKER] %s died at (%.1f, %.1f) - %s", 
                                    deadPlayer.c_str(), mx, my, cause.c_str());
                        }
                    } catch (...) {}
                }
            }
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

        float prevPX = px, prevPY = py;
        playerBody.vx = 0.f;
        wasGrounded = grounded;

        // Generate input events
        if (Input::isKeyPressed(SDL_SCANCODE_A)) {
            auto inputEv = Engine::Events::Input("A", true, &gameTimeline, 4);
            eventManager.raiseEvent(inputEv);
            playerBody.vx = -PLAYER_SPEED;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_D)) {
            auto inputEv = Engine::Events::Input("D", true, &gameTimeline, 4);
            eventManager.raiseEvent(inputEv);
            playerBody.vx = PLAYER_SPEED;
        }
        if ((Input::isKeyPressed(SDL_SCANCODE_W) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) && wasGrounded) {
            auto inputEv = Engine::Events::Input("JUMP", true, &gameTimeline, 4);
            eventManager.raiseEvent(inputEv);
            playerBody.vy = JUMP_VELOCITY;
        }

        Physics::step(dt * 1000, px, py, playerBody);
        clampPlayerPosition(px, py, playerW, playerH);

        // ===== SPIKE DETECTIVE: Track Position Trail =====
        positionTrail.push_back({px, py});
        if ((int)positionTrail.size() > MAX_TRAIL_LENGTH) {
            positionTrail.erase(positionTrail.begin());
        }
        // fade death markers
        {
            std::lock_guard<std::mutex> lock(deathMarkersMutex);
            for (auto it = networkDeathMarkers.begin(); it != networkDeathMarkers.end(); ) {
                double age = gameTimeline.time() - it->timestamp;
                it->alpha = 1.0f - (float)(age / 10.0);
                if (age > 10.0) it = networkDeathMarkers.erase(it);
                else ++it;
            }
        }
        // ===== END POSITION TRAIL TRACKING =====

        SDL_FRect playerRect {px, py, playerW, playerH};
        grounded = false;

        SDL_FRect leftRect {leftX, leftY, leftWidth, leftHeight};
        SDL_FRect middleRect {middleX, middleY, middleWidth, middleHeight};
        SDL_FRect plat3Rect {platform3X, platform3TopY, platform3Width, platform3FullHeight};
        SDL_FRect bridge1Rect {bridge1X, bridge1Y, bridgeWidth, bridgeHeight};
        SDL_FRect finalRect {finalX, finalY, finalWidth, finalHeight};

        if (checkCollision(playerRect, leftRect) && playerBody.vy >= 0 && prevPY + playerH <= leftY + 20) {
            py = leftY - playerH; playerBody.vy = 0; grounded = true;
        }
        if (checkCollision(playerRect, middleRect) && playerBody.vy >= 0 && prevPY + playerH <= middleY + 20) {
            py = middleY - playerH; playerBody.vy = 0; grounded = true;
        }
        if (checkCollision(playerRect, plat3Rect) && playerBody.vy >= 0 && prevPY + playerH <= platform3TopY + 20) {
            py = platform3TopY - playerH; playerBody.vy = 0; grounded = true;
        }
        if (bridge1Visible && checkCollision(playerRect, bridge1Rect) && playerBody.vy >= 0 && prevPY + playerH <= bridge1Y + 20) {
            py = bridge1Y - playerH; playerBody.vy = 0; grounded = true;
        }
        if (checkCollision(playerRect, finalRect) && playerBody.vy >= 0 && prevPY + playerH <= finalY + 20) {
            py = finalY - playerH; playerBody.vy = 0; grounded = true;
            if (px > finalX + 50.f) {
                gameWon = true;
                statusMessage = "YOU WIN!";
                statusMessageTimer = 300;
            }
        }

        // Server-controlled moving platforms
        // --- Horizontal moving platform (corrected) ---
SDL_FRect mpRect {horizontalPlatform.x, horizontalPlatform.y, horizontalPlatform.w, horizontalPlatform.h};
if (checkCollision(playerRect, mpRect)) {
    if (playerBody.vy >= 0 && prevPY + playerH <= horizontalPlatform.y + 20) {
        // Landed on the moving platform
        py = horizontalPlatform.y - playerH;
        playerBody.vy = 0;
        grounded = true;

        // Compute server-based displacement (platform movement since last frame)
        float platDX = horizontalPlatform.x - prevHorizX;

        // Move player along with platform ONLY while grounded
        if (std::fabs(platDX) > 0.01f) {
            px += platDX;
        }
    }
}

        SDL_FRect vpRect {verticalPlatform.x, verticalPlatform.y, verticalPlatform.w, verticalPlatform.h};
        if (checkCollision(playerRect, vpRect)) {
            if (playerBody.vy >= 0 && prevPY + playerH <= verticalPlatform.y + 20) {
                py = verticalPlatform.y - playerH;
                playerBody.vy = 0;
                float platDY = verticalPlatform.y - prevVertY;
                py += platDY;
                grounded = true;
            }
        }

        prevHorizX = horizontalPlatform.x;

        // Check spawn points (checkpoints)
        playerRect = {px, py, playerW, playerH};
        {
            std::lock_guard<std::mutex> lock(gameObjectMutex);
            for (const auto& spawnId : spawnIds) {
                const auto* spawn = gameObjectRegistry.get(spawnId);
                if (!spawn) continue;
                auto spawnPos = spawn->get<Engine::Vec2>("pos", {0, 0});
                SDL_FRect checkpointRect = {spawnPos.x - 50.f, spawnPos.y - 50.f, 100.f, 100.f};
                if (checkCollision(playerRect, checkpointRect)) {
                    if (currentSpawnId != spawnId) {
                        currentSpawnId = spawnId;
                        statusMessage = "Checkpoint: " + spawnId;
                        statusMessageTimer = 90;
                    }
                }
            }
        }

        // ===== SPIKE DETECTIVE: Spike collisions + Death events + P2P markers =====
        auto handleDeath = [&](const char* causeTag, const char* collideTag = nullptr) {
            if (collideTag) {
                auto collEv = Engine::Events::Collision(clientId, collideTag, &gameTimeline, 1);
                eventManager.raiseEvent(collEv);
                sendEventP2P(collEv);
            }
            auto deathEv = Engine::Events::Death(clientId, &gameTimeline, 1);
            deathEv.payload["x"] = px;
            deathEv.payload["y"] = py;
            deathEv.payload["cause"] = std::string(causeTag);
            eventManager.raiseEvent(deathEv);
            sendEventP2P(deathEv);
            sendDeathMarkerP2P(deathEv);

            // Respawn to checkpoint
            const auto* spawn = gameObjectRegistry.get(currentSpawnId);
            if (spawn) {
                auto spawnPos = spawn->get<Engine::Vec2>("pos", {leftX + 80.f, leftY - playerH});
                px = spawnPos.x;
                py = spawnPos.y;
            }
            playerBody.vx = playerBody.vy = 0.f;
        };

        for (float x = leftX + leftWidth; x < middleX; x += 220.f) {
            SDL_FRect spikeRect{ x, DESIGN_HEIGHT - 280.f, 220.f, 280.f };
            if (checkCollision(playerRect, spikeRect)) { handleDeath("Spikes", "spike_gap1"); break; }
        }
        for (float x = middleX + middleWidth; x < platform3X; x += 220.f) {
            SDL_FRect spikeRect{ x, DESIGN_HEIGHT - 280.f, 220.f, 280.f };
            if (checkCollision(playerRect, spikeRect)) { handleDeath("Spikes", "spike_gap2"); break; }
        }
        for (float x = platform3X + platform3Width; x < finalX; x += 220.f) {
            SDL_FRect spikeRect{ x, DESIGN_HEIGHT - 280.f, 220.f, 280.f };
            if (checkCollision(playerRect, spikeRect)) { handleDeath("Spikes", "spike_gap3"); break; }
        }

        if (py > DESIGN_HEIGHT + 100) {
            handleDeath("Fell off map", nullptr);
        }

        {
            std::lock_guard<std::mutex> lock(gameObjectMutex);
            for (const auto& id : deathZoneIds) {
                const auto* zone = gameObjectRegistry.get(id);
                if (!zone) continue;
                auto zonePos  = zone->get<Engine::Vec2>("pos", {0, 0});
                auto zoneSize = zone->get<Engine::Vec2>("size", {0, 0});
                SDL_FRect zoneRect = {zonePos.x, zonePos.y, zoneSize.x, zoneSize.y};
                if (checkCollision(playerRect, zoneRect)) {
                    handleDeath("Death zone", "death_zone");
                    break;
                }
            }
        }
        // ===== END SPIKE DETECTIVE COLLISIONS =====

        // Camera
        float targetCameraX = px - DESIGN_WIDTH / 2.f;
        if (targetCameraX < 0) targetCameraX = 0;
        if (targetCameraX > WORLD_WIDTH - DESIGN_WIDTH) targetCameraX = WORLD_WIDTH - DESIGN_WIDTH;
        cameraX += (targetCameraX - cameraX) * 0.1f;

        localPlayer.update();

        // Get peer player data (P2P)
        auto peerPlayers = peerManager.getPeerPlayerData();
        {
            std::lock_guard<std::mutex> lock(remotePlayersMutex);
            for (const auto &[peerId, data] : peerPlayers) {
                if (remoteEntities.find(peerId) == remoteEntities.end() || !remoteEntities[peerId].entity) {
                    remoteEntities[peerId].entity = new Entity(renderer, "../assets/player.png",
                                                              0, 0, frameWidth, frameHeight, frameCount, 150);
                }
            }
            for (auto it = remoteEntities.begin(); it != remoteEntities.end();) {
                if (peerPlayers.find(it->first) == peerPlayers.end()) {
                    delete it->second.entity;
                    it = remoteEntities.erase(it);
                } else ++it;
            }
        }

        // RENDERING
        SDL_SetRenderDrawColor(renderer, 135, 206, 235, 255);
        SDL_RenderClear(renderer);

        float spikeW = 220.f, spikeH = 280.f, spikeY = DESIGN_HEIGHT - spikeH;
        for (float x = leftX + leftWidth; x < middleX; x += spikeW) {
            spikeTex.setPosition(x - cameraX, spikeY);
            spikeTex.setSize(spikeW, spikeH);
            spikeTex.render(renderer, window);
        }
        for (float x = middleX + middleWidth; x < platform3X; x += spikeW) {
            spikeTex.setPosition(x - cameraX, spikeY);
            spikeTex.setSize(spikeW, spikeH);
            spikeTex.render(renderer, window);
        }
        for (float x = platform3X + platform3Width; x < finalX; x += spikeW) {
            spikeTex.setPosition(x - cameraX, spikeY);
            spikeTex.setSize(spikeW, spikeH);
            spikeTex.render(renderer, window);
        }

        groundBottom.setPosition(leftX - cameraX, leftY);
        groundBottom.setSize(leftWidth, leftHeight);
        groundBottom.render(renderer, window);
        groundTop.setPosition(leftX - cameraX, leftY - 48.f);
        groundTop.setSize(leftWidth, 48.f);
        groundTop.render(renderer, window);

        groundBottom.setPosition(middleX - cameraX, middleY);
        groundBottom.setSize(middleWidth, middleHeight);
        groundBottom.render(renderer, window);
        groundTop.setPosition(middleX - cameraX, middleY - 48.f);
        groundTop.setSize(middleWidth, 48.f);
        groundTop.render(renderer, window);

        groundBottom.setPosition(platform3X - cameraX, platform3TopY);
        groundBottom.setSize(platform3Width, platform3FullHeight);
        groundBottom.render(renderer, window);
        groundTop.setPosition(platform3X - cameraX, platform3TopY - 48.f);
        groundTop.setSize(platform3Width, 48.f);
        groundTop.render(renderer, window);

        if (bridge1Visible) {
            platformTex.setPosition(bridge1X - cameraX, bridge1Y);
            platformTex.setSize(bridgeWidth, bridgeHeight);
            platformTex.render(renderer, window);
        }

        groundBottom.setPosition(finalX - cameraX, finalY);
        groundBottom.setSize(finalWidth, finalHeight);
        groundBottom.render(renderer, window);
        groundTop.setPosition(finalX - cameraX, finalY - 48.f);
        groundTop.setSize(finalWidth, 48.f);
        groundTop.render(renderer, window);

        float flagW = 150.f, flagH = 150.f;
        flagTex.setPosition(finalX - cameraX + finalWidth - flagW - 30.f, finalY - flagH - 48.f);
        flagTex.setSize(flagW, flagH);
        flagTex.render(renderer, window);

        // Server-controlled platforms
        platformTex.setPosition(horizontalPlatform.x - cameraX, horizontalPlatform.y);
        platformTex.setSize(horizontalPlatform.w, horizontalPlatform.h);
        platformTex.render(renderer, window);

        platformTex.setPosition(verticalPlatform.x - cameraX, verticalPlatform.y);
        platformTex.setSize(verticalPlatform.w, verticalPlatform.h);
        platformTex.render(renderer, window);

        // ===== SPIKE DETECTIVE: Render Death Markers (from peers) =====
        {
            std::lock_guard<std::mutex> lock(deathMarkersMutex);
            for (const auto& marker : networkDeathMarkers) {
                float mx = marker.position.x - cameraX;
                float my = marker.position.y;

                SDL_SetRenderDrawColor(renderer, 255, 0, 0, (uint8_t)(marker.alpha * 255));
                for (int i = -1; i <= 1; ++i) {
                    SDL_RenderLine(renderer, mx - 15 + i, my - 15, mx + 15 + i, my + 15);
                    SDL_RenderLine(renderer, mx - 15 + i, my + 15, mx + 15 + i, my - 15);
                }
                if (marker.alpha > 0.5f) {
                    std::string warn = "WARNING: " + marker.cause;
                    renderSimpleText(renderer, warn, mx - 60, my - 35, 255, (uint8_t)(100 + marker.alpha * 155), 0);
                }
            }
        }
        // ===== SPIKE DETECTIVE: Replay Overlay =====
        if (eventManager.isReplaying()) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 50, 80);
            SDL_FRect overlay = {0, 0, (float)DESIGN_WIDTH, (float)DESIGN_HEIGHT};
            SDL_RenderFillRect(renderer, &overlay);

            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 220);
            SDL_FRect replayBanner = {DESIGN_WIDTH / 2 - 250, 30, 500, 80};
            SDL_RenderFillRect(renderer, &replayBanner);

            SDL_SetRenderDrawColor(renderer, 255, 255, 0, 255);
            for (int i = 0; i < 3; i++) {
                SDL_FRect border = {replayBanner.x - i, replayBanner.y - i, replayBanner.w + i*2, replayBanner.h + i*2};
                SDL_RenderRect(renderer, &border);
            }

            renderSimpleText(renderer, ">>> REPLAYING YOUR DEATH <<<", DESIGN_WIDTH / 2 - 140, 55, 255, 255, 0);
            renderSimpleText(renderer, "Watch the cyan trail closely!", DESIGN_WIDTH / 2 - 120, 80, 255, 255, 255);

            // thick cyan trail + dots + start/death markers
            if (!positionTrail.empty()) {
                for (int t = -5; t <= 5; ++t) {
                    SDL_SetRenderDrawColor(renderer, 0, 255, 255, 150);
                    for (size_t i = 1; i < positionTrail.size(); ++i) {
                        float x1 = positionTrail[i-1].x - cameraX;
                        float y1 = positionTrail[i-1].y + t;
                        float x2 = positionTrail[i].x - cameraX;
                        float y2 = positionTrail[i].y + t;
                        SDL_RenderLine(renderer, x1, y1, x2, y2);
                    }
                }
                SDL_SetRenderDrawColor(renderer, 255, 255, 0, 255);
                for (size_t i = 0; i < positionTrail.size(); i += 5) {
                    float x = positionTrail[i].x - cameraX;
                    float y = positionTrail[i].y;
                    SDL_FRect dot = {x - 5, y - 5, 10, 10};
                    SDL_RenderFillRect(renderer, &dot);
                }
                if (!positionTrail.empty()) {
                    float sx = positionTrail.front().x - cameraX;
                    float sy = positionTrail.front().y;
                    SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                    SDL_FRect sdot = {sx - 10, sy - 10, 20, 20};
                    SDL_RenderFillRect(renderer, &sdot);
                    renderSimpleText(renderer, "START", sx - 20, sy - 30, 0, 255, 0);
                }
                if (!positionTrail.empty()) {
                    float ex = positionTrail.back().x - cameraX;
                    float ey = positionTrail.back().y;
                    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
                    for (int i = -2; i <= 2; i++) {
                        SDL_RenderLine(renderer, ex - 15, ey - 15 + i, ex + 15, ey + 15 + i);
                        SDL_RenderLine(renderer, ex - 15, ey + 15 + i, ex + 15, ey - 15 + i);
                    }
                    renderSimpleText(renderer, "DEATH", ex - 20, ey - 35, 255, 0, 0);
                }
            }
        }

        localPlayer.setPosition(px - cameraX, py);
        localPlayer.setSize(playerW, playerH);
        localPlayer.render(renderer, window);

        // Remote players (from P2P)
        {
            std::lock_guard<std::mutex> lock(remotePlayersMutex);
            for (const auto &[peerId, data] : peerPlayers) {
                if (remoteEntities[peerId].entity) {
                    remoteEntities[peerId].entity->update();
                    remoteEntities[peerId].entity->setPosition(data.x - cameraX, data.y);
                    remoteEntities[peerId].entity->setSize(playerW, playerH);
                    remoteEntities[peerId].entity->render(renderer, window);
                }
            }
        }

        // ===== SPIKE DETECTIVE: Death Analytics Overlay =====
        if (showingDeathAnalytics && analyticsDisplayTimer > 0) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
            SDL_FRect analyticsBox = {DESIGN_WIDTH / 2 - 300, 200, 600, 400};
            SDL_RenderFillRect(renderer, &analyticsBox);
            SDL_SetRenderDrawColor(renderer, 255, 50, 50, 255);
            SDL_RenderRect(renderer, &analyticsBox);

            renderSimpleText(renderer, "=== DEATH ANALYSIS ===", DESIGN_WIDTH / 2 - 100, 220, 255, 50, 50);

            float y = 260, lh = 25;
            renderSimpleText(renderer, "Cause of Death: " + lastDeath.causeOfDeath, DESIGN_WIDTH / 2 - 280, y); y += lh;

            char buf[64];
            snprintf(buf, sizeof(buf), "Time Alive: %.1fs", lastDeath.timeAlive);
            renderSimpleText(renderer, buf, DESIGN_WIDTH / 2 - 280, y); y += lh;

            snprintf(buf, sizeof(buf), "Speed at Death: %.0f px/s", lastDeath.speed);
            renderSimpleText(renderer, buf, DESIGN_WIDTH / 2 - 280, y); y += lh;

            snprintf(buf, sizeof(buf), "Location: (%.0f, %.0f)", lastDeath.deathLocation.x, lastDeath.deathLocation.y);
            renderSimpleText(renderer, buf, DESIGN_WIDTH / 2 - 280, y); y += lh;

            snprintf(buf, sizeof(buf), "Total Deaths: %d", (int)deathHistory.size());
            renderSimpleText(renderer, buf, DESIGN_WIDTH / 2 - 280, y); y += lh + 10;

            renderSimpleText(renderer, "Last Inputs:", DESIGN_WIDTH / 2 - 280, y, 200, 200, 255); y += lh;
            int inputsToShow = std::min(5, (int)lastDeath.inputHistory.size());
            for (int i = inputsToShow - 1; i >= 0; --i) {
                char li[64];
                snprintf(li, sizeof(li), "  %d. %s", inputsToShow - i, lastDeath.inputHistory[lastDeath.inputHistory.size() - 1 - i].c_str());
                renderSimpleText(renderer, li, DESIGN_WIDTH / 2 - 260, y, 150, 200, 255);
                y += lh;
            }

            analyticsDisplayTimer--;
        }

        // UI
        renderSimpleText(renderer, "A/D: Move  W/Space: Jump", 20, 20, 255, 255, 255);
        renderSimpleText(renderer, "P: Pause  1/2/3: Speed  R: Restart  S: Scaling", 20, 40, 255, 255, 255);
        renderSimpleText(renderer, "F1: Record  F2: Stop  F3: Replay  F4: Analytics", 20, 60, 255, 200, 100);
        renderSimpleText(renderer, "[HYBRID P2P] Server=Platforms, P2P=Players+Events", 20, 80, 0, 255, 255);

        std::string cameraText = "Camera: " + std::to_string(int(cameraX));
        renderSimpleText(renderer, cameraText, DESIGN_WIDTH - 180, 20, 100, 200, 255);

        std::string speedText = "Speed: " + std::to_string(int(gameTimeline.scale() * 100)) + "%";
        renderSimpleText(renderer, speedText, DESIGN_WIDTH - 200, 40, 0, 255, 0);

        if (gameTimeline.isPaused()) {
            renderSimpleText(renderer, "PAUSED", DESIGN_WIDTH - 200, 60, 255, 0, 0);
        }

        int peerCount = (int)peerPlayers.size();
        std::string peerCountText = "Peers: " + std::to_string(peerCount);
        renderSimpleText(renderer, peerCountText, DESIGN_WIDTH - 250, 80, 255, 255, 0);

        std::string eventStatus = "Events: ";
        if (eventManager.isRecording()) eventStatus += "RECORDING";
        else if (eventManager.isReplaying()) eventStatus += "REPLAYING";
        else eventStatus += "Ready (" + std::to_string(eventManager.pendingCount()) + " pending)";
        renderSimpleText(renderer, eventStatus, DESIGN_WIDTH - 400, 100, 100, 255, 100);

        if (statusMessageTimer > 0) {
            renderSimpleText(renderer, statusMessage, DESIGN_WIDTH / 2 - 150, 150, 255, 255, 0);
            statusMessageTimer--;
        }

        if (gameWon) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
            SDL_FRect overlay = {0, 0, (float)DESIGN_WIDTH, (float)DESIGN_HEIGHT};
            SDL_RenderFillRect(renderer, &overlay);

            SDL_SetRenderDrawColor(renderer, 255, 215, 0, 255);
            SDL_FRect winBox = {DESIGN_WIDTH / 2 - 300, DESIGN_HEIGHT / 2 - 80, 600, 160};
            SDL_RenderFillRect(renderer, &winBox);

            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderDebugText(renderer, DESIGN_WIDTH / 2 - 80, DESIGN_HEIGHT / 2 - 40, "YOU WIN!");
            SDL_RenderDebugText(renderer, DESIGN_WIDTH / 2 - 120, DESIGN_HEIGHT / 2 + 20, "Press R to Restart");
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    // Cleanup
    {
        std::lock_guard<std::mutex> lock(remotePlayersMutex);
        for (auto& [id, entity] : remoteEntities) {
            if (entity.entity) delete entity.entity;
        }
    }
    
    gameObjectRegistry.clear();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    
    return 0;
}
