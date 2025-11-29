#include "input.h"

const bool* Input::s_state = nullptr;
int Input::s_len = 0;
std::unordered_map<std::string, InputChord> Input::s_chords;
std::unordered_map<std::string, bool> Input::s_chordPrevState;

void Input::poll() {
    s_state = SDL_GetKeyboardState(&s_len);
    
    for (auto& [name, _] : s_chords) {
        s_chordPrevState[name] = isChordActive(name);
    }
}

bool Input::isKeyPressed(SDL_Scancode sc) {
    return (s_state && sc < s_len) ? s_state[sc] : false;
}

void Input::registerChord(const InputChord& chord) {
    s_chords.insert_or_assign(chord.name, chord);
    s_chordPrevState[chord.name] = false;
}

bool Input::checkChord(const InputChord& chord) {
    if (!s_state) return false;
    
    // All keys in the chord must be pressed
    for (SDL_Scancode key : chord.keys) {
        if (!isKeyPressed(key)) {
            return false;
        }
    }
    return true;
}

bool Input::isChordActive(const std::string& chordName) {
    auto it = s_chords.find(chordName);
    if (it == s_chords.end()) return false;
    
    return checkChord(it->second);
}

bool Input::isChordJustPressed(const std::string& chordName) {
    auto prevIt = s_chordPrevState.find(chordName);
    bool wasPressedBefore = (prevIt != s_chordPrevState.end()) ? prevIt->second : false;
    
    return isChordActive(chordName) && !wasPressedBefore;
}

std::vector<std::string> Input::getActiveChords() {
    std::vector<std::string> active;
    for (const auto& [name, chord] : s_chords) {
        if (checkChord(chord)) {
            active.push_back(name);
        }
    }
    return active;
}

std::vector<std::string> Input::getAllChordNames() {
    std::vector<std::string> names;
    names.reserve(s_chords.size());
    for (const auto& [name, _] : s_chords) {
        names.push_back(name);
    }
    return names;
}
