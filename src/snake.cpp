#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <deque>
#include <string>
#include <random>
#include <vector>
#include <iostream>
#include "registry.h"
#include "timeline.h"
#include "event_manager.h"
#include "input.h"
#include "memory_pool.h"
#include "scaling.h"
#include "collision.h"

static constexpr float W = 1920;
static constexpr float H = 1080;
static constexpr float CELL = 25;
static constexpr int COLS = int(W / CELL);
static constexpr int ROWS = int(H / CELL);
static constexpr int MAX_SNAKE = COLS * ROWS;

struct Dir { int dx, dy; };
static const Dir UP{0,-1}, DOWN{0,1}, LEFT{-1,0}, RIGHT{1,0};

enum class EndState { None, Lose, Win };

struct GameState {
    std::deque<std::string> parts;
    Dir cur{1,0};
    Dir nxt{1,0};
    float acc = 0;
    float step = 0.12f;
    float normalStep = 0.12f;
    float boostStep = 0.06f;
    bool paused = false;
    int ids = 0;
    int score = 0;
    int highScore = 0;
    EndState end = EndState::None;
    bool isBoosting = false;
};

static GameState G;

static void rect(SDL_Renderer* r, float x,float y,float w,float h, SDL_Color c){
    SDL_SetRenderDrawColor(r,c.r,c.g,c.b,c.a);
    SDL_FRect rc{x,y,w,h};
    SDL_RenderFillRect(r,&rc);
}

static void glow(SDL_Renderer* r,float x,float y,float w,float h, SDL_Color c){
    SDL_SetRenderDrawColor(r,c.r,c.g,c.b,180);
    SDL_FRect a{x-3,y-3,w+6,h+6};
    SDL_RenderFillRect(r,&a);
    SDL_SetRenderDrawColor(r,c.r,c.g,c.b,255);
    SDL_FRect b{x,y,w,h};
    SDL_RenderFillRect(r,&b);
}

static bool isInBoundary(int gx, int gy) {
    int midCol = COLS / 2;
    if (gx == midCol && gy >= ROWS/4 && gy < 3*ROWS/4) {
        return true;
    }
    int midRow = ROWS / 2;
    if (gy == midRow) {
        if (gx < COLS/4 || gx >= 3*COLS/4) {
            return true;
        }
    }
    return false;
}

static SDL_FRect gridToRect(int gx, int gy) {
    return SDL_FRect{(float)(gx * CELL), (float)(gy * CELL), CELL, CELL};
}

static bool checkBoundaryCollision(int gx, int gy) {
    SDL_FRect pos = gridToRect(gx, gy);
    
    int midCol = COLS / 2;
    for(int row = ROWS/4; row < 3*ROWS/4; row++) {
        SDL_FRect wall = gridToRect(midCol, row);
        if(aabbIntersect(pos, wall)) return true;
    }
    
    int midRow = ROWS / 2;
    for(int col = 0; col < COLS/4; col++) {
        SDL_FRect wall = gridToRect(col, midRow);
        if(aabbIntersect(pos, wall)) return true;
    }
    for(int col = 3*COLS/4; col < COLS; col++) {
        SDL_FRect wall = gridToRect(col, midRow);
        if(aabbIntersect(pos, wall)) return true;
    }
    
    return false;
}

static void drawWalls(SDL_Renderer* r){
    SDL_Color wc{180,200,255,255};
    SDL_SetRenderDrawColor(r, wc.r, wc.g, wc.b, wc.a);
    SDL_FRect t{0,0,W,4}, b{0,H-4,W,4}, l{0,0,4,H}, ri{W-4,0,4,H};
    SDL_RenderFillRect(r,&t);
    SDL_RenderFillRect(r,&b);
    SDL_RenderFillRect(r,&l);
    SDL_RenderFillRect(r,&ri);
}

static void drawScore(SDL_Renderer* r, int score){
    SDL_SetRenderDrawColor(r,0,0,0,160);
    SDL_FRect box{10,10,160,40};
    SDL_RenderFillRect(r,&box);

    SDL_SetRenderDrawColor(r,255,255,255,255);
    std::string s = "Score: " + std::to_string(score);
    float x = 20, y = 20;

    for(char c : s){
        SDL_RenderLine(r, x, y, x + 8, y);
        x += 10;
    }
}

