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

/**
 * Base class for multithreaded network servers.
 * Handles client connections, threading, and message distribution.
 * Game-specific servers inherit from this and implement virtual methods.
 */
class NetworkServer {
public:
    NetworkServer();
    virtual ~NetworkServer();

    /**
     * Start the multithreaded server on the specified port.
     * Creates threads for accepting connections, world updates, and per-client communication.
     */
    void startServer(int port);

    /**
     * Stop the server and clean up all threads.
     */
    void stopServer();

    /**
     * Check if server is currently running.
     */
    bool isRunning() const { return running.load(); }

protected:
    /**
     * Game-specific message handling. Called when a client sends a message.
     * @param clientId Unique identifier for the client
     * @param message The message received from the client
     */
    virtual void handleClientMessage(const std::string& clientId, const std::string& message) = 0;

    /**
     * Generate the current world state to send to all clients.
     * Called periodically by the world update thread.
     * @return String containing the world state data
     */
    virtual std::string generateWorldState() = 0;

    /**
     * Called when a new client connects. Override for game-specific initialization.
     * @param clientId Unique identifier for the new client
     */
    virtual void onClientConnected(const std::string& clientId) {}

    /**
     * Called when a client disconnects. Override for game-specific cleanup.
     * @param clientId Unique identifier for the disconnected client
     */
    virtual void onClientDisconnected(const std::string& clientId) {}

    /**
     * Set the world update frequency (how often generateWorldState is called).
     * @param fps Updates per second (default: 60)
     */
    void setWorldUpdateRate(int fps) { worldUpdateIntervalMs = 1000 / fps; }

    /**
     * Get list of currently connected client IDs.
     */
    std::vector<std::string> getConnectedClients() const;

    /**
     * Send a message to a specific client.
     */
    void sendToClient(const std::string& clientId, const std::string& message);

    /**
     * Send a message to all connected clients.
     */
    void broadcastToAllClients(const std::string& message);

private:
    // Threading control
    std::atomic<bool> running{false};
    std::thread acceptThread;
    std::thread worldUpdateThread;
    
    // ZMQ context
    void* zmqContext;
    void* acceptSocket;
    
    // Client management
    class ClientHandler;
    std::unordered_map<std::string, std::unique_ptr<ClientHandler>> clients;
    mutable std::mutex clientsMutex;
    
    // World update timing
    int worldUpdateIntervalMs = 16; // 60 FPS default
    
    // Thread functions
    void acceptClients(int port);
    void updateWorld();
    void cleanupDisconnectedClients();
    
    // Helper functions
    std::string generateClientId();
    void removeClient(const std::string& clientId);
};

/**
 * Handles communication with a single client using dedicated threads.
 * Each client gets one reader thread and one writer thread for asynchronous communication.
 */
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
    
    // Message queues with thread safety
    std::queue<std::string> outgoingMessages;
    std::mutex outgoingMutex;
    
    void readerLoop();
    void writerLoop();
};

} // namespace Engine