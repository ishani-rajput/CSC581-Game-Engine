#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include "input.h"
#include "physics.h"
#include "collision.h"
#include "scaling.h"
#include "timeline.h"
#include "peer_manager.h"

// ENGINE INTEGRATION
#include "net_strategy.h"
#include "object_model.h"
#include "registry.h"

#include <string>
#include <unordered_map>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include <chrono>

struct PipePair { 
    SDL_FRect top, bottom; 
    PipePair(float tx, float ty, float tw, float th, float bx, float by, float bw, float bh) 
        : top{tx, ty, tw, th}, bottom{bx, by, bw, bh} {} 
};

static inline int numericIdFrom(const std::string& s) {
    int n = 0; bool any = false;
    for(char c : s) if(isdigit((unsigned char)c)) { any = true; n = n * 10 + (c - '0'); }
    if(!any) n = 1 + rand() % 20;
    if(n < 1) n = 1; if(n > 20) n = 1 + (n % 20);
    return n;
}

static float floatRand(float a, float b){
    return a + (b-a) * (float)rand()/(float)RAND_MAX;
}

// TASK 6: Apply camera offset to world coordinates
static SDL_FRect applyCamera(const SDL_FRect& worldRect, float cameraX) {
    return {worldRect.x - cameraX, worldRect.y, worldRect.w, worldRect.h};
}

struct RemotePlayer { 
    SDL_FRect rect; 
    int currentFrame = 0;
    double animationAccum = 0.0;
    std::chrono::high_resolution_clock::time_point lastAnimTime = std::chrono::high_resolution_clock::now();
    bool paused = false;
    float scale = 1.0f;
};

static SDL_Texture* tryLoadTexture(SDL_Renderer* r, const char* const* paths, int n){
    for(int i=0;i<n;++i){ if(!paths[i]) continue; if(SDL_Texture* t=IMG_LoadTexture(r, paths[i])) return t; }
    SDL_Log("Failed to load texture: %s", SDL_GetError()); return nullptr;
}

// ENGINE REGISTRY
static Engine::Registry gRegistry;

// TASK 4: Initialize spawn points (hidden, non-rendered objects)
void initializeSpawnPoints() {
    // Spawn point 1 - Ground level, left side
    auto& spawn1 = gRegistry.upsert("spawn_point_1");
    spawn1.set<Engine::Vec2>("pos", {100.f, 840.f});
    spawn1.set<bool>("active", true);
    
    // Spawn point 2 - Ground level, center
    auto& spawn2 = gRegistry.upsert("spawn_point_2");
    spawn2.set<Engine::Vec2>("pos", {480.f, 840.f});
    spawn2.set<bool>("active", true);
    
    // Spawn point 3 - Ground level, right side
    auto& spawn3 = gRegistry.upsert("spawn_point_3");
    spawn3.set<Engine::Vec2>("pos", {1400.f, 840.f});
    spawn3.set<bool>("active", true);
    
    // Spawn point 4 - On skully platform
    auto& spawn4 = gRegistry.upsert("spawn_point_4");
    spawn4.set<Engine::Vec2>("pos", {870.f, 392.f}); // 500 - 108 (character height)
    spawn4.set<bool>("active", true);
    
    // Spawn point 5 - On grey platform (right)
    auto& spawn5 = gRegistry.upsert("spawn_point_5");
    spawn5.set<Engine::Vec2>("pos", {1620.f, 542.f}); // 650 - 108
    spawn5.set<bool>("active", true);
    
    // Spawn point 6 - Near moving platform area
    auto& spawn6 = gRegistry.upsert("spawn_point_6");
    spawn6.set<Engine::Vec2>("pos", {1100.f, 840.f});
    spawn6.set<bool>("active", true);
}

// TASK 4: Get spawn point position by ID
Engine::Vec2 getSpawnPointPosition(int spawnId) {
    // Clamp spawn ID to valid range [1, 6]
    if (spawnId < 1) spawnId = 1;
    if (spawnId > 6) spawnId = ((spawnId - 1) % 6) + 1;
    
    std::string spawnName = "spawn_point_" + std::to_string(spawnId);
    auto* spawn = gRegistry.get(spawnName);
    if (spawn) {
        return spawn->get<Engine::Vec2>("pos", {480.f, 840.f});
    }
    return {480.f, 840.f}; // Default position if spawn point not found
}

