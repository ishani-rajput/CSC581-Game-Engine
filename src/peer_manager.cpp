#include "peer_manager.h"
#include <sstream>
#include <iostream>
#include <thread>

PeerManager::PeerManager(const std::string& myId) : id(myId) {
    ctx = zmq_ctx_new();
    pub = zmq_socket(ctx, ZMQ_PUB);
    sub = zmq_socket(ctx, ZMQ_SUB);

    // Subscribe to all messages
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, "", 0);
}

PeerManager::~PeerManager() {
    zmq_close(pub);
    zmq_close(sub);
    zmq_ctx_term(ctx);
}

void PeerManager::listen(const std::string& endpoint) {
    if (zmq_bind(pub, endpoint.c_str()) != 0) {
        std::cerr << "PeerManager listen failed: " << zmq_strerror(errno) << "\n";
    }
}

void PeerManager::connect(const std::string& endpoint) {
    if (zmq_connect(sub, endpoint.c_str()) != 0) {
        std::cerr << "PeerManager connect failed: " << zmq_strerror(errno) << "\n";
    }
}

void PeerManager::updateMyPose(float x, float y) {
    std::ostringstream oss;
    oss << "POSE " << id << " " << x << " " << y;
    std::string msg = oss.str();
    zmq_send(pub, msg.c_str(), (int)msg.size(), 0);

    // Receive non-blocking updates
    char buf[256];
    int n = zmq_recv(sub, buf, sizeof(buf)-1, ZMQ_DONTWAIT);
    if (n > 0) {
        buf[n] = 0;
        std::istringstream iss(buf);
        std::string type; iss >> type;
        if (type == "POSE") {
            std::string pid; float px, py;
            iss >> pid >> px >> py;
            if (pid != id) {
                std::lock_guard<std::mutex> lk(peerMutex);
                peers[pid] = {px, py};
            }
        } else if (type == "GHOST") {
            float gx, gy; iss >> gx >> gy;
            ghostX.store(gx);
            ghostY.store(gy);
        }
    }
}

std::unordered_map<std::string, PeerManager::Pose> PeerManager::getPeerPoses() {
    std::lock_guard<std::mutex> lk(peerMutex);
    return peers;
}

void PeerManager::updateGhost(float gx, float gy) {
    std::ostringstream oss;
    oss << "GHOST " << gx << " " << gy;
    std::string msg = oss.str();
    zmq_send(pub, msg.c_str(), (int)msg.size(), 0);
    ghostX.store(gx);
    ghostY.store(gy);
}

void PeerManager::getGhost(float& gx, float& gy) {
    gx = ghostX.load();
    gy = ghostY.load();
}
