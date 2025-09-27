#include "network_server.h"
#include <iostream>
#include <cstring>
#include <random>
#include <algorithm>

namespace Engine {

// ============================================================================
// NetworkServer Implementation
// ============================================================================

NetworkServer::NetworkServer() {
    zmqContext = zmq_ctx_new();
}

NetworkServer::~NetworkServer() {
    stopServer();
    if (zmqContext) {
        zmq_ctx_destroy(zmqContext);
    }
}

void NetworkServer::startServer(int port) {
    if (running.load()) {
        std::cerr << "Server is already running!" << std::endl;
        return;
    }
    
    running.store(true);
    
    // Start the accept thread
    acceptThread = std::thread(&NetworkServer::acceptClients, this, port);
    
    // Start the world update thread
    worldUpdateThread = std::thread(&NetworkServer::updateWorld, this);
    
    std::cout << "Server started on port " << port << std::endl;
}

void NetworkServer::stopServer() {
    if (!running.load()) return;
    
    running.store(false);
    
    // Wait for threads to finish
    if (acceptThread.joinable()) {
        acceptThread.join();
    }
    if (worldUpdateThread.joinable()) {
        worldUpdateThread.join();
    }
    
    // Clean up all clients
    std::lock_guard<std::mutex> lock(clientsMutex);
    clients.clear();
    
    if (acceptSocket) {
        zmq_close(acceptSocket);
        acceptSocket = nullptr;
    }
    
    std::cout << "Server stopped" << std::endl;
}

void NetworkServer::acceptClients(int port) {
    acceptSocket = zmq_socket(zmqContext, ZMQ_REP);
    std::string address = "tcp://*:" + std::to_string(port);
    
    if (zmq_bind(acceptSocket, address.c_str()) != 0) {
        std::cerr << "Failed to bind to port " << port << ": " << zmq_strerror(errno) << std::endl;
        running.store(false);
        return;
    }
    
    std::cout << "Server listening on port " << port << "..." << std::endl;
    
    char buffer[1024];
    
    while (running.load()) {
        // Set receive timeout to allow periodic checks of running flag
        int timeout = 1000; // 1 second
        zmq_setsockopt(acceptSocket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
        
        int bytesReceived = zmq_recv(acceptSocket, buffer, sizeof(buffer) - 1, 0);
        
        if (bytesReceived > 0) {
            buffer[bytesReceived] = '\0';
            std::string message(buffer);
            
            // Check if this is a new client connection request
            if (message.find("CONNECT") == 0) {
                // Create new client
                std::string clientId = generateClientId();
                
                // Create a new socket for this client (DEALER/ROUTER pattern would be better, 
                // but assignment restricts us to REQ/REP with threads)
                void* clientSocket = zmq_socket(zmqContext, ZMQ_REP);
                
                {
                    std::lock_guard<std::mutex> lock(clientsMutex);
                    clients[clientId] = std::make_unique<ClientHandler>(clientId, clientSocket, this);
                    clients[clientId]->start();
                }
                
                // Send connection confirmation
                std::string response = "CONNECTED " + clientId;
                zmq_send(acceptSocket, response.c_str(), response.length(), 0);
                
                onClientConnected(clientId);
                std::cout << "Client connected: " << clientId << std::endl;
            } else {
                // Handle regular message (fallback for REQ/REP limitation)
                // In a real implementation, we'd use ROUTER/DEALER for better async handling
                
                // Try to extract client ID from message
                std::string clientId = "unknown";
                size_t idPos = message.find("ID ");
                if (idPos != std::string::npos) {
                    size_t start = idPos + 3;
                    size_t end = message.find(" ", start);
                    if (end != std::string::npos) {
                        clientId = message.substr(start, end - start);
                    }
                }
                
                // Handle the message
                handleClientMessage(clientId, message);
                
                // Send response
                std::string response = generateWorldState();
                zmq_send(acceptSocket, response.c_str(), response.length(), 0);
            }
        } else if (errno != EAGAIN) {
            // Real error occurred
            if (running.load()) {
                std::cerr << "Error receiving message: " << zmq_strerror(errno) << std::endl;
            }
        }
        
        // Periodically clean up disconnected clients
        cleanupDisconnectedClients();
    }
}

void NetworkServer::updateWorld() {
    while (running.load()) {
        auto startTime = std::chrono::steady_clock::now();
        
        // Generate world state and broadcast to all clients
        std::string worldState = generateWorldState();
        
        // In a full ROUTER/DEALER implementation, we'd send here
        // For REQ/REP, the world state is sent as responses to client requests
        
        auto endTime = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        
        // Sleep for remaining time to maintain target framerate
        int sleepTime = worldUpdateIntervalMs - static_cast<int>(elapsed.count());
        if (sleepTime > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepTime));
        }
    }
}

