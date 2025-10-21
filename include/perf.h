#pragma once
#include <cstdint>

namespace Engine {

struct PerfResult { double ms = 0.0; };

struct PerfConfig {
    int clients = 2;
    int objects = 100;
    int iterations = 100000;
};

class PerfSuite {
public:
    static PerfResult RunFullState(const PerfConfig& cfg);
    static PerfResult RunInputDelta(const PerfConfig& cfg);
};

} 
