#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include "input.h"
#include "physics.h"
#include "collision.h"
#include "scaling.h"
#include <vector>
#include <algorithm>
#include <cstdlib>

// Types
struct PipePair { 
    SDL_FRect top, bottom; 
    bool scored=false; 
};

// Helpers
static float floatRand(float a, float b){ 
    return a + (b-a) * (float)rand()/(float)RAND_MAX; 
}

static SDL_Texture* tryLoadTexture(SDL_Renderer* r, const char* const* paths, int n){
    for(int i=0;i<n;++i){ 
        if(!paths[i]) continue; 
        if(SDL_Texture* t=IMG_LoadTexture(r, paths[i])) return t; 
    }
    SDL_Log("Failed to load texture: %s", SDL_GetError()); return nullptr;
}

int main(int, char**){
    if(!SDL_Init(SDL_INIT_VIDEO)){ 
        SDL_Log("SDL init failed: %s", SDL_GetError()); 
        return 1; 
    }
    SDL_Window* window=nullptr; 
    SDL_Renderer* renderer=nullptr;
    if(!SDL_CreateWindowAndRenderer("Feeling Loopy - Skully Bird",1920,1080,SDL_WINDOW_RESIZABLE,&window,&renderer)){
        SDL_Log("CreateWindowAndRenderer failed: %s", SDL_GetError()); 
        SDL_Quit(); 
        return 1; 
    }
    SDL_SetRenderVSync(renderer, 1); 

    // assets
    const char* skullPaths[] = {"../assets/skullFace.png","assets/skullFace.png"};
    SDL_Texture* skullTex = tryLoadTexture(renderer, skullPaths, 2);
    if(!skullTex){ 
        SDL_DestroyRenderer(renderer); 
        SDL_DestroyWindow(window); 
        SDL_Quit(); return 1; 
    }
    const char* brickPaths[] = {"../assets/platform.png","assets/platform.png"};
    SDL_Texture* brickTex = tryLoadTexture(renderer, brickPaths, 2);
    SDL_SetTextureScaleMode(skullTex, SDL_SCALEMODE_NEAREST);
    if(brickTex) SDL_SetTextureScaleMode(brickTex, SDL_SCALEMODE_NEAREST);
    float texW=0, texH=0; 
    SDL_GetTextureSize(skullTex,&texW,&texH);
    const int FRAME_COUNT=6, ANIM_MS=100; 
    const float FRAME_W=texW/FRAME_COUNT, FRAME_H=texH;
    int currentFrame=0; Uint32 lastAnimTick=SDL_GetTicks();
    // world & player
    float WORLD_W=1920.f, WORLD_H=1080.f;  

    float characterSize = 108.f;  
    SDL_FRect skully={ 480.f, 540.f, characterSize, characterSize };  
    Body skBody; skBody.affectedByGravity=true; Physics::setGravity(2400.f);
    const float JUMP_VELOCITY=-900.f;
    // ground
    SDL_FRect ground={0.f, 1080.f-120.f, 1920.f, 120.f};  
    // pipes
    std::vector<PipePair> pipes; 
    const float PIPE_SPEED=-450.f, PIPE_W=140.f, PIPE_GAP=280.f;
    const float PIPE_SPAWN_EVERY=1.4f; 
    float spawnTimer=0.f;
    Uint32 lastTicks=SDL_GetTicks(); 
    bool running=true; 
    SDL_Event ev; 
    bool prevSpace=false; 
    bool prevToggle=false;
    // window size tracking
    int winW=0, winH=0; 
    SDL_GetWindowSize(window, &winW, &winH);
    
    // scaling
    Scaling::setMode(ScaleMode::Pixel);
    auto resetPipesOnly=[&](){ 
        pipes.clear(); 
        spawnTimer=0.f; 
    };
    auto spawnPipe=[&](){
        float center = floatRand(1080.0f*0.30f, 1080.0f*0.70f);
        float topH = center - PIPE_GAP*0.5f;
        float bottomY = center + PIPE_GAP*0.5f;
        
        pipes.push_back({ 
            SDL_FRect{1920.0f + PIPE_W, 0.f, PIPE_W, topH},
            SDL_FRect{1920.0f + PIPE_W, bottomY, PIPE_W, 1080.0f-bottomY-120.0f} 
        });
    };
    while(running){
        while(SDL_PollEvent(&ev)){ 
            if(ev.type==SDL_EVENT_QUIT) 
            running=false; 
        }
        Input::poll();
        if(Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) running=false;
        
        Uint32 now=SDL_GetTicks();
        float dtSec=(float)(now-lastTicks)*0.001f;
        if (dtSec > 0.033f) dtSec = 0.033f; 
        lastTicks=now;
        if(now-lastAnimTick >= (Uint32)ANIM_MS){ 
            currentFrame=(currentFrame+1)%FRAME_COUNT; 
            lastAnimTick=now; 
        }
        // jump
        bool spaceNow=Input::isKeyPressed(SDL_SCANCODE_SPACE);
        if(spaceNow && !prevSpace) skBody.vy=JUMP_VELOCITY; prevSpace=spaceNow;
        
        // toggle scaling mode
        bool toggleNow=Input::isKeyPressed(SDL_SCANCODE_T);
        if(toggleNow && !prevToggle) {
            ScaleMode currentMode = Scaling::mode();
            ScaleMode newMode = (currentMode == ScaleMode::Pixel) ? ScaleMode::Proportional : ScaleMode::Pixel;
            Scaling::setMode(newMode);
            SDL_Log("Switched to %s scaling mode", (newMode == ScaleMode::Pixel) ? "Pixel" : "Proportional");
        }
        prevToggle=toggleNow;
        // physics
        Physics::step(dtSec*1000.f, skully.x, skully.y, skBody);
        if(skully.y+skully.h>=ground.y){ 
            skully.y=ground.y-skully.h; 
            skBody.vy=0.f; 
        }
        if(skully.y<0.f){ 
            skully.y=0.f; 
            skBody.vy=0.f; 
        }
        // pipes
        spawnTimer+=dtSec; 
        if(spawnTimer>=PIPE_SPAWN_EVERY){ 
            spawnTimer-=PIPE_SPAWN_EVERY; 
            spawnPipe(); 
        }
        for(auto& p:pipes){ 
            p.top.x+=PIPE_SPEED*dtSec; 
            p.bottom.x+=PIPE_SPEED*dtSec; 
        }
        pipes.erase(std::remove_if(pipes.begin(),pipes.end(),
            [&](PipePair& pp){ return (pp.top.x+pp.top.w)<-50.f; }), pipes.end());
        bool hit=false; 
        for(auto& p:pipes){ 
            if(aabbIntersect(skully,p.top)||aabbIntersect(skully,p.bottom)){ 
                hit=true; break; 
            } 
        }
        if(hit) resetPipesOnly();
        // window resize 
        int ww=0, wh=0; SDL_GetWindowSize(window, &ww, &wh);
        if (ww != winW || wh != winH) {
            // Update stored window size
            winW = ww; winH = wh;
        }
        // Blue background
        SDL_SetRenderDrawColor(renderer, 100, 150, 255, 255);  
        SDL_RenderClear(renderer);
        // ground
        SDL_FRect gDst = Scaling::compute(ground, window);
        if(brickTex){
            float tw=0,th=0; 
            SDL_GetTextureSize(brickTex,&tw,&th); 
            if(tw<1) tw=64; 
            if(th<1) th=64;
            float scaleY=ground.h/th, tileW=tw*scaleY, tileH=th*scaleY;
            for(float x=0;x<1920.0f+tileW;x+=tileW){
                SDL_FRect tilePx{ x,ground.y,tileW,tileH };
                SDL_FRect tileDst=Scaling::compute(tilePx, window);
                SDL_RenderTexture(renderer, brickTex, nullptr, &tileDst);
            }
        }else{ 
            SDL_RenderFillRect(renderer,&gDst); 
        }
        // pipes
        SDL_SetRenderDrawColor(renderer, 20, 120, 50, 255);
        for(auto& p:pipes){
            SDL_FRect t=Scaling::compute(p.top, window), b=Scaling::compute(p.bottom, window);
            SDL_RenderFillRect(renderer,&t); 
            SDL_RenderFillRect(renderer,&b);
        }
        // player
        SDL_FRect dst=Scaling::compute(skully, window);
        SDL_FRect src{ 
            FRAME_W*currentFrame,0.f,FRAME_W,FRAME_H 
        };
        SDL_RenderTexture(renderer, skullTex, &src, &dst);
        SDL_RenderPresent(renderer);
    }
    if (brickTex) SDL_DestroyTexture(brickTex);
    SDL_DestroyTexture(skullTex);
    SDL_DestroyRenderer(renderer); 
    SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
