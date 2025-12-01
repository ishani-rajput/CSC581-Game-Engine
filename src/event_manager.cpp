#include "event_manager.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <utility>

namespace Engine {

std::string Event::serialize() const {
    std::ostringstream oss;
    oss << static_cast<int>(type) << "|" 
        << timestamp << "|" 
        << priority;
    for (const auto& [key, value] : payload) {
        oss << "|" << key << ":";
        if (std::holds_alternative<int>(value))
            oss << "i:" << std::get<int>(value);
        else if (std::holds_alternative<float>(value))
            oss << "f:" << std::get<float>(value);
        else if (std::holds_alternative<bool>(value))
            oss << "b:" << (std::get<bool>(value) ? "1" : "0");
        else if (std::holds_alternative<std::string>(value))
            oss << "s:" << std::get<std::string>(value);
    }
    return oss.str();
}

Event Event::deserialize(const std::string& data) {
    std::istringstream iss(data);
    std::string token;

    std::getline(iss, token, '|');
    EventType type = static_cast<EventType>(std::stoi(token));

    std::getline(iss, token, '|');
    double timestamp = std::stod(token);

    std::getline(iss, token, '|');
    int priority = std::stoi(token);

    Event ev(type, timestamp, priority);

    while (std::getline(iss, token, '|')) {
        size_t c1 = token.find(':'), c2 = token.find(':', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) continue;
        std::string key = token.substr(0, c1);
        char typeChar = token[c1 + 1];
        std::string val = token.substr(c2 + 1);
        if (typeChar == 'i') ev.payload[key] = std::stoi(val);
        else if (typeChar == 'f') ev.payload[key] = std::stof(val);
        else if (typeChar == 'b') ev.payload[key] = (val == "1");
        else if (typeChar == 's') ev.payload[key] = val;
    }
    return ev;
}

EventManager::EventManager(Timeline* tl) : timeline(tl) {}

Timeline* EventManager::getTimeline() const { return timeline; }

void EventManager::registerListener(EventType type, Listener callback) {
    std::lock_guard<std::mutex> lock(mtx);
    listeners[type].push_back(std::move(callback));
}

void EventManager::unregisterListener(EventType type) {
    std::lock_guard<std::mutex> lock(mtx);
    listeners.erase(type);
}

void EventManager::raiseEvent(const Event& ev) {
    std::lock_guard<std::mutex> lock(mtx);
    queue.push(ev);

    if (recording && ev.type != EventType::ReplayStart &&
        ev.type != EventType::ReplayStop && ev.type != EventType::ReplayPlay) {
        replayBuffer.push_back(ev);
    }
}

void EventManager::raiseEventFromNetwork(const std::string& serialized) {
    try {
        Event ev = Event::deserialize(serialized);
        raiseEvent(ev);
    } catch (const std::exception& e) {
        std::cerr << "Failed to deserialize event: " << e.what() << "\n";
    }
}

void EventManager::dispatchEvents() {
    using Task = std::pair<Event, std::vector<Listener>>;
    std::vector<Task> tasks;
    bool replayJustFinished = false;

    {
        std::lock_guard<std::mutex> lock(mtx);

        while (!queue.empty()) {
            Event ev = queue.top();
            queue.pop();
            auto it = listeners.find(ev.type);
            if (it != listeners.end()) {
                tasks.emplace_back(ev, it->second);
            }
        }

        if (replaying && timeline) {
            replayElapsed = timeline->time() - replayStartTime;
            while (replayPlaybackIndex < replayBuffer.size()) {
                const Event& stored = replayBuffer[replayPlaybackIndex];
                double eventOffset = stored.timestamp - recordingStartTime;
                if (eventOffset < 0.0) eventOffset = 0.0;
                if (eventOffset <= replayElapsed) {
                    auto jt = listeners.find(stored.type);
                    if (jt != listeners.end()) {
                        Event playbackEvent = stored;
                        playbackEvent.timestamp = replayStartTime + eventOffset;
                        tasks.emplace_back(playbackEvent, jt->second);
                    }
                    replayPlaybackIndex++;
                } else {
                    break;
                }
            }
            if (replayPlaybackIndex >= replayBuffer.size()) {
                replaying = false;
                replayJustFinished = true;
            }
        }
    }

    for (auto& [event, listenersCopy] : tasks) {
        for (auto& fn : listenersCopy) {
            fn(event);
        }
    }

    if (replayJustFinished) {
        std::cout << "[REPLAY] Playback finished\n";
    }
}

void EventManager::dispatchEvents(int maxCount) {
    using Task = std::pair<Event, std::vector<Listener>>;
    std::vector<Task> tasks;

    {
        std::lock_guard<std::mutex> lock(mtx);
        int processed = 0;
        while (!queue.empty() && processed < maxCount) {
            Event ev = queue.top();
            queue.pop();
            auto it = listeners.find(ev.type);
            if (it != listeners.end()) {
                tasks.emplace_back(ev, it->second);
            }
            processed++;
        }
    }

    for (auto& [event, listenersCopy] : tasks) {
        for (auto& fn : listenersCopy) {
            fn(event);
        }
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

void EventManager::startRecording() {
    std::lock_guard<std::mutex> lock(mtx);
    replayBuffer.clear();
    recording = true;
    replaying = false;
    recordingStartTime = timeline ? timeline->time() : 0.0;
    replayPlaybackIndex = 0;
    std::cout << "[REPLAY] Recording started\n";
}

void EventManager::stopRecording() {
    std::lock_guard<std::mutex> lock(mtx);
    recording = false;
    std::cout << "[REPLAY] Recording stopped (" << replayBuffer.size() << " events)\n";
}

void EventManager::playReplay() {
    std::lock_guard<std::mutex> lock(mtx);
    if (replayBuffer.empty()) {
        std::cout << "[REPLAY] No events to replay\n";
        return;
    }
    replaying = true;
    recording = false;
    replayStartTime = timeline ? timeline->time() : 0.0;
    replayElapsed = 0.0;
    replayPlaybackIndex = 0;
    std::cout << "[REPLAY] Playing replay with " << replayBuffer.size() << " events\n";
}

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

Event InputChord(const std::string& chordName,
                 Timeline* tl, int priority) {
    Event e(EventType::InputChord, tl ? tl->time() : 0.0, priority);
    e.payload["chord"] = chordName;
    return e;
}

Event ReplayStart(Timeline* tl) {
    Event e(EventType::ReplayStart, tl ? tl->time() : 0.0, 0);
    e.payload["state"] = std::string("start");
    return e;
}
Event ReplayStop(Timeline* tl) {
    Event e(EventType::ReplayStop, tl ? tl->time() : 0.0, 0);
    e.payload["state"] = std::string("stop");
    return e;
}
Event ReplayPlay(Timeline* tl) {
    Event e(EventType::ReplayPlay, tl ? tl->time() : 0.0, 0);
    e.payload["state"] = std::string("play");
    return e;
}

} 
} 