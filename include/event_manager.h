#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <queue>
#include <functional>
#include <mutex>
#include <variant>
#include "timeline.h"

namespace Engine {

//----------------------------------------------
// 1. EVENT REPRESENTATION
//----------------------------------------------
enum class EventType { Collision, Death, Spawn, Input, Custom };

// Strongly typed event data
using EventValue = std::variant<int, float, bool, std::string>;

struct Event {
    EventType type;
    double timestamp;     // Taken from Timeline
    int priority;         // Lower = higher priority
    std::unordered_map<std::string, EventValue> payload;

    Event(EventType t, double ts, int p)
        : type(t), timestamp(ts), priority(p) {}

    // Serialization for networking (Part 1.2)
    std::string serialize() const;
    static Event deserialize(const std::string& data);
};

//----------------------------------------------
// 2. EVENT MANAGER INTERFACE
//----------------------------------------------
class EventManager {
public:
    using Listener = std::function<void(const Event&)>;

    explicit EventManager(Timeline* tl = nullptr);

    Timeline* getTimeline() const;

    // Registration
    void registerListener(EventType type, Listener callback);
    void unregisterListener(EventType type);

    // Raising
    void raiseEvent(const Event& ev);
    void raiseEventFromNetwork(const std::string& serialized);

    // Handling
    void dispatchEvents();
    void dispatchEvents(int maxCount);  // Rate-limited dispatch

    void clearQueue();
    size_t pendingCount() const;

private:
    struct Compare {
        bool operator()(const Event& a, const Event& b) const {
            if (a.priority == b.priority)
                return a.timestamp > b.timestamp;
            return a.priority > b.priority;
        }
    };

    Timeline* timeline;
    std::unordered_map<EventType, std::vector<Listener>> listeners;
    std::priority_queue<Event, std::vector<Event>, Compare> queue;
    mutable std::mutex mtx;
};

//----------------------------------------------
// 3. FACTORY HELPERS FOR REQUIRED EVENTS
//----------------------------------------------
namespace Events {
    Event Collision(const std::string& objA, const std::string& objB,
                    Timeline* tl, int priority = 1);
    Event Death(const std::string& who, Timeline* tl, int priority = 2);
    Event Spawn(const std::string& who, float x, float y,
                Timeline* tl, int priority = 3);
    Event Input(const std::string& key, bool pressed,
                Timeline* tl, int priority = 4);
}

} // namespace Engine