// TASK 5: Respawn player at a random spawn point
void respawnPlayer(SDL_FRect& skully, Body& skBody) {
    // Choose a random spawn point (1-6)
    int randomSpawn = 1 + (rand() % 6);
    Engine::Vec2 spawnPos = getSpawnPointPosition(randomSpawn);
    
    // Reset player position
    skully.x = spawnPos.x;
    skully.y = spawnPos.y;
    
    // Reset velocities
    skBody.vx = 0.f;
    skBody.vy = 0.f;
    
    SDL_Log("Player respawned at spawn point %d (%.1f, %.1f)", randomSpawn, spawnPos.x, spawnPos.y);
}

// TASK 5: Initialize death zones (hidden, non-rendered boundary objects)
void initializeDeathZones() {
    // Death zone - Left boundary (off screen left)
    auto& dz = gRegistry.upsert("death_zone_left");
    dz.set<Engine::Vec2>("pos", {-200.f, 0.f});
    dz.set<Engine::Vec2>("size", {200.f, 1080.f});
    dz.set<bool>("active", true);
}

// TASK 5: Check if player is in any death zone
bool checkDeathZones(const SDL_FRect& player) {
    // Check left boundary death zone
    auto* dz = gRegistry.get("death_zone_left");
    if (dz && dz->get<bool>("active", true)) {
        Engine::Vec2 pos = dz->get<Engine::Vec2>("pos", {0.f, 0.f});
        Engine::Vec2 size = dz->get<Engine::Vec2>("size", {0.f, 0.f});
        SDL_FRect dzRect = {pos.x, pos.y, size.x, size.y};
        if (aabbIntersect(player, dzRect)) {
            SDL_Log("Player entered death zone (left boundary)");
            return true;
        }
    }
    
    return false;
}

