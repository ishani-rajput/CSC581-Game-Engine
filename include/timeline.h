#pragma once
#include <SDL3/SDL.h>
#include <cstdint>

class Timeline {
public:
    Timeline();

    void anchorToRealTime();

    void anchorTo(Timeline* parent);

    double tick();

    void setScale(double s);      
    double scale() const;

    void pause(bool p);
    void togglePause();
    bool isPaused() const;

    void setTicSeconds(double seconds); 
    double ticSeconds() const;

    double time() const;

    void reset();

private:
    double    m_scale = 1.0;
    bool      m_paused = false;
    double    m_ticSeconds = 0.0; 

    Timeline* m_parent = nullptr;

    uint64_t  m_prevCounter = 0;
    double    m_secondsPerCount = 0.0;

    double    m_gameTime = 0.0;

    double    m_accum = 0.0;

    double    m_parentLastTime = 0.0;

    double sampleDeltaUnscaled();

    void initializePerformanceCounter();
};