void NetworkServer::cleanupDisconnectedClients() {
    std::lock_guard<std::mutex> lock(clientsMutex);
    
    auto it = clients.begin();
    while (it != clients.end()) {
        if (!it->second->isConnected()) {
            std::cout << "Client disconnected: " << it->first << std::endl;
            onClientDisconnected(it->first);
            it = clients.erase(it);
        } else {
            ++it;
        }
    }
}

std::string NetworkServer::generateClientId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(1000, 9999);
    return "client_" + std::to_string(dis(gen));
}

void NetworkServer::removeClient(const std::string& clientId) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    clients.erase(clientId);
}

std::vector<std::string> NetworkServer::getConnectedClients() const {
    std::lock_guard<std::mutex> lock(clientsMutex);
    std::vector<std::string> result;
    
    for (const auto& pair : clients) {
        if (pair.second->isConnected()) {
            result.push_back(pair.first);
        }
    }
    
    return result;
}

void NetworkServer::sendToClient(const std::string& clientId, const std::string& message) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    auto it = clients.find(clientId);
    if (it != clients.end()) {
        it->second->queueMessage(message);
    }
}

void NetworkServer::broadcastToAllClients(const std::string& message) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    for (const auto& pair : clients) {
        if (pair.second->isConnected()) {
            pair.second->queueMessage(message);
        }
    }
}

// ============================================================================
// ClientHandler Implementation
// ============================================================================

NetworkServer::ClientHandler::ClientHandler(const std::string& id, void* socket, NetworkServer* srv)
    : clientId(id), clientSocket(socket), server(srv) {
}

NetworkServer::ClientHandler::~ClientHandler() {
    stop();
    if (clientSocket) {
        zmq_close(clientSocket);
    }
}

void NetworkServer::ClientHandler::start() {
    if (running.load()) return;
    
    running.store(true);
    readerThread = std::thread(&ClientHandler::readerLoop, this);
    writerThread = std::thread(&ClientHandler::writerLoop, this);
}

void NetworkServer::ClientHandler::stop() {
    if (!running.load()) return;
    
    running.store(false);
    
    if (readerThread.joinable()) {
        readerThread.join();
    }
    if (writerThread.joinable()) {
        writerThread.join();
    }
}

void NetworkServer::ClientHandler::queueMessage(const std::string& message) {
    std::lock_guard<std::mutex> lock(outgoingMutex);
    outgoingMessages.push(message);
}

void NetworkServer::ClientHandler::readerLoop() {
    char buffer[1024];
    
    while (running.load() && connected.load()) {
        // Set a timeout for non-blocking behavior
        int timeout = 100; // 100ms
        zmq_setsockopt(clientSocket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
        
        int bytesReceived = zmq_recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        
        if (bytesReceived > 0) {
            buffer[bytesReceived] = '\0';
            std::string message(buffer);
            
            // Forward to server for processing
            server->handleClientMessage(clientId, message);
            
        } else if (errno != EAGAIN) {
            // Connection error
            connected.store(false);
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void NetworkServer::ClientHandler::writerLoop() {
    while (running.load() && connected.load()) {
        std::string messageToSend;
        bool hasMessage = false;
        
        {
            std::lock_guard<std::mutex> lock(outgoingMutex);
            if (!outgoingMessages.empty()) {
                messageToSend = outgoingMessages.front();
                outgoingMessages.pop();
                hasMessage = true;
            }
        }
        
        if (hasMessage) {
            int bytesSent = zmq_send(clientSocket, messageToSend.c_str(), messageToSend.length(), ZMQ_DONTWAIT);
            
            if (bytesSent == -1 && errno != EAGAIN) {
                // Connection error
                connected.store(false);
                break;
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
    }
}

} // namespace Engine