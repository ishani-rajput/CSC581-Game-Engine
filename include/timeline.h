#pragma once
#include <SDL3/SDL.h>
#include <cstdint>

// A game-time timeline that can be paused, scaled, and anchored to real time
// or to another timeline (parent).
class Timeline {
public:
    Timeline();

    // Call once at startup; anchors this timeline to real-time.
    void anchorToRealTime();

    // Anchor to another timeline (e.g., to build local time relative to server time).
    void anchorTo(Timeline* parent);

    // Advance the timeline and return elapsed *scaled* seconds since last tick.
    // If paused, returns 0. Call once per frame.
    double tick();

    // Control
    void setScale(double s);       // e.g. 0.5, 1.0, 2.0
    double scale() const;

    void pause(bool p);
    void togglePause();
    bool isPaused() const;

    // Optional: fixed tic size for deterministic stepping (0 = variable step).
    void setTicSeconds(double seconds); // e.g., 1.0/120.0
    double ticSeconds() const;

    // Cumulative game-time (scaled, excludes paused time), in seconds.
    double time() const;

    // Reset timeline to current state (useful when anchoring)
    void reset();

private:
    double    m_scale = 1.0;
    bool      m_paused = false;
    double    m_ticSeconds = 0.0; // 0 => variable dt

    Timeline* m_parent = nullptr;

    // High-res timers (only used when no parent)
    uint64_t  m_prevCounter = 0;
    double    m_secondsPerCount = 0.0;

    // Accumulated scaled game-time
    double    m_gameTime = 0.0;

    // For fixed-step option
    double    m_accum = 0.0;

    // Parent tracking (only used when parent exists)
    double    m_parentLastTime = 0.0;

    // Get unscaled delta seconds since last tick
    double sampleDeltaUnscaled();
    
    // Initialize performance counter system
    void initializePerformanceCounter();
};