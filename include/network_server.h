#pragma once

#include <zmq.h>
#include <string>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <chrono>
#include <functional>

namespace Engine {

class NetworkServer {
public:
    NetworkServer();
    virtual ~NetworkServer();

    void startServer(int port);

    void stopServer();

    bool isRunning() const { return running.load(); }

protected:
    virtual void handleClientMessage(const std::string& clientId, const std::string& message) = 0;

    virtual std::string generateWorldState() = 0;

    virtual void onClientConnected(const std::string& clientId) {}

    virtual void onClientDisconnected(const std::string& clientId) {}

    void setWorldUpdateRate(int fps) { worldUpdateIntervalMs = 1000 / fps; }

    std::vector<std::string> getConnectedClients() const;

    void sendToClient(const std::string& clientId, const std::string& message);

    void broadcastToAllClients(const std::string& message);

private:
    std::atomic<bool> running{false};
    std::thread acceptThread;
    std::thread worldUpdateThread;
    
    void* zmqContext;
    void* acceptSocket;
    
    class ClientHandler;
    std::unordered_map<std::string, std::unique_ptr<ClientHandler>> clients;
    mutable std::mutex clientsMutex;
    
    int worldUpdateIntervalMs = 16; 
    
    void acceptClients(int port);
    void updateWorld();
    void cleanupDisconnectedClients();
    
    std::string generateClientId();
    void removeClient(const std::string& clientId);
};

class NetworkServer::ClientHandler {
public:
    ClientHandler(const std::string& id, void* socket, NetworkServer* server);
    ~ClientHandler();
    
    void start();
    void stop();
    void queueMessage(const std::string& message);
    bool isConnected() const { return connected.load(); }
    
private:
    std::string clientId;
    void* clientSocket;
    NetworkServer* server;
    
    std::atomic<bool> running{false};
    std::atomic<bool> connected{true};
    
    std::thread readerThread;
    std::thread writerThread;
    
    std::queue<std::string> outgoingMessages;
    std::mutex outgoingMutex;
    
    void readerLoop();
    void writerLoop();
};

} 