int main(int argc, char** argv){
    if (argc < 2) {
        std::cerr << "Usage: ./client <id>\n";
        return 1;
    }
    const std::string CLIENT_ID = argv[1];
    
    // TASK 4: Initialize spawn points in the object model
    initializeSpawnPoints();
    
    // TASK 5: Initialize death zones in the object model
    initializeDeathZones();

    // Initialize random seed for spawn point selection
    srand(static_cast<unsigned int>(time(nullptr)));

    // NETWORK STRATEGY
    Engine::NetStrategy strat = Engine::NetStrategy::FullState;
    if (const char* s = std::getenv("NET_STRATEGY")) 
        if (std::string(s) == "input") strat = Engine::NetStrategy::InputDelta;

    PeerManager peerManager(CLIENT_ID);
    peerManager.connectToServer("tcp://127.0.0.1:5555");
    peerManager.sendToServer("CONNECT");
    std::string handshakeResponse = peerManager.receiveFromServer();
    if (handshakeResponse.find("CONNECTED") == std::string::npos) {
        std::cerr << "Failed to connect to server" << std::endl;
        return 1;
    }
    const int myClientNum = numericIdFrom(CLIENT_ID);
    const int myPubPort = 7000 + myClientNum;
    {
        std::ostringstream oss; 
        oss << "tcp://*:" << myPubPort; 
        peerManager.startPeerListener(oss.str());
    }
    for(int i = 1; i <= 20; i++) {
        if(i == myClientNum) continue;
        std::ostringstream ep; ep << "tcp://localhost:" << (7000 + i);
        peerManager.connectToPeerNetwork(ep.str());
    }

    if(!SDL_Init(SDL_INIT_VIDEO)){ SDL_Log("SDL init failed: %s", SDL_GetError()); return 1; }
    SDL_Window* window=nullptr; SDL_Renderer* renderer=nullptr;
    if(!SDL_CreateWindowAndRenderer(("Client - " + CLIENT_ID).c_str(),1500,900,SDL_WINDOW_RESIZABLE,&window,&renderer)){
        SDL_Log("CreateWindowAndRenderer failed: %s", SDL_GetError()); SDL_Quit(); return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    const char* skullPaths[] = {"../assets/skullFace.png","assets/skullFace.png"};
    SDL_Texture* skullTex = tryLoadTexture(renderer, skullPaths, 2);
    if(!skullTex){ SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    const char* brickPaths[] = {"../assets/platform.png","assets/platform.png"};
    SDL_Texture* brickTex = tryLoadTexture(renderer, brickPaths, 2);
    
    const char* skullyPlatformPaths[] = {"../assets/skullyPlatform.png", "assets/skullyPlatform.png"};
    SDL_Texture* skullyPlatformTex = tryLoadTexture(renderer, skullyPlatformPaths, 2);
    
    SDL_SetTextureScaleMode(skullTex, SDL_SCALEMODE_NEAREST);
    if(brickTex) SDL_SetTextureScaleMode(brickTex, SDL_SCALEMODE_NEAREST);
    if(skullyPlatformTex) SDL_SetTextureScaleMode(skullyPlatformTex, SDL_SCALEMODE_NEAREST);

    float texW=0, texH=0; SDL_GetTextureSize(skullTex,&texW,&texH);
    const int FRAME_COUNT=6; const float FRAME_W=texW/FRAME_COUNT, FRAME_H=texH;
    int currentFrame=0; double animationAccum=0.0; const double ANIM_FRAME_SEC=0.100;

    float characterSize=108.f;
    
    // TASK 4: Use spawn points to determine initial player position
    // Each client spawns at a different spawn point based on their ID
    Engine::Vec2 spawnPos = getSpawnPointPosition(myClientNum);
    SDL_FRect skully={spawnPos.x, spawnPos.y, characterSize, characterSize};
    
    Body skBody; skBody.affectedByGravity=true; Physics::setGravity(2400.f);
    const float JUMP_VELOCITY=-900.f;
    const float MOVE_SPEED = 400.f;
    SDL_FRect ground={0.f,1080.f-120.f,1920.f,120.f};
    
    // Skully platform - floating platform with texture
    SDL_FRect skullyPlatform={800.f, 500.f, 250.f, 50.f};
    
    // Grey platform - extreme right
    SDL_FRect greyPlatform={1550.f, 650.f, 300.f, 40.f};
    
    // Red moving platform (Task 3 - vertical movement)
    SDL_FRect movingPlatform={1200.f, 550.f, 180.f, 35.f};
    const float MOVING_PLAT_SPEED = 120.f;
    float movingPlatDir = 1.f;  // 1 = down, -1 = up
    const float MOVING_PLAT_MIN_Y = 400.f, MOVING_PLAT_MAX_Y = 700.f;

    std::vector<PipePair> pipes;
    const float PIPE_W = 140.f;
    const float PIPE_GAP = 280.f;
    const float SCREEN_WIDTH = 1920.f;
    const float SCREEN_HEIGHT = 1080.f;
    const float PIPE_SPEED = -450.f;
    const double PIPE_SPAWN_EVERY = 1.4;
    double localSpawnTimer = 0.0;
    int nextPipeId = 0;

    Scaling::setMode(ScaleMode::Pixel);
    Timeline gameTime; 
    gameTime.anchorToRealTime(); 
    gameTime.setScale(1.0);

    std::unordered_map<std::string, RemotePlayer> others;
    
    // TASK 6: Camera tracking for side-scrolling
    float cameraX = 0.f;

    bool running=true; SDL_Event ev;
    bool prevSpace=false, prevToggle=false;
    bool prevP=false, prev1=false, prev2=false, prev3=false;

    while(running){
        while(SDL_PollEvent(&ev)){ if(ev.type==SDL_EVENT_QUIT) running=false; }
        Input::poll();
        if(Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) running=false;

        bool toggleNow=Input::isKeyPressed(SDL_SCANCODE_T);
        if(toggleNow && !prevToggle){
            Scaling::setMode(Scaling::mode()==ScaleMode::Pixel? ScaleMode::Proportional: ScaleMode::Pixel);
        }
        prevToggle=toggleNow;

        bool pauseNow = Input::isKeyPressed(SDL_SCANCODE_P);
        if (pauseNow && !prevP) { gameTime.togglePause(); }
        prevP = pauseNow;

        bool onePress = Input::isKeyPressed(SDL_SCANCODE_1);
        bool twoPress = Input::isKeyPressed(SDL_SCANCODE_2);
        bool threePress = Input::isKeyPressed(SDL_SCANCODE_3);

        if (onePress && !prev1) { gameTime.setScale(0.5); }
        if (twoPress && !prev2) { gameTime.setScale(1.0); }
        if (threePress && !prev3) { gameTime.setScale(2.0); }
        prev1=onePress; prev2=twoPress; prev3=threePress;

        double deltaSec=gameTime.tick();

        bool spaceNow=Input::isKeyPressed(SDL_SCANCODE_SPACE);
        if(spaceNow && !prevSpace) skBody.vy=JUMP_VELOCITY;
        prevSpace=spaceNow;

        // Horizontal movement
        bool moveLeft = Input::isKeyPressed(SDL_SCANCODE_A);
        bool moveRight = Input::isKeyPressed(SDL_SCANCODE_D);
        if (moveLeft && !moveRight) skBody.vx = -MOVE_SPEED;
        else if (moveRight && !moveLeft) skBody.vx = MOVE_SPEED;
        else skBody.vx = 0.f;

        if (!gameTime.isPaused()) {
            Physics::step(static_cast<float>(deltaSec*1000.0), skully.x, skully.y, skBody);
            
            // TASK 6: Camera follows player (side-scrolling)
            // Camera starts following when player moves beyond 30% of screen width
            const float CAMERA_FOLLOW_THRESHOLD = 1920.f * 0.3f;
            const float CAMERA_BACK_THRESHOLD = 1920.f * 0.2f;
            
            // Move camera right when player moves right beyond threshold
            if (skully.x - cameraX > CAMERA_FOLLOW_THRESHOLD) {
                cameraX = skully.x - CAMERA_FOLLOW_THRESHOLD;
            }
            // Move camera left when player moves left beyond back threshold
            if (skully.x - cameraX < CAMERA_BACK_THRESHOLD && cameraX > 0.f) {
                cameraX = skully.x - CAMERA_BACK_THRESHOLD;
                if (cameraX < 0.f) cameraX = 0.f;
            }
            
            // No hard screen edge limits - world can be infinite
            
            // Moving platform - vertical movement (Task 3)
            movingPlatform.y += MOVING_PLAT_SPEED * movingPlatDir * deltaSec;
            if (movingPlatform.y <= MOVING_PLAT_MIN_Y) {
                movingPlatform.y = MOVING_PLAT_MIN_Y;
                movingPlatDir = 1.f;
            } else if (movingPlatform.y >= MOVING_PLAT_MAX_Y) {
                movingPlatform.y = MOVING_PLAT_MAX_Y;
                movingPlatDir = -1.f;
            }
            
            // Platform collisions
            if(skully.y+skully.h>=ground.y){ skully.y=ground.y-skully.h; skBody.vy=0.f; }
            if(aabbIntersect(skully, skullyPlatform) && skBody.vy > 0.f){ skully.y=skullyPlatform.y-skully.h; skBody.vy=0.f; }
            if(aabbIntersect(skully, greyPlatform) && skBody.vy > 0.f){ skully.y=greyPlatform.y-skully.h; skBody.vy=0.f; }
            if(aabbIntersect(skully, movingPlatform) && skBody.vy > 0.f){ skully.y=movingPlatform.y-skully.h; skBody.vy=0.f; }
            if(skully.y<0.f){ skully.y=0.f; skBody.vy=0.f; }

            // TASK 5: Death zone collision - respawn if player enters a death zone
            if (checkDeathZones(skully)) {
                respawnPlayer(skully, skBody);
            }

            // Pipe collisions - respawn on hit
            bool hit = false;
            for(auto& p : pipes) {
                if(aabbIntersect(skully, p.top) || aabbIntersect(skully, p.bottom)) {
                    hit = true; break;
                }
            }
            if (hit) { 
                pipes.clear(); 
                localSpawnTimer = 0.0;
                nextPipeId = 0;
                respawnPlayer(skully, skBody);  // TASK 5: Respawn at a random spawn point
            }
        }

        animationAccum += deltaSec;
        while(animationAccum >= ANIM_FRAME_SEC) {
            animationAccum -= ANIM_FRAME_SEC;
            currentFrame = (currentFrame + 1) % FRAME_COUNT;
        }

        // TASK 6: Pipe movement and synchronized spawning
        if (!gameTime.isPaused()) {
            // Move pipes toward player
            for(auto& p : pipes) {
                p.top.x += PIPE_SPEED * deltaSec;
                p.bottom.x += PIPE_SPEED * deltaSec;
            }
            
            // Spawn pipes with synchronized heights using deterministic random
            localSpawnTimer += deltaSec;
            while(localSpawnTimer >= PIPE_SPAWN_EVERY) {
                localSpawnTimer -= PIPE_SPAWN_EVERY;
                
                // Use deterministic pseudo-random based on pipe ID for sync across clients
                // This ensures all clients see the same pattern
                unsigned int seed = 12345 + nextPipeId * 7919;  // Prime multiplier for better distribution
                float t = (float)((seed ^ (seed >> 16)) & 0xFFFF) / 65535.0f;  // Normalize to 0-1
                
                float minCenter = SCREEN_HEIGHT * 0.30f;
                float maxCenter = SCREEN_HEIGHT * 0.70f;
                float center = minCenter + t * (maxCenter - minCenter);
                float topH = center - PIPE_GAP * 0.5f;
                float bottomY = center + PIPE_GAP * 0.5f;
                
                // Spawn pipes at right edge relative to camera
                float spawnX = cameraX + SCREEN_WIDTH + PIPE_W;
                pipes.emplace_back(
                    spawnX, 0.f, PIPE_W, topH,
                    spawnX, bottomY, PIPE_W, SCREEN_HEIGHT - bottomY - 120.f
                );
                nextPipeId++;
            }
            
            // Remove off-screen pipes
            pipes.erase(std::remove_if(pipes.begin(), pipes.end(),
                [](const PipePair& p){ return (p.top.x + p.top.w) < -50.f; }), pipes.end());
        }

        // ENGINE OBJECT MODEL - local player
        auto& meGO = gRegistry.upsert(CLIENT_ID);
        meGO.set<Engine::Vec2>("pos", {skully.x, skully.y});
        meGO.set<bool>("paused", gameTime.isPaused());
        meGO.set<float>("scale", gameTime.scale());

        // NETWORK STRATEGY SEND
        if (strat == Engine::NetStrategy::FullState) {
            peerManager.updateMyPlayerData(skully.x, skully.y, gameTime.isPaused(), gameTime.scale());
        } else {
            bool L = Input::isKeyPressed(SDL_SCANCODE_A);
            bool R = Input::isKeyPressed(SDL_SCANCODE_D);
            bool J = Input::isKeyPressed(SDL_SCANCODE_SPACE);
            peerManager.sendInputDelta(L, R, J, 0.f, 0.f, SDL_GetTicks());
        }
        
        static auto lastCleanup = std::chrono::high_resolution_clock::now();
        auto now = std::chrono::high_resolution_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastCleanup).count() >= 1) {
            peerManager.cleanupStalePeers();
            lastCleanup = now;
        }
        
        auto peerData = peerManager.getPeerPlayerData();
        std::unordered_map<std::string, RemotePlayer> oldOthers = others;
        others.clear();
        
        for (const auto& [peerId, playerData] : peerData) {
            // ENGINE OBJECT MODEL - others
            auto& go = gRegistry.upsert(peerId);
            go.set<Engine::Vec2>("pos", {playerData.x, playerData.y});
            go.set<bool>("paused", playerData.paused);
            go.set<float>("scale", playerData.scale);

            auto elapsed = std::chrono::high_resolution_clock::now() - playerData.lastUpdate;
            if (elapsed > std::chrono::seconds(5)) continue; // skip stale peers

            RemotePlayer rp;
            rp.rect = {playerData.x, playerData.y, characterSize, characterSize};
            rp.paused = playerData.paused;
            rp.scale = playerData.scale;
            
            if (oldOthers.find(peerId) != oldOthers.end()) {
                rp.currentFrame = oldOthers[peerId].currentFrame;
                rp.animationAccum = oldOthers[peerId].animationAccum;
                rp.lastAnimTime = oldOthers[peerId].lastAnimTime;
            }
            others[peerId] = rp;
        }
        
        char msg[256];
        snprintf(msg, sizeof(msg), "ID %s X %.3f Y %.3f PAUSED %d SCALE %.3f",
                CLIENT_ID.c_str(), skully.x, skully.y, gameTime.isPaused() ? 1 : 0, gameTime.scale());
        peerManager.sendToServer(msg);
        
        std::string serverResponse = peerManager.receiveFromServer();
        if (!serverResponse.empty()) {
            if (serverResponse.rfind("DISCONNECT", 0) == 0) {
                char deadId[256];
                if (sscanf(serverResponse.c_str(), "DISCONNECT %255s", deadId) == 1) {
                    others.erase(deadId);
                    gRegistry.erase(deadId); // remove from registry as well
                    std::cout << "Peer " << deadId << " disconnected.\n";
                }
            } else {
                // Note: Server pipe sync removed - using local pipe spawning only
                // (Each client spawns its own pipes independently)
            }
        }

        SDL_SetRenderDrawColor(renderer,100,150,255,255);
        SDL_RenderClear(renderer);

        // Ground (with camera offset)
        SDL_FRect groundWorld = applyCamera(ground, cameraX);
        SDL_FRect gDst=Scaling::compute(groundWorld,window);
        if(brickTex){
            float tw=0,th=0; SDL_GetTextureSize(brickTex,&tw,&th); if(tw<1) tw=64; if(th<1) th=64;
            float scaleY=ground.h/th, tileW=tw*scaleY, tileH=th*scaleY;
            // Extend ground for camera scrolling
            for(float x=-cameraX;x<1920.0f+cameraX+tileW;x+=tileW){
                SDL_FRect tilePx{ x,ground.y,tileW,tileH };
                SDL_FRect tileWorld = applyCamera(tilePx, cameraX);
                SDL_FRect tileDst=Scaling::compute(tileWorld,window);
                SDL_RenderTexture(renderer,brickTex,nullptr,&tileDst);
            }
        } else SDL_RenderFillRect(renderer,&gDst);
        
        // Render skully platform (with camera offset)
        SDL_FRect spWorld = applyCamera(skullyPlatform, cameraX);
        SDL_FRect spDst=Scaling::compute(spWorld,window);
        if(skullyPlatformTex){
            SDL_RenderTexture(renderer,skullyPlatformTex,nullptr,&spDst);
        } else {
            SDL_SetRenderDrawColor(renderer, 200, 100, 200, 255);
            SDL_RenderFillRect(renderer,&spDst);
        }
        
        // Render grey platform - extreme right (with camera offset)
        SDL_FRect gpWorld = applyCamera(greyPlatform, cameraX);
        SDL_FRect gpDst=Scaling::compute(gpWorld,window);
        SDL_SetRenderDrawColor(renderer, 120, 120, 130, 255);
        SDL_RenderFillRect(renderer,&gpDst);
        
        // Render red moving platform (Task 3) (with camera offset)
        SDL_FRect mpWorld = applyCamera(movingPlatform, cameraX);
        SDL_FRect mpDst=Scaling::compute(mpWorld,window);
        SDL_SetRenderDrawColor(renderer, 220, 50, 50, 255);
        SDL_RenderFillRect(renderer,&mpDst);

        // Render pipes (with camera offset)
        SDL_SetRenderDrawColor(renderer,20,120,50,255);
        for(auto& p:pipes){
            SDL_FRect tWorld = applyCamera(p.top, cameraX);
            SDL_FRect bWorld = applyCamera(p.bottom, cameraX);
            SDL_FRect t=Scaling::compute(tWorld,window), b=Scaling::compute(bWorld,window);
            SDL_RenderFillRect(renderer,&t); SDL_RenderFillRect(renderer,&b);
        }

        // Render local player (with camera offset)
        SDL_FRect src{ FRAME_W*currentFrame,0.f,FRAME_W,FRAME_H };
        SDL_FRect skullyWorld = applyCamera(skully, cameraX);
        SDL_FRect dst=Scaling::compute(skullyWorld,window);
        SDL_RenderTexture(renderer,skullTex,&src,&dst);

        // Render other players (with camera offset)
        for(auto& kv:others){
            SDL_FRect otherWorld = applyCamera(kv.second.rect, cameraX);
            SDL_FRect d=Scaling::compute(otherWorld,window);
            if (!kv.second.paused) {
                auto now = std::chrono::high_resolution_clock::now();
                double realdeltaSec = std::chrono::duration<double>(now - kv.second.lastAnimTime).count();
                kv.second.lastAnimTime = now;
                kv.second.animationAccum += realdeltaSec * kv.second.scale;
                while(kv.second.animationAccum >= ANIM_FRAME_SEC) {
                    kv.second.animationAccum -= ANIM_FRAME_SEC;
                    kv.second.currentFrame = (kv.second.currentFrame + 1) % FRAME_COUNT;
                }
            }
            SDL_FRect remoteSrc = { (float)(kv.second.currentFrame * FRAME_W), 0, FRAME_W, FRAME_H };
            SDL_RenderTexture(renderer,skullTex,&remoteSrc,&d);
        }

        SDL_RenderPresent(renderer);
    }

    if(brickTex) SDL_DestroyTexture(brickTex);
    if(skullyPlatformTex) SDL_DestroyTexture(skullyPlatformTex);
    SDL_DestroyTexture(skullTex);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
