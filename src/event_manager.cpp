#include "event_manager.h"
#include <iostream>
#include <sstream>

namespace Engine {

//----------------------------------------------
// Event Serialization (for networking)
//----------------------------------------------
std::string Event::serialize() const {
    std::ostringstream oss;
    
    // Format: TYPE|TIMESTAMP|PRIORITY|KEY1:TYPE:VALUE|KEY2:TYPE:VALUE|...
    oss << static_cast<int>(type) << "|" 
        << timestamp << "|" 
        << priority;
    
    for (const auto& [key, value] : payload) {
        oss << "|" << key << ":";
        
        if (std::holds_alternative<int>(value)) {
            oss << "i:" << std::get<int>(value);
        } else if (std::holds_alternative<float>(value)) {
            oss << "f:" << std::get<float>(value);
        } else if (std::holds_alternative<bool>(value)) {
            oss << "b:" << (std::get<bool>(value) ? "1" : "0");
        } else if (std::holds_alternative<std::string>(value)) {
            oss << "s:" << std::get<std::string>(value);
        }
    }
    
    return oss.str();
}

Event Event::deserialize(const std::string& data) {
    std::istringstream iss(data);
    std::string token;
    
    // Parse type
    std::getline(iss, token, '|');
    EventType type = static_cast<EventType>(std::stoi(token));
    
    // Parse timestamp
    std::getline(iss, token, '|');
    double timestamp = std::stod(token);
    
    // Parse priority
    std::getline(iss, token, '|');
    int priority = std::stoi(token);
    
    Event ev(type, timestamp, priority);
    
    // Parse payload
    while (std::getline(iss, token, '|')) {
        size_t colonPos1 = token.find(':');
        size_t colonPos2 = token.find(':', colonPos1 + 1);
        
        if (colonPos1 == std::string::npos || colonPos2 == std::string::npos)
            continue;
        
        std::string key = token.substr(0, colonPos1);
        char typeChar = token[colonPos1 + 1];
        std::string valueStr = token.substr(colonPos2 + 1);
        
        if (typeChar == 'i') {
            ev.payload[key] = std::stoi(valueStr);
        } else if (typeChar == 'f') {
            ev.payload[key] = std::stof(valueStr);
        } else if (typeChar == 'b') {
            ev.payload[key] = (valueStr == "1");
        } else if (typeChar == 's') {
            ev.payload[key] = valueStr;
        }
    }
    
    return ev;
}

//----------------------------------------------
// Constructor
//----------------------------------------------
EventManager::EventManager(Timeline* tl) : timeline(tl) {}

Timeline* EventManager::getTimeline() const {
    return timeline;
}

//----------------------------------------------
// Registration
//----------------------------------------------
void EventManager::registerListener(EventType type, Listener callback) {
    std::lock_guard<std::mutex> lock(mtx);
    listeners[type].push_back(std::move(callback));
}

void EventManager::unregisterListener(EventType type) {
    std::lock_guard<std::mutex> lock(mtx);
    listeners.erase(type);
}

//----------------------------------------------
// Raising
//----------------------------------------------
void EventManager::raiseEvent(const Event& ev) {
    std::lock_guard<std::mutex> lock(mtx);
    queue.push(ev);
}

void EventManager::raiseEventFromNetwork(const std::string& serialized) {
    try {
        Event ev = Event::deserialize(serialized);
        raiseEvent(ev);
    } catch (const std::exception& e) {
        std::cerr << "Failed to deserialize event: " << e.what() << "\n";
    }
}

//----------------------------------------------
// Handling (dispatch in priority/time order)
//----------------------------------------------
void EventManager::dispatchEvents() {
    std::lock_guard<std::mutex> lock(mtx);
    while (!queue.empty()) {
        Event ev = queue.top();
        queue.pop();
        
        auto it = listeners.find(ev.type);
        if (it != listeners.end()) {
            for (auto& fn : it->second) {
                fn(ev);
            }
        }
    }
}

void EventManager::dispatchEvents(int maxCount) {
    std::lock_guard<std::mutex> lock(mtx);
    int processed = 0;
    
    while (!queue.empty() && processed < maxCount) {
        Event ev = queue.top();
        queue.pop();
        
        auto it = listeners.find(ev.type);
        if (it != listeners.end()) {
            for (auto& fn : it->second) {
                fn(ev);
            }
        }
        processed++;
    }
}

void EventManager::clearQueue() {
    std::lock_guard<std::mutex> lock(mtx);
    while (!queue.empty()) queue.pop();
}

size_t EventManager::pendingCount() const {
    std::lock_guard<std::mutex> lock(mtx);
    return queue.size();
}

//----------------------------------------------
// Factory Helpers (auto-timestamp using Timeline)
//----------------------------------------------
namespace Events {

Event Collision(const std::string& objA, const std::string& objB,
                Timeline* tl, int priority) {
    Event e(EventType::Collision, tl ? tl->time() : 0.0, priority);
    e.payload["A"] = objA;
    e.payload["B"] = objB;
    return e;
}

Event Death(const std::string& who, Timeline* tl, int priority) {
    Event e(EventType::Death, tl ? tl->time() : 0.0, priority);
    e.payload["entity"] = who;
    return e;
}

Event Spawn(const std::string& who, float x, float y,
            Timeline* tl, int priority) {
    Event e(EventType::Spawn, tl ? tl->time() : 0.0, priority);
    e.payload["entity"] = who;
    e.payload["x"] = x;
    e.payload["y"] = y;
    return e;
}

Event Input(const std::string& key, bool pressed,
            Timeline* tl, int priority) {
    Event e(EventType::Input, tl ? tl->time() : 0.0, priority);
    e.payload["key"] = key;
    e.payload["pressed"] = pressed;
    return e;
}

} // namespace Events
} // namespace Engine
