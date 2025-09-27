#include "timeline.h"

Timeline::Timeline() {
    uint64_t freq = SDL_GetPerformanceFrequency();
    m_secondsPerCount = (freq > 0) ? (1.0 / static_cast<double>(freq)) : (1.0 / 1000.0);
}

void Timeline::anchorToRealTime() {
    m_parent = nullptr;
    m_prevCounter = SDL_GetPerformanceCounter();
}

void Timeline::anchorTo(Timeline* parent) {
    m_parent = parent;
    m_prevCounter = SDL_GetPerformanceCounter();
}

double Timeline::sampleDeltaUnscaled() {
    if (m_parent) {
        // If anchored to parent timeline, use parent’s tick-less sample by reading real-time
        // then multiply by parent’s scale state indirectly via parent->tick() would double-advance it,
        // so we just use real time for sampling and let this timeline’s own scale apply.
        // (If you need true hierarchical scaling, you can expose parent’s raw delta.)
    }
    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t counts = now - m_prevCounter;
    m_prevCounter = now;
    return static_cast<double>(counts) * m_secondsPerCount; // unscaled real seconds
}

double Timeline::tick() {
    if (m_prevCounter == 0) anchorToRealTime();

    if (m_paused) return 0.0;

    double unscaled = sampleDeltaUnscaled();
    double scaled = unscaled * m_scale;

    if (m_ticSeconds <= 0.0) {
        // Variable step
        m_gameTime += scaled;
        return scaled;
    } else {
        // Fixed step accumulation
        m_accum += scaled;
        double dtOut = 0.0;
        if (m_accum >= m_ticSeconds) {
            dtOut = m_ticSeconds;
            m_accum -= m_ticSeconds;
            m_gameTime += dtOut;
        }
        return dtOut; // may be 0 if not enough accumulated
    }
}

void Timeline::setScale(double s) {
    if (s < 0.01) s = 0.01;
    if (s > 8.0)  s = 8.0;
    m_scale = s;
}

double Timeline::scale() const { return m_scale; }

void Timeline::pause(bool p) { m_paused = p; }
void Timeline::togglePause() { m_paused = !m_paused; }
bool Timeline::isPaused() const { return m_paused; }

void Timeline::setTicSeconds(double seconds) { m_ticSeconds = (seconds < 0.0 ? 0.0 : seconds); }
double Timeline::ticSeconds() const { return m_ticSeconds; }

double Timeline::time() const { return m_gameTime; }