static std::string nextId(GameState& g){
    return "s_"+std::to_string(g.ids++);
}

static void spawnFood(Engine::Registry& reg,const GameState& g, int fx=-1, int fy=-1){
    static std::mt19937 rng{std::random_device{}()};
    
    if(fx < 0 || fy < 0) {
        std::uniform_int_distribution<int> rx(1,COLS-2);
        std::uniform_int_distribution<int> ry(1,ROWS-2);
        bool ok=false;

        while(!ok){
            fx=rx(rng); 
            fy=ry(rng);
            ok=true;

            if(isInBoundary(fx, fy)){
                ok=false;
                continue;
            }

            for(auto& id:g.parts){
                auto* s=reg.get(id);
                if(s && s->get<int>("gx")==fx && s->get<int>("gy")==fy){
                    ok=false; 
                    break;
                }
            }
        }
    }

    auto& f=reg.upsert("food");
    f.set("gx",fx);
    f.set("gy",fy);
    f.set("kind",2);
}

static void initSnake(GameState& g,Engine::Registry& reg, bool spawnInitialFood = true){
    g.parts.clear();
    g.ids=0;
    g.score=0;
    g.cur={1,0};
    g.nxt={1,0};
    g.acc=0;
    g.end=EndState::None;

    int cx = COLS/2, cy = ROWS/2;
    std::string h = nextId(g);
    auto& hd = reg.upsert(h);
    hd.set("gx",cx);
    hd.set("gy",cy);
    hd.set("kind",0);
    g.parts.push_back(h);

    for(int i=1;i<3;i++){
        std::string b = nextId(g);
        auto& s = reg.upsert(b);
        s.set("gx",cx-i);
        s.set("gy",cy);
        s.set("kind",1);
        g.parts.push_back(b);
    }

    if(spawnInitialFood) {
        spawnFood(reg,g);
    }
}

static void resetGame(GameState& g,Engine::Registry& reg){
    if(g.score > g.highScore) {
        g.highScore = g.score;
    }
    reg.clear();
    initSnake(g,reg);
}

static void gameLose(GameState& g){ g.end=EndState::Lose; }

static void update(GameState& g,Engine::Registry& reg,Timeline& tl,Engine::EventManager& em,float dt){
    if(g.paused || g.end!=EndState::None) return;

    g.acc += dt;
    if(g.acc < g.step) return;
    g.acc = 0;

    g.cur = g.nxt;

    auto* hd = reg.get(g.parts.front());
    if(!hd) return;

    int hx=hd->get<int>("gx"), hy=hd->get<int>("gy");
    int nx=hx + g.cur.dx, ny=hy + g.cur.dy;

    if(nx<0 || nx>=COLS || ny<0 || ny>=ROWS){
        gameLose(g);
        return;
    }

    if(checkBoundaryCollision(nx, ny)){
        gameLose(g);
        return;
    }

    SDL_FRect headRect = gridToRect(nx, ny);
    for(auto& id:g.parts){
        auto* s=reg.get(id);
        if(s) {
            int sx = s->get<int>("gx");
            int sy = s->get<int>("gy");
            SDL_FRect bodyRect = gridToRect(sx, sy);
            if(aabbIntersect(headRect, bodyRect)){
                gameLose(g);
                return;
            }
        }
    }

    auto* fd=reg.get("food");
    bool eat=false;
    if(fd){
        int fx=fd->get<int>("gx"), fy=fd->get<int>("gy");
        SDL_FRect foodRect = gridToRect(fx, fy);
        if(aabbIntersect(headRect, foodRect)){
            eat=true;
            g.score+=10;
            spawnFood(reg,g);
            
            if(!em.isReplaying()) {
                auto* newFood = reg.get("food");
                if(newFood) {
                    int newFx = newFood->get<int>("gx");
                    int newFy = newFood->get<int>("gy");
                    em.raiseEvent(Engine::Events::Spawn("food", (float)newFx, (float)newFy, &tl));
                }
            }
        }
    }

    hd->set("kind",1);

    std::string hid = nextId(g);
    auto& nh = reg.upsert(hid);
    nh.set("gx",nx);
    nh.set("gy",ny);
    nh.set("kind",0);
    g.parts.push_front(hid);

    if(!eat){
        std::string t=g.parts.back();
        g.parts.pop_back();
        reg.erase(t);
    }
}

