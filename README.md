# Game Engine with Pool Allocators & Input Chords

A custom game engine featuring pool-based memory management, input chord detection, and demonstrating code reusability across multiple game genres.

## 🎮 Games

### **Space Invaders**
A top-down shooter demonstrating pool allocator stress testing with 55 aliens.

**Controls:**
- **Arrow Keys** — Move ship left/right
- **Space** — Fire bullets
- **W+A+S+D** — Input chord (if enabled)

**Goal:** Destroy all aliens before they reach the bottom

### **Brick Breaker**
A paddle game with 144 bricks featuring health-based damage system.

**Controls:**
- **Arrow Keys** — Move paddle left/right
- **W+A+S+D** — Input chord (if enabled)

**Goal:** Break all bricks with the ball
- **Green bricks** — 1 hit to destroy
- **Blue bricks** — 2 hits to destroy
- **Red bricks** — 3 hits to destroy  

---

## 📦 Build Instructions

```bash
cd build
cmake ..
ninja
```

---

## 🎯 Running the Games

### **Space Invaders**
```bash
cd build
./space_invaders
```

### **Brick Breaker**
```bash
cd build
./brick_breaker
```

---

## 🔑 Key Features

### **Pool Allocators**
- Efficient O(1) allocation/deallocation for game objects
- Eliminates fragmentation compared to traditional `new`/`delete`
- Space Invaders: Pools for 55 aliens, 60+ bullets, 12 particles
- Brick Breaker: Pool for 144 bricks

### **Input Chord Detection**
- Recognizes multi-key combinations (e.g., W+A+S+D)
- Configurable activation thresholds
- Frame-based timing for chord recognition
- Both games support chord detection for testing

### **Code Reusability**
- **~62-66% code reuse** across three games (Feeling Spikey, Space Invaders, Brick Breaker)
- Shared engine systems:
  - Timeline (time management)
  - Registry (GameObject storage)
  - Event system (Collision, Death events)
  - Input handling with chord support
  - Pool-based memory management

---

## 📝 Milestone 5 Highlights

This repository demonstrates:
- **Pool Allocators:** Custom memory management with O(1) allocation
- **Input Chords:** Multi-key combination detection system
- **Engine Reusability:** 2000+ lines of shared code across multiple game genres
- **Game Variety:** Platformer, shooter, and paddle game using the same engine  

---

## 📂 File Structure

```
src/
├── main.cpp              # Feeling Spikey (platformer)
├── space_invaders.cpp    # Space Invaders game
├── brick_breaker.cpp     # Brick Breaker game
├── client.cpp            # Client-server client
├── server.cpp            # Client-server server
├── hybrid_client.cpp     # Hybrid P2P client
├── hybrid_server.cpp     # Hybrid P2P server
├── network_server.cpp    # NetworkServer base class
├── peer_manager.cpp      # P2P communication manager
├── timeline.cpp          # Timeline system (shared)
├── entity.cpp            # Entity rendering (shared)
├── physics.cpp           # Physics engine (shared)
├── input.cpp             # Input handling with chords (shared)
└── event_manager.cpp     # Event system (shared)

include/
├── registry.h            # GameObject registry with pools
├── timeline.h            # Timeline interface
├── input.h               # Input chord detection
├── event_manager.h       # Event system
├── collision.h           # Collision detection
├── physics.h             # Physics interface
└── object_model.h        # GameObject definitions

assets/
├── player.png            # Feeling Spikey sprites
├── platform.png          # Moving platform
├── ground.png            # Static platforms
├── ground_bottom.png     # Platform base
└── spikes.png            # Hazards
```
