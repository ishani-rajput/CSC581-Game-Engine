#pragma once
#include <SDL3/SDL.h>
#include <vector>
#include <unordered_map>
#include <functional>
#include <string>

struct InputChord {
    std::vector<SDL_Scancode> keys;
    std::string name;
    
    InputChord(std::string n, std::vector<SDL_Scancode> k)
        : name(std::move(n)), keys(std::move(k)) {}
    
    bool operator==(const InputChord& other) const {
        return name == other.name;
    }
};

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
    
    static void registerChord(const InputChord& chord);
    static bool isChordActive(const std::string& chordName);
    static std::vector<std::string> getActiveChords();
    
    static bool isChordJustPressed(const std::string& chordName);
    
    static std::vector<std::string> getAllChordNames();

private:
    static const bool* s_state; 
    static int s_len;
    
    static std::unordered_map<std::string, InputChord> s_chords;
    static std::unordered_map<std::string, bool> s_chordPrevState;
    
    static bool checkChord(const InputChord& chord);
};
