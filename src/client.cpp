#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include "input.h"
#include "physics.h"
#include "collision.h"
#include "scaling.h"
#include "timeline.h"
#include "peer_manager.h"

#include <string>
#include <unordered_map>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>

// ---- Helpers ----
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

struct RemotePlayer { 
    SDL_FRect rect; 
    int currentFrame = 0;
    double animAccum = 0.0;
    std::chrono::high_resolution_clock::time_point lastAnimTime = std::chrono::high_resolution_clock::now();
    bool paused = false;
    float scale = 1.0f;
};

static SDL_Texture* tryLoadTexture(SDL_Renderer* r, const char* const* paths, int n){
    for(int i=0;i<n;++i){ if(!paths[i]) continue; if(SDL_Texture* t=IMG_LoadTexture(r, paths[i])) return t; }
    SDL_Log("Failed to load texture: %s", SDL_GetError()); return nullptr;
}

int main(int argc, char** argv){
    if (argc < 2) {
        std::cerr << "Usage: ./client <id>\n";
        return 1;
    }
    const std::string CLIENT_ID = argv[1];

    // --- RNG seed (same on all clients, though server is authoritative) ---
    srand(12345);

    // ---- PeerManager setup ----
    PeerManager peerManager(CLIENT_ID);
    
    // Connect to server for world state (pipes)
    peerManager.connectToServer("tcp://127.0.0.1:5555");
    
    // Initial handshake with server
    peerManager.sendToServer("CONNECT");
    std::string handshakeResponse = peerManager.receiveFromServer();
    if (handshakeResponse.find("CONNECTED") == std::string::npos) {
        std::cerr << "Failed to connect to server" << std::endl;
        return 1;
    }
    
    // Set up peer networking
    const int myNum = numericIdFrom(CLIENT_ID);
    const int myPubPort = 7000 + myNum;
    
    // Start listening for peer connections
    std::ostringstream oss; 
    oss << "tcp://*:" << myPubPort; 
    peerManager.startPeerListener(oss.str());
    
    // Connect to other peers
    for(int i = 1; i <= 20; i++) {
        if(i == myNum) continue;
        std::ostringstream ep; ep << "tcp://localhost:" << (7000 + i);
        peerManager.connectToPeerNetwork(ep.str());
    }

    // ---- SDL setup ----
    if(!SDL_Init(SDL_INIT_VIDEO)){ SDL_Log("SDL init failed: %s", SDL_GetError()); return 1; }
    SDL_Window* window=nullptr; SDL_Renderer* renderer=nullptr;
    if(!SDL_CreateWindowAndRenderer(("Client - " + CLIENT_ID).c_str(),1500,900,SDL_WINDOW_RESIZABLE,&window,&renderer)){
        SDL_Log("CreateWindowAndRenderer failed: %s", SDL_GetError()); SDL_Quit(); return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    // Assets
    const char* skullPaths[] = {"../assets/skullFace.png","assets/skullFace.png"};
    SDL_Texture* skullTex = tryLoadTexture(renderer, skullPaths, 2);
    if(!skullTex){ SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    const char* brickPaths[] = {"../assets/platform.png","assets/platform.png"};
    SDL_Texture* brickTex = tryLoadTexture(renderer, brickPaths, 2);
    SDL_SetTextureScaleMode(skullTex, SDL_SCALEMODE_NEAREST);
    if(brickTex) SDL_SetTextureScaleMode(brickTex, SDL_SCALEMODE_NEAREST);

    float texW=0, texH=0; SDL_GetTextureSize(skullTex,&texW,&texH);
    const int FRAME_COUNT=6; const float FRAME_W=texW/FRAME_COUNT, FRAME_H=texH;
    int currentFrame=0; double animAccum=0.0; const double ANIM_FRAME_SEC=0.100;

    // Local player
    float characterSize=108.f;
    SDL_FRect skully={480.f,540.f,characterSize,characterSize};
    Body skBody; skBody.affectedByGravity=true; Physics::setGravity(2400.f);
    const float JUMP_VELOCITY=-900.f;
    SDL_FRect ground={0.f,1080.f-120.f,1920.f,120.f};

    // Pipes (received from server)
    std::vector<PipePair> pipes;
    
    // Local pipe spawning for scaled speeds
    double localSpawnTimer = 0.0;
    const double PIPE_SPAWN_EVERY = 1.4;
    const float PIPE_W = 140.f;
    const float PIPE_GAP = 280.f;
    const float SCREEN_WIDTH = 1920.f;
    const float SCREEN_HEIGHT = 1080.f;

    // Timeline & scaling (Section 1: Time representation)
    Scaling::setMode(ScaleMode::Pixel);
    Timeline gameTime; 
    gameTime.anchorToRealTime(); 
    gameTime.setScale(1.0);

    // Remote players
    std::unordered_map<std::string, RemotePlayer> others;

    bool running=true; SDL_Event ev;
    bool prevSpace=false, prevToggle=false;
    bool prevP=false, prev1=false, prev2=false, prev3=false;
    double lastScale = 1.0;

    while(running){
        while(SDL_PollEvent(&ev)){ if(ev.type==SDL_EVENT_QUIT) running=false; }
        Input::poll();
        if(Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) running=false;

        // --- Scaling toggle (T) ---
        bool toggleNow=Input::isKeyPressed(SDL_SCANCODE_T);
        if(toggleNow && !prevToggle){
            Scaling::setMode(Scaling::mode()==ScaleMode::Pixel? ScaleMode::Proportional: ScaleMode::Pixel);
        }
        prevToggle=toggleNow;

        // --- Pause toggle (P) - Section 1: Timeline controls ---
        bool pNow = Input::isKeyPressed(SDL_SCANCODE_P);
        if (pNow && !prevP) {
            gameTime.togglePause();
        }
        prevP = pNow;

        // --- Speed controls (1,2,3) - Section 1: Timeline scaling ---
        bool k1Now = Input::isKeyPressed(SDL_SCANCODE_1);
        bool k2Now = Input::isKeyPressed(SDL_SCANCODE_2);
        bool k3Now = Input::isKeyPressed(SDL_SCANCODE_3);

        if (k1Now && !prev1) { gameTime.setScale(0.5); }
        if (k2Now && !prev2) { gameTime.setScale(1.0); }
        if (k3Now && !prev3) { gameTime.setScale(2.0); }
        prev1=k1Now; prev2=k2Now; prev3=k3Now;

        // --- Advance timeline ---
        double dtSec=gameTime.tick();
        
        // Check if scale changed
        if (gameTime.scale() != lastScale) {
            lastScale = gameTime.scale();
            // If returning to normal speed, clear pipes to get fresh server data
            if (gameTime.scale() == 1.0) {
                pipes.clear();
            }
        }

        // jump (always available)
        bool spaceNow=Input::isKeyPressed(SDL_SCANCODE_SPACE);
        if(spaceNow && !prevSpace) skBody.vy=JUMP_VELOCITY;
        prevSpace=spaceNow;

        // physics (only when not paused)
        if (!gameTime.isPaused()) {
            Physics::step(static_cast<float>(dtSec*1000.0), skully.x, skully.y, skBody);
            if(skully.y+skully.h>=ground.y){ skully.y=ground.y-skully.h; skBody.vy=0.f; }
            if(skully.y<0.f){ skully.y=0.f; skBody.vy=0.f; }

            // Client-side collision detection for scaled speeds
            if (gameTime.scale() != 1.0) {
                bool hit = false;
                for(auto& p : pipes) {
                    if(aabbIntersect(skully, p.top) || aabbIntersect(skully, p.bottom)) {
                        hit = true;
                        break;
                    }
                }
                if (hit) {
                    // Reset pipes and spawn timer on collision
                    pipes.clear();
                    localSpawnTimer = 0.0;
                }
            }
            // Server handles collision for normal speed
        }

        // animation (like main.cpp - always updates)
        animAccum += dtSec;
        while(animAccum >= ANIM_FRAME_SEC) {
            animAccum -= ANIM_FRAME_SEC;
            currentFrame = (currentFrame + 1) % FRAME_COUNT;
        }

        // Local pipe movement and spawning (respects speed scaling)
        if (!gameTime.isPaused()) {
            const float PIPE_SPEED = -450.f; // Match server speed
            
            // Move existing pipes
            for(auto& p : pipes) {
                p.top.x += PIPE_SPEED * dtSec;
                p.bottom.x += PIPE_SPEED * dtSec;
            }
            
            // Spawn new pipes locally when at scaled speeds
            if (gameTime.scale() != 1.0) {
                localSpawnTimer += dtSec;
                while(localSpawnTimer >= PIPE_SPAWN_EVERY) {
                    localSpawnTimer -= PIPE_SPAWN_EVERY;
                    float center = floatRand(SCREEN_HEIGHT * 0.30f, SCREEN_HEIGHT * 0.70f);
                    float topH = center - PIPE_GAP * 0.5f;
                    float bottomY = center + PIPE_GAP * 0.5f;
                    pipes.emplace_back(
                        SCREEN_WIDTH + PIPE_W, 0.f, PIPE_W, topH,
                        SCREEN_WIDTH + PIPE_W, bottomY, PIPE_W, SCREEN_HEIGHT - bottomY - 120.f
                    );
                }
            }
            
            // Remove pipes that are off-screen
            pipes.erase(std::remove_if(pipes.begin(), pipes.end(),
                [](const PipePair& p){ return (p.top.x + p.top.w) < -50.f; }), pipes.end());
        }

        // ---- Hybrid P2P Networking ----
        // 1. Update my player data (broadcasts to peers)
        peerManager.updateMyPlayerData(skully.x, skully.y, gameTime.isPaused(), gameTime.scale());
        
        // 2. Cleanup stale peers periodically
        static auto lastCleanup = std::chrono::high_resolution_clock::now();
        auto now = std::chrono::high_resolution_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastCleanup).count() >= 1) {
            peerManager.cleanupStalePeers();
            lastCleanup = now;
        }
        
        // 3. Get peer player data
        auto peerData = peerManager.getPeerPlayerData();
        
        // Store existing animation states before clearing
        std::unordered_map<std::string, RemotePlayer> oldOthers = others;
        others.clear();
        
        // Convert peer data to RemotePlayer format
        for (const auto& [peerId, playerData] : peerData) {
            RemotePlayer rp;
            rp.rect = {playerData.x, playerData.y, characterSize, characterSize};
            rp.paused = playerData.paused;
            rp.scale = playerData.scale;
            
            // Preserve animation state if player existed before
            if (oldOthers.find(peerId) != oldOthers.end()) {
                rp.currentFrame = oldOthers[peerId].currentFrame;
                rp.animAccum = oldOthers[peerId].animAccum;
                rp.lastAnimTime = oldOthers[peerId].lastAnimTime;
            }
            others[peerId] = rp;
        }
        
        // 4. Communicate with server for world state (pipes)
        char msg[256];
        snprintf(msg, sizeof(msg), "ID %s X %.3f Y %.3f PAUSED %d SCALE %.3f",
                CLIENT_ID.c_str(), skully.x, skully.y, gameTime.isPaused() ? 1 : 0, gameTime.scale());
        peerManager.sendToServer(msg);
        
        std::string serverResponse = peerManager.receiveFromServer();
        if (!serverResponse.empty()) {
            size_t numPlayers = 0, numPipes = 0;
            
            if (sscanf(serverResponse.c_str(), "N %zu P %zu", &numPlayers, &numPipes) == 2) {
                const char* lines = strchr(serverResponse.c_str(), '\n');
                
                // Skip player data lines (we get this from peers now)
                for (size_t i = 0; i < numPlayers && lines; i++) {
                    lines++;
                    lines = strchr(lines, '\n');
                }
                
                // Only update pipes if not paused AND at normal speed
                if (!gameTime.isPaused() && gameTime.scale() == 1.0) {
                    pipes.clear();
                    
                    // Parse pipes
                    for(size_t i = 0; i < numPipes && lines; i++){
                        lines++;
                        float tx,ty,tw,th,bx,by,bw,bh;
                        if(sscanf(lines,"%f %f %f %f %f %f %f %f",&tx,&ty,&tw,&th,&bx,&by,&bw,&bh)==8){
                            pipes.emplace_back(tx,ty,tw,th,bx,by,bw,bh);
                        }
                        lines=strchr(lines,'\n');
                    }
                }
            }
        }

        // ---- Render ----
        SDL_SetRenderDrawColor(renderer,100,150,255,255);
        SDL_RenderClear(renderer);

        // ground
        SDL_FRect gDst=Scaling::compute(ground,window);
        if(brickTex){
            float tw=0,th=0; SDL_GetTextureSize(brickTex,&tw,&th); if(tw<1) tw=64; if(th<1) th=64;
            float scaleY=ground.h/th, tileW=tw*scaleY, tileH=th*scaleY;
            for(float x=0;x<1920.0f+tileW;x+=tileW){
                SDL_FRect tilePx{ x,ground.y,tileW,tileH };
                SDL_FRect tileDst=Scaling::compute(tilePx,window);
                SDL_RenderTexture(renderer,brickTex,nullptr,&tileDst);
            }
        } else SDL_RenderFillRect(renderer,&gDst);

        // pipes
        SDL_SetRenderDrawColor(renderer,20,120,50,255);
        for(auto& p:pipes){
            SDL_FRect t=Scaling::compute(p.top,window), b=Scaling::compute(p.bottom,window);
            SDL_RenderFillRect(renderer,&t); SDL_RenderFillRect(renderer,&b);
        }

        // skull animation frame
        SDL_FRect src{ FRAME_W*currentFrame,0.f,FRAME_W,FRAME_H };

        // draw local player
        SDL_FRect dst=Scaling::compute(skully,window);
        SDL_RenderTexture(renderer,skullTex,&src,&dst);

        // draw remote players (respect pause + scale)
        for(auto& kv:others){
            SDL_FRect d=Scaling::compute(kv.second.rect,window);

            // Update animation only if remote player is not paused
            if (!kv.second.paused) {
                auto now = std::chrono::high_resolution_clock::now();
                double realDtSec = std::chrono::duration<double>(now - kv.second.lastAnimTime).count();
                kv.second.lastAnimTime = now;

                kv.second.animAccum += realDtSec * kv.second.scale;
                while(kv.second.animAccum >= ANIM_FRAME_SEC) {
                    kv.second.animAccum -= ANIM_FRAME_SEC;
                    kv.second.currentFrame = (kv.second.currentFrame + 1) % FRAME_COUNT;
                }
            }

            SDL_FRect remoteSrc = { (float)(kv.second.currentFrame * FRAME_W), 0, FRAME_W, FRAME_H };
            SDL_RenderTexture(renderer,skullTex,&remoteSrc,&d);
        }

        SDL_RenderPresent(renderer);
    }

    if(brickTex) SDL_DestroyTexture(brickTex);
    SDL_DestroyTexture(skullTex);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
    // PeerManager destructor handles ZMQ cleanup automatically
    SDL_Quit();
    return 0;
}