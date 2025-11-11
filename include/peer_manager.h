#pragma once
#include <unordered_map>
#include <string>
#include <mutex>
#include <vector>
#include <zmq.h>
#include "net_strategy.h"
#include <chrono>

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

    void connectToServer(const std::string& serverEndpoint);
    void connectToPeerNetwork(const std::string& peerEndpoint);
    void startPeerListener(const std::string& listenEndpoint);

    void updateMyPlayerData(float x, float y, bool paused, float scale);
    std::unordered_map<std::string, PlayerData> getPeerPlayerData();

    void sendToServer(const std::string& message);
    std::string receiveFromServer();

    void cleanupStalePeers();
    
    void processPeerMessages();
    void sendInputDelta(bool left, bool right, bool jump,
                        float analogX, float analogY, uint64_t ticksMs);

    // Broadcast an arbitrary string to all connected peers.
    void publishToPeers(const std::string& msg);

    // Non-blocking drain of all pending messages from peers.
    std::vector<std::string> drainPeerMessages();

private:
    std::string id;
    void* ctx{nullptr};
    
    void* serverSocket{nullptr};
    void* pub{nullptr};
    void* sub{nullptr};
    
    std::unordered_map<std::string, PlayerData> peers;
    std::mutex peerMutex;
    
    static constexpr auto PEER_TIMEOUT = std::chrono::seconds(5);
};