static void drawDigit(SDL_Renderer* r, int digit, int x, int y, int w, int h) {
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255); 
    
    int segW = w / 5;
    int segH = h / 2;
    
    bool segments[7] = {false};
    
    switch(digit) {
        case 0: segments[0]=segments[1]=segments[2]=segments[3]=segments[4]=segments[5]=true; break;
        case 1: segments[1]=segments[2]=true; break;
        case 2: segments[0]=segments[1]=segments[6]=segments[4]=segments[3]=true; break;
        case 3: segments[0]=segments[1]=segments[6]=segments[2]=segments[3]=true; break;
        case 4: segments[5]=segments[6]=segments[1]=segments[2]=true; break;
        case 5: segments[0]=segments[5]=segments[6]=segments[2]=segments[3]=true; break;
        case 6: segments[0]=segments[5]=segments[4]=segments[3]=segments[2]=segments[6]=true; break;
        case 7: segments[0]=segments[1]=segments[2]=true; break;
        case 8: segments[0]=segments[1]=segments[2]=segments[3]=segments[4]=segments[5]=segments[6]=true; break;
        case 9: segments[0]=segments[1]=segments[2]=segments[6]=segments[5]=segments[3]=true; break;
    }
    
    SDL_FRect rect;
    if (segments[0]) { rect = SDL_FRect{(float)x, (float)y, (float)w, (float)segW}; SDL_RenderFillRect(r, &rect); }
    if (segments[1]) { rect = SDL_FRect{(float)(x+w-segW), (float)y, (float)segW, (float)segH}; SDL_RenderFillRect(r, &rect); }
    if (segments[2]) { rect = SDL_FRect{(float)(x+w-segW), (float)(y+segH), (float)segW, (float)segH}; SDL_RenderFillRect(r, &rect); }
    if (segments[3]) { rect = SDL_FRect{(float)x, (float)(y+h-segW), (float)w, (float)segW}; SDL_RenderFillRect(r, &rect); }
    if (segments[4]) { rect = SDL_FRect{(float)x, (float)(y+segH), (float)segW, (float)segH}; SDL_RenderFillRect(r, &rect); }
    if (segments[5]) { rect = SDL_FRect{(float)x, (float)y, (float)segW, (float)segH}; SDL_RenderFillRect(r, &rect); }
    if (segments[6]) { rect = SDL_FRect{(float)x, (float)(y+segH-segW/2), (float)w, (float)segW}; SDL_RenderFillRect(r, &rect); }
}

static void drawNumber(SDL_Renderer* r, int number, int x, int y, int digitWidth, int digitHeight) {
    if (number == 0) {
        drawDigit(r, 0, x, y, digitWidth, digitHeight);
        return;
    }
    
    std::vector<int> digits;
    int n = number;
    while (n > 0) {
        digits.push_back(n % 10);
        n /= 10;
    }
    
    int offsetX = x;
    for (int i = digits.size() - 1; i >= 0; --i) {
        drawDigit(r, digits[i], offsetX, y, digitWidth, digitHeight);
        offsetX += digitWidth + 5;
    }
}

