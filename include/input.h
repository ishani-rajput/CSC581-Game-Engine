#pragma once
#include <SDL3/SDL.h>
#include <vector>
#include <unordered_map>
#include <functional>
#include <string>

// Represents a chord: multiple keys that must be pressed simultaneously
struct InputChord {
    std::vector<SDL_Scancode> keys;
    std::string name;
    
    InputChord(std::string n, std::vector<SDL_Scancode> k)
        : name(std::move(n)), keys(std::move(k)) {}
    
    bool operator==(const InputChord& other) const {
        return name == other.name;
    }
};

// Hash function for InputChord to use in unordered_map
namespace std {
    template<>
    struct hash<InputChord> {
        size_t operator()(const InputChord& chord) const {
            return hash<string>()(chord.name);
        }
    };
}

class Input {
public:
    static void poll(); 
    static bool isKeyPressed(SDL_Scancode sc);
    
    // Chord detection
    static void registerChord(const InputChord& chord);
    static bool isChordActive(const std::string& chordName);
    static std::vector<std::string> getActiveChords();
    
    // Check if a chord was just activated this frame (wasn't active last frame)
    static bool isChordJustPressed(const std::string& chordName);
    
    // Get all registered chord names
    static std::vector<std::string> getAllChordNames();

private:
    static const bool* s_state; 
    static int s_len;
    
    // Chord management
    static std::unordered_map<std::string, InputChord> s_chords;
    static std::unordered_map<std::string, bool> s_chordPrevState;
    
    static bool checkChord(const InputChord& chord);
};