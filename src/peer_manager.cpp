#include "peer_manager.h"
#include <sstream>
#include <iostream>
#include "net_strategy.h"

PeerManager::PeerManager(const std::string& myId) : id(myId) {
    ctx = zmq_ctx_new();
    
    pub = zmq_socket(ctx, ZMQ_PUB);
    sub = zmq_socket(ctx, ZMQ_SUB);
    
    // Subscribe to all messages
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, "", 0);
}

PeerManager::~PeerManager() {
    if (pub) zmq_close(pub);
    if (sub) zmq_close(sub);
    if (serverSocket) zmq_close(serverSocket);
    if (ctx) zmq_ctx_term(ctx);
}

void PeerManager::connectToServer(const std::string& serverEndpoint) {
    serverSocket = zmq_socket(ctx, ZMQ_REQ);
    if (zmq_connect(serverSocket, serverEndpoint.c_str()) != 0) {
        std::cerr << "Failed to connect to server: " << zmq_strerror(errno) << "\n";
    }
}

void PeerManager::connectToPeerNetwork(const std::string& peerEndpoint) {
    if (zmq_connect(sub, peerEndpoint.c_str()) != 0) {
        std::cerr << "Failed to connect to peer network: " << zmq_strerror(errno) << "\n";
    }
}

void PeerManager::startPeerListener(const std::string& listenEndpoint) {
    if (zmq_bind(pub, listenEndpoint.c_str()) != 0) {
        std::cerr << "Failed to start peer listener: " << zmq_strerror(errno) << "\n";
    }
}

void PeerManager::updateMyPlayerData(float x, float y, bool paused, float scale) {
    std::ostringstream oss;
    oss << "PLAYER " << id << " " << x << " " << y << " " << (paused ? 1 : 0) << " " << scale;
    std::string msg = oss.str();
    if (zmq_send(pub, msg.c_str(), (int)msg.size(), 0) == -1) {
        std::cerr << "Failed to send peer message: " << zmq_strerror(errno) << std::endl;
    }

    processPeerMessages();
}

void PeerManager::processPeerMessages() {
    char buf[512];
    while (true) {
        int n = zmq_recv(sub, buf, sizeof(buf)-1, ZMQ_DONTWAIT);
        if (n <= 0) break; 
        
        buf[n] = 0;
        std::istringstream iss(buf);
        std::string type; iss >> type;
        
        if (type == "PLAYER") {
            std::string pid; float px, py; int ppaused; float pscale;
            if (iss >> pid >> px >> py >> ppaused >> pscale && pid != id) {
                std::lock_guard<std::mutex> lk(peerMutex);
                peers[pid] = {px, py, ppaused == 1, pscale, std::chrono::high_resolution_clock::now()};
            }
        }
        else if (type == "INPUT") {
            std::string pid; int left, right, jump; float ax, ay; uint64_t ticks;
            if (iss >> pid >> left >> right >> jump >> ax >> ay >> ticks && pid != id) {
                std::lock_guard<std::mutex> lk(peerMutex);
                auto& peer = peers[pid];
                peer.lastUpdate = std::chrono::high_resolution_clock::now();
            }
        }
    }
}

std::unordered_map<std::string, PeerManager::PlayerData> PeerManager::getPeerPlayerData() {
    std::lock_guard<std::mutex> lk(peerMutex);
    return peers;
}

void PeerManager::sendToServer(const std::string& message) {
    if (serverSocket) {
        zmq_send(serverSocket, message.c_str(), (int)message.size(), 0);
    }
}

std::string PeerManager::receiveFromServer() {
    if (!serverSocket) return "";
    
    char buf[8192];
    int n = zmq_recv(serverSocket, buf, sizeof(buf)-1, 0);
    if (n > 0) {
        buf[n] = 0;
        return std::string(buf);
    }
    return "";
}

void PeerManager::cleanupStalePeers() {
    auto now = std::chrono::high_resolution_clock::now();
    std::lock_guard<std::mutex> lk(peerMutex);
    
    auto it = peers.begin();
    while (it != peers.end()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastUpdate);
        if (elapsed > PEER_TIMEOUT) {
            std::cout << "Removing stale peer: " << it->first << std::endl;
            it = peers.erase(it);
        } else {
            ++it;
        }
    }
}

void PeerManager::sendInputDelta(bool left, bool right, bool jump,
                                 float ax, float ay, uint64_t ticksMs) {
    std::ostringstream oss;
    oss << "INPUT " << id << " "
        << (left?1:0) << " " << (right?1:0) << " " << (jump?1:0) << " "
        << ax << " " << ay << " " << ticksMs;
    const std::string msg = oss.str();
    if (zmq_send(pub, msg.c_str(), (int)msg.size(), 0) == -1) {
        std::cerr << "Failed to send INPUT: " << zmq_strerror(errno) << "\n";
    }
    processPeerMessages(); 
}

void PeerManager::publishToPeers(const std::string& msg) {
    if (!pub) return;
    if (zmq_send(pub, msg.c_str(), (int)msg.size(), ZMQ_DONTWAIT) == -1) {
        if (errno != EAGAIN) {
            std::cerr << "[PeerManager] Failed to publishToPeers: "
                      << zmq_strerror(errno) << std::endl;
        }
    }
}

std::vector<std::string> PeerManager::drainPeerMessages() {
    std::vector<std::string> messages;
    if (!sub) return messages;

    char buf[4096];
    int n;
    while (true) {
        n = zmq_recv(sub, buf, sizeof(buf) - 1, ZMQ_DONTWAIT);
        if (n <= 0) break;
        buf[n] = '\0';
        messages.emplace_back(buf);
    }
    return messages;
}
