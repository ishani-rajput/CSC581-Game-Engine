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

enum class EventType {
    Collision,
    Death,
    Spawn,
    Input,
    InputChord,      // NEW: For chord (simultaneous key press) events
    ReplayStart,
    ReplayStop,
    ReplayPlay,
    Custom
};

using EventValue = std::variant<int, float, bool, std::string>;

struct Event {
    EventType type;
    double timestamp;     
    int priority;         
    std::unordered_map<std::string, EventValue> payload;

    Event(EventType t, double ts, int p)
        : type(t), timestamp(ts), priority(p) {}
    Event() : type(EventType::Custom), timestamp(0), priority(0) {}

    std::string serialize() const;
    static Event deserialize(const std::string& data);
};

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
    void dispatchEvents(int maxCount);  

    void clearQueue();
    size_t pendingCount() const;

    void startRecording();
    void stopRecording();
    void playReplay();
    bool isRecording() const { return recording; }
    bool isReplaying() const { return replaying; }

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

    std::vector<Event> replayBuffer;
    bool recording = false;
    bool replaying = false;
    double replayStartTime = 0.0;
    double replayElapsed = 0.0;
    double recordingStartTime = 0.0;
    size_t replayPlaybackIndex = 0;
};

namespace Events {
    Event Collision(const std::string& objA, const std::string& objB,
                    Timeline* tl, int priority = 1);
    Event Death(const std::string& who, Timeline* tl, int priority = 2);
    Event Spawn(const std::string& who, float x, float y,
                Timeline* tl, int priority = 3);
    Event Input(const std::string& key, bool pressed,
                Timeline* tl, int priority = 4);
    Event InputChord(const std::string& chordName, 
                     Timeline* tl, int priority = 4);

    Event ReplayStart(Timeline* tl);
    Event ReplayStop(Timeline* tl);
    Event ReplayPlay(Timeline* tl);
}

} 
