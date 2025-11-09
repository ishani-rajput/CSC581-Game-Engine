#include "timeline.h"

Timeline::Timeline() {
    initializePerformanceCounter();
}

void Timeline::initializePerformanceCounter() {
    uint64_t freq = SDL_GetPerformanceFrequency();
    m_secondsPerCount = (freq > 0) ? (1.0 / static_cast<double>(freq)) : (1.0 / 1000.0);
}

void Timeline::anchorToRealTime() {
    m_parent = nullptr;
    m_prevCounter = SDL_GetPerformanceCounter();
    m_parentLastTime = 0.0;
}

void Timeline::anchorTo(Timeline* parent) {
    if (!parent) {
        anchorToRealTime();
        return;
    }
    
    m_parent = parent;
    m_parentLastTime = parent->time(); 
    m_prevCounter = 0; 
}

void Timeline::reset() {
    m_gameTime = 0.0;
    m_accum = 0.0;
    if (m_parent) {
        m_parentLastTime = m_parent->time();
    } else {
        m_prevCounter = SDL_GetPerformanceCounter();
    }
}

double Timeline::sampleDeltaUnscaled() {
    if (m_parent) {
        double parentCurrentTime = m_parent->time();
        double parentDelta = parentCurrentTime - m_parentLastTime;
        m_parentLastTime = parentCurrentTime;
        return parentDelta;
    } else {
        if (m_prevCounter == 0) {
            m_prevCounter = SDL_GetPerformanceCounter();
            return 0.0; 
        }
        
        uint64_t now = SDL_GetPerformanceCounter();
        uint64_t counts = now - m_prevCounter;
        m_prevCounter = now;
        return static_cast<double>(counts) * m_secondsPerCount;
    }
}

double Timeline::tick() {
    if (!m_parent && m_prevCounter == 0) {
        anchorToRealTime();
        return 0.0; 
    }
    
    if (m_paused) {
        if (m_parent) {
            m_parentLastTime = m_parent->time();
        } else {
            m_prevCounter = SDL_GetPerformanceCounter();
        }
        return 0.0;
    }

    double unscaled = sampleDeltaUnscaled();
    double scaled = unscaled * m_scale;

    if (m_ticSeconds <= 0.0) {
        m_gameTime += scaled;
        return scaled;
    } else {
        m_accum += scaled;
        double dtOut = 0.0;
        
        while (m_accum >= m_ticSeconds) {
            dtOut += m_ticSeconds;
            m_accum -= m_ticSeconds;
            m_gameTime += m_ticSeconds;
        }
        
        return dtOut; 
    }
}

void Timeline::setScale(double s) {
    if (s < 0.01) s = 0.01;
    if (s > 8.0)  s = 8.0;
    m_scale = s;
}

double Timeline::scale() const { 
    return m_scale; 
}

void Timeline::pause(bool p) { 
    m_paused = p; 
}

void Timeline::togglePause() { 
    m_paused = !m_paused; 
}

bool Timeline::isPaused() const { 
    return m_paused; 
}

void Timeline::setTicSeconds(double seconds) { 
    m_ticSeconds = (seconds < 0.0 ? 0.0 : seconds); 
}

double Timeline::ticSeconds() const { 
    return m_ticSeconds; 
}

double Timeline::time() const { 
    return m_gameTime; 
}