static void drawEnd(SDL_Renderer* r, EndState st, int score, int highScore, SDL_Texture* loseTexture){
    SDL_SetRenderDrawColor(r,0,0,0,180);
    SDL_FRect v{0,0,W,H};
    SDL_RenderFillRect(r,&v);

    float cx=W/2, cy=H/2;

    if(loseTexture) {
        SDL_FRect imgRect = {cx - 200.f, cy - 150.f, 400.f, 150.f};
        SDL_RenderTexture(r, loseTexture, nullptr, &imgRect);
    }
    
    SDL_SetRenderDrawColor(r, 40, 40, 60, 255);
    SDL_FRect scoreBox = {cx - 150.f, cy + 20.f, 300.f, 80.f};
    SDL_RenderFillRect(r, &scoreBox);
    
    SDL_SetRenderDrawColor(r,255,255,255,255);
    drawNumber(r, score, (int)(cx - 80), (int)(cy + 35), 40, 50);
    
    if(highScore > 0) {
        SDL_FRect highScoreBox = {cx - 150.f, cy + 110.f, 300.f, 60.f};
        SDL_SetRenderDrawColor(r, 30, 30, 50, 255);
        SDL_RenderFillRect(r, &highScoreBox);
        
        SDL_SetRenderDrawColor(r,255,215,0,255);
        drawNumber(r, highScore, (int)(cx - 70), (int)(cy + 120), 35, 40);
    }
}

static void draw(SDL_Renderer* r,const GameState& g,Engine::Registry& reg, SDL_Texture* loseTexture, Engine::EventManager* em){
    SDL_SetRenderDrawColor(r,18,8,32,255);
    SDL_RenderClear(r);

    drawWalls(r);

    SDL_SetRenderDrawColor(r,150,150,180,255);
    
    int midCol = COLS / 2;
    for(int row = ROWS/4; row < 3*ROWS/4; row++){
        float x = midCol * CELL;
        float y = row * CELL;
        rect(r, x+2, y+2, CELL-4, CELL-4, SDL_Color{150,150,180,255});
    }
    
    int midRow = ROWS / 2;
    for(int col = 0; col < COLS/4; col++){
        float x = col * CELL;
        float y = midRow * CELL;
        rect(r, x+2, y+2, CELL-4, CELL-4, SDL_Color{150,150,180,255});
    }
    for(int col = 3*COLS/4; col < COLS; col++){
        float x = col * CELL;
        float y = midRow * CELL;
        rect(r, x+2, y+2, CELL-4, CELL-4, SDL_Color{150,150,180,255});
    }

    auto* fd=reg.get("food");
    if(fd){
        int gx=fd->get<int>("gx"), gy=fd->get<int>("gy");
        glow(r,gx*CELL, gy*CELL, CELL, CELL, SDL_Color{255,180,40,255});
    }

    for(auto& id:g.parts){
        auto* s=reg.get(id);
        if(!s) continue;
        int gx=s->get<int>("gx"), gy=s->get<int>("gy");
        int kind=s->get<int>("kind");

        SDL_Color hc{110,255,190,255};
        SDL_Color bc{70,210,150,255};
        
        if(g.isBoosting) {
            hc = SDL_Color{255,255,100,255};
            bc = SDL_Color{255,220,100,255};
        }

        rect(r,gx*CELL+1, gy*CELL+1, CELL-2, CELL-2, kind==0?hc:bc);
    }

    SDL_SetRenderDrawColor(r, 100, 100, 100, 200);
    SDL_FRect scoreBackground = {10.f, 10.f, 150.f, 40.f};
    SDL_RenderFillRect(r, &scoreBackground);
    drawNumber(r, g.score, 20, 15, 25, 30);
    
    if(g.isBoosting) {
        SDL_SetRenderDrawColor(r, 255, 200, 0, 220);
        SDL_FRect boostIndicator = {10.f, 60.f, 150.f, 30.f};
        SDL_RenderFillRect(r, &boostIndicator);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        for(int i = 0; i < 5; i++) {
            SDL_RenderLine(r, 20 + i*25, 70, 35 + i*25, 70);
        }
    }
    
    if(g.highScore > 0) {
        SDL_SetRenderDrawColor(r, 80, 80, 100, 200);
        SDL_FRect highScoreBackground = {W - 160.f, 10.f, 150.f, 40.f};
        SDL_RenderFillRect(r, &highScoreBackground);
        SDL_SetRenderDrawColor(r,255,215,0,255);
        drawNumber(r, g.highScore, (int)(W - 145), 15, 25, 30);
    }

    if(em && em->isRecording()) {
        SDL_SetRenderDrawColor(r, 255, 0, 0, 255);
        SDL_FRect recDot = {W - 50.f, 65.f, 16.f, 16.f};
        SDL_RenderFillRect(r, &recDot);
    }

    if(g.end!=EndState::None && !em->isReplaying()){
        drawEnd(r,g.end,g.score,g.highScore,loseTexture);
    }

    SDL_RenderPresent(r);
}

