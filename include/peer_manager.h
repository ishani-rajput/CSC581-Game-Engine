#pragma once
#include <unordered_map>
#include <string>
#include <mutex>
#include <zmq.h>
#include <chrono>

/**
 * PeerManager
 * A reusable engine component for hybrid peer-to-peer networking.
 * Handles direct peer communication for player data while coordinating with server for world state.
 */
class PeerManager {
public:
    struct PlayerData {
        float x{0}, y{0};
        bool paused{false};
        float scale{1.0f};
        std::chrono::high_resolution_clock::time_point lastUpdate;
    };

    PeerManager(const std::string& myId);
    ~PeerManager();

    // Hybrid P2P: Connect to both server and peer network
    void connectToServer(const std::string& serverEndpoint);
    void connectToPeerNetwork(const std::string& peerEndpoint);
    void startPeerListener(const std::string& listenEndpoint);

    // Update your own player data (broadcasts to peers, not server)
    void updateMyPlayerData(float x, float y, bool paused, float scale);

    // Get the latest known player data of all peers
    std::unordered_map<std::string, PlayerData> getPeerPlayerData();

    // Server communication (for world state like pipes)
    void sendToServer(const std::string& message);
    std::string receiveFromServer();

    // Cleanup old peer data
    void cleanupStalePeers();
    
    // Process incoming peer messages (called internally by updateMyPlayerData)
    void processPeerMessages();

private:
    std::string id;
    void* ctx{nullptr};
    
    // Server communication (REQ/REP)
    void* serverSocket{nullptr};
    
    // Peer communication (PUB/SUB)
    void* pub{nullptr};
    void* sub{nullptr};
    
    // Peer management
    std::unordered_map<std::string, PlayerData> peers;
    std::mutex peerMutex;
    
    // Timeout for stale peer detection (5 seconds)
    static constexpr auto PEER_TIMEOUT = std::chrono::seconds(5);
};