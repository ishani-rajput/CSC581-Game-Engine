# Feeling Spikey — Multiplayer Game Engine

A platformer game demonstrating timeline control, multithreading, client-server networking, asynchronous communication, and hybrid peer-to-peer architectures.

## 🎮 Controls
- **W / Space** — Jump  
- **A** — Move left  
- **D** — Move right  
- **P** — Pause/Unpause timeline
- **1** — Slow motion (0.5x speed)
- **2** — Normal speed (1.0x)
- **3** — Fast forward (2.0x speed)
- **S** — Toggle scaling mode (Pixel ↔ Proportional)

## 🏆 Goal
- Start on the left platform  
- Avoid the spikes by riding the moving platform across  
- Reach the right platform safely  
- Falling into spikes or off the screen resets the player  

---

## 📦 Build Instructions

```bash
cd build
cmake ..
ninja
```

---

## 🎯 Testing Different Architectures

### **1. Single-Player Multithreaded (Sections 1 & 3)**
Tests timeline control and multithreaded game loop with separate player and environment threads.

```bash
./main
```

**What to test:**
- Press **P** to pause — both player and platform should freeze
- Press **1/2/3** to change speed — everything scales together
---

### **2. Client-Server Multiplayer (Sections 2 & 4)**
Tests client-server networking with per-client asynchronous threading.

**Start the server:**
```bash
./server
```

**Start multiple clients (in separate terminals):**
```bash
./client
./client
./client
```

**What to test:**
- All clients see the same moving platform position (server-authoritative)
- Each client controls their own character
- Press **1/2/3** on different clients to change their individual speeds
- Fast client (3) sends more network messages, slow client (1) sends fewer
- Server console shows client connections and threading

**Expected behavior:**
- Client A at 0.5x speed: sends ~30 messages/sec
- Client B at 1.0x speed: sends ~60 messages/sec
- Client C at 2.0x speed: sends ~120 messages/sec
- All clients see platform in identical position

---

### **3. Hybrid Peer-to-Peer (Section 5)**
Tests hybrid architecture where server controls platforms and peers communicate directly for player positions.

**Start the hybrid server:**
```bash
./hybrid_server
```

**Start multiple hybrid clients (in separate terminals):**
```bash
./hybrid_client
./hybrid_client
./hybrid_client
```

**What to test:**
- Server only sends platform updates (~30/sec)
- Player positions communicated directly peer-to-peer (PUB/SUB)
- New clients automatically discover and connect to existing peers
- Console shows peer discovery messages
- If a client crashes, others remove it after 3 seconds

**Network architecture:**
- **Server → Clients:** Platform positions (REQ/REP, low frequency)
- **Client ↔ Clients:** Player positions (PUB/SUB, high frequency, direct)

---

### **main.cpp** — Single-player multithreaded
- Player logic thread
- Environment thread
- Main rendering thread
- Demonstrates Section 1 (Timeline) and Section 3 (Multithreading)

### **client.cpp + server.cpp** — Client-server
- Server: NetworkServer with per-client threads
- Client: Personal timeline affects network rate
- Demonstrates Section 2 (Networking) and Section 4 (Asynchronicity)

### **hybrid_client.cpp + hybrid_server.cpp** — Hybrid P2P
- Server: Platform authority only
- Clients: Direct peer communication via PUB/SUB
- PeerManager handles discovery and cleanup
- Demonstrates Section 5 (Peer-to-Peer)

---

## 📝 Project Sections Demonstrated

✅ **Section 1:** Timeline with pause, 0.5x/1.0x/2.0x speed  
✅ **Section 2:** Client-server with 3+ clients  
✅ **Section 3:** Multithreaded game loop  
✅ **Section 4:** Asynchronous per-client threading, independent speeds  
✅ **Section 5:** Hybrid P2P with direct peer communication  

---

## 📂 File Structure

```
src/
├── main.cpp              # Single-player multithreaded (Sections 1 & 3)
├── client.cpp            # Client-server client (Sections 2 & 4)
├── server.cpp            # Client-server server (Sections 2 & 4)
├── hybrid_client.cpp     # Hybrid P2P client (Section 5)
├── hybrid_server.cpp     # Hybrid P2P server (Section 5)
├── network_server.cpp    # NetworkServer base class
├── peer_manager.cpp      # P2P communication manager
├── timeline.cpp          # Timeline system
├── entity.cpp            # Entity rendering
├── physics.cpp           # Physics engine
├── collision.cpp         # Collision detection
└── input.cpp             # Input handling

assets/
├── player.png            # Player sprite sheet
├── platform.png          # Moving platform
├── ground.png            # Static platforms
├── ground_bottom.png     # Platform base
└── spikes.png            # Hazards
```