int main(){
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* win=SDL_CreateWindow("Neo Snake", int(W), int(H), SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren=SDL_CreateRenderer(win,nullptr);

    SDL_Texture* loseTexture = IMG_LoadTexture(ren, "../assets/lose.png");

    Timeline tl; 
    tl.anchorToRealTime(); 
    tl.setScale(1.0);

    Engine::PoolAllocator<Engine::GameObject> pool(600);
    Engine::Registry reg(&pool);
    Engine::EventManager em(&tl);

    InputChord boostUp("boost_up", {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_UP});
    Input::registerChord(boostUp);
    
    InputChord boostDown("boost_down", {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_DOWN});
    Input::registerChord(boostDown);
    
    InputChord boostLeft("boost_left", {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LEFT});
    Input::registerChord(boostLeft);
    
    InputChord boostRight("boost_right", {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RIGHT});
    Input::registerChord(boostRight);

    em.registerListener(Engine::EventType::Input, [](const Engine::Event& ev) {
        auto keyIt = ev.payload.find("key");
        if(keyIt != ev.payload.end() && std::holds_alternative<std::string>(keyIt->second)) {
            std::string key = std::get<std::string>(keyIt->second);
            if(key == "up") G.nxt = UP;
            else if(key == "down") G.nxt = DOWN;
            else if(key == "left") G.nxt = LEFT;
            else if(key == "right") G.nxt = RIGHT;
            
            G.step = G.normalStep;
            G.isBoosting = false;
        }
    });

    em.registerListener(Engine::EventType::InputChord, [](const Engine::Event& ev) {
        auto chordIt = ev.payload.find("chord");
        if(chordIt != ev.payload.end() && std::holds_alternative<std::string>(chordIt->second)) {
            std::string chord = std::get<std::string>(chordIt->second);
            if(chord == "boost_up") {
                G.nxt = UP;
                G.step = G.boostStep;
                G.isBoosting = true;
            }
            else if(chord == "boost_down") {
                G.nxt = DOWN;
                G.step = G.boostStep;
                G.isBoosting = true;
            }
            else if(chord == "boost_left") {
                G.nxt = LEFT;
                G.step = G.boostStep;
                G.isBoosting = true;
            }
            else if(chord == "boost_right") {
                G.nxt = RIGHT;
                G.step = G.boostStep;
                G.isBoosting = true;
            }
        }
    });

    em.registerListener(Engine::EventType::Spawn, [&reg](const Engine::Event& ev) {
        auto entityIt = ev.payload.find("entity");
        if(entityIt != ev.payload.end() && std::holds_alternative<std::string>(entityIt->second)) {
            std::string entity = std::get<std::string>(entityIt->second);
            if(entity == "food") {
                auto xIt = ev.payload.find("x");
                auto yIt = ev.payload.find("y");
                if(xIt != ev.payload.end() && yIt != ev.payload.end()) {
                    int fx = (int)std::get<float>(xIt->second);
                    int fy = (int)std::get<float>(yIt->second);
                    spawnFood(reg, G, fx, fy);
                }
            }
        }
    });

    initSnake(G,reg);

    bool run=true;
    bool pPrev=false, rPrev=false, tPrev=false, recPrev=false, s1=false,s2=false,s3=false;

    while(run){
        SDL_Event e;
        while(SDL_PollEvent(&e)) if(e.type==SDL_EVENT_QUIT) run=false;

        Input::poll();
        if(Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) run=false;

        bool pNow=Input::isKeyPressed(SDL_SCANCODE_P);
        if(pNow && !pPrev) G.paused=!G.paused;
        pPrev=pNow;

        bool rNow=Input::isKeyPressed(SDL_SCANCODE_R);
        if(rNow && !rPrev) resetGame(G,reg);
        rPrev=rNow;

        bool tNow=Input::isKeyPressed(SDL_SCANCODE_T);
        if(tNow && !tPrev) {
            auto current = Scaling::mode();
            Scaling::setMode(
                current == ScaleMode::Pixel ? ScaleMode::Proportional : ScaleMode::Pixel
            );
        }
        tPrev=tNow;

        bool recNow=Input::isKeyPressed(SDL_SCANCODE_O);
        if(recNow && !recPrev) {
            if(em.isRecording()) {
                em.stopRecording();
            } else {
                if(G.score > G.highScore) {
                    G.highScore = G.score;
                }
                reg.clear();
                initSnake(G, reg);
                
                em.startRecording();
                auto* food = reg.get("food");
                if(food) {
                    int fx = food->get<int>("gx");
                    int fy = food->get<int>("gy");
                    em.raiseEvent(Engine::Events::Spawn("food", (float)fx, (float)fy, &tl));
                }
            }
        }
        recPrev=recNow;

        if(G.end!=EndState::None){
            bool yNow=Input::isKeyPressed(SDL_SCANCODE_Y);
            static bool yPrev=false;
            if(yNow && !yPrev) {
                G.parts.clear();
                reg.clear();
                initSnake(G, reg, false);  
                
                em.playReplay();
            }
            yPrev=yNow;
        }

        if(G.end==EndState::None && !em.isReplaying()){
            bool boosting = false;
            if(Input::isChordActive("boost_up")) {
                G.nxt = UP;
                boosting = true;
                em.raiseEvent(Engine::Events::InputChord("boost_up", &tl));
            }
            else if(Input::isChordActive("boost_down")) {
                G.nxt = DOWN;
                boosting = true;
                em.raiseEvent(Engine::Events::InputChord("boost_down", &tl));
            }
            else if(Input::isChordActive("boost_left")) {
                G.nxt = LEFT;
                boosting = true;
                em.raiseEvent(Engine::Events::InputChord("boost_left", &tl));
            }
            else if(Input::isChordActive("boost_right")) {
                G.nxt = RIGHT;
                boosting = true;
                em.raiseEvent(Engine::Events::InputChord("boost_right", &tl));
            }
            else if(Input::isKeyPressed(SDL_SCANCODE_UP)) {
                G.nxt=UP;
                em.raiseEvent(Engine::Events::Input("up", true, &tl));
            }
            else if(Input::isKeyPressed(SDL_SCANCODE_DOWN)) {
                G.nxt=DOWN;
                em.raiseEvent(Engine::Events::Input("down", true, &tl));
            }
            else if(Input::isKeyPressed(SDL_SCANCODE_LEFT)) {
                G.nxt=LEFT;
                em.raiseEvent(Engine::Events::Input("left", true, &tl));
            }
            else if(Input::isKeyPressed(SDL_SCANCODE_RIGHT)) {
                G.nxt=RIGHT;
                em.raiseEvent(Engine::Events::Input("right", true, &tl));
            }
            
            if(boosting) {
                G.step = G.boostStep;
                G.isBoosting = true;
            } else {
                G.step = G.normalStep;
                G.isBoosting = false;
            }
        }

        bool k1=Input::isKeyPressed(SDL_SCANCODE_1);
        bool k2=Input::isKeyPressed(SDL_SCANCODE_2);
        bool k3=Input::isKeyPressed(SDL_SCANCODE_3);
        if(k1 && !s1) tl.setScale(0.5);
        if(k2 && !s2) tl.setScale(1.0);
        if(k3 && !s3) tl.setScale(2.0);
        s1=k1; s2=k2; s3=k3;

        float dt=float(tl.tick());
        
        em.dispatchEvents();
        
        if(dt > 0) update(G,reg,tl,em,dt);

        draw(ren,G,reg,loseTexture,&em);
        SDL_Delay(1);
    }

    if (loseTexture) SDL_DestroyTexture(loseTexture);

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
