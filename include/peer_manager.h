#pragma once
#include <unordered_map>
#include <string>
#include <mutex>
#include <atomic>
#include <zmq.h>

/**
 * PeerManager
 * A reusable engine component for peer-to-peer networking.
 * Handles discovery, publishing local player state, and receiving peer states.
 */
class PeerManager {
public:
    struct Pose {
        float x{0}, y{0};
    };

    PeerManager(const std::string& myId);
    ~PeerManager();

    // Connect to existing network of peers
    void connect(const std::string& endpoint);

    // Start listening for new peers
    void listen(const std::string& endpoint);

    // Update your own player pose (broadcasts to peers)
    void updateMyPose(float x, float y);

    // Get the latest known poses of all peers
    std::unordered_map<std::string, Pose> getPeerPoses();

    // Get/set ghost position (special shared object)
    void updateGhost(float gx, float gy);
    void getGhost(float& gx, float& gy);

private:
    std::string id;
    void* ctx{nullptr};
    void* pub{nullptr};
    void* sub{nullptr};

    std::atomic<float> ghostX{1500.f};
    std::atomic<float> ghostY{600.f};

    std::unordered_map<std::string, Pose> peers;
    std::mutex peerMutex;
};
