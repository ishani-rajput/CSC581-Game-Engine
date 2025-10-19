#include "perf.h"
#include <iostream>
#include <vector>
#include <cstdlib>

static double mean(const std::vector<double>& v){ double s=0; for(double x: v) s+=x; return s/v.size(); }
static double var (const std::vector<double>& v){ double m=mean(v); double s=0; for(double x: v) s+=(x-m)*(x-m); return s/(v.size()>1? v.size()-1:1); }

int main(int argc, char** argv) {
    Engine::PerfConfig c;
    if (argc>1) c.clients    = std::atoi(argv[1]);
    if (argc>2) c.objects    = std::atoi(argv[2]);
    if (argc>3) c.iterations = std::atoi(argv[3]);

    // run 5x to get stats
    std::vector<double> a, b;
    for (int i=0;i<5;i++) a.push_back(Engine::PerfSuite::RunFullState(c).ms);
    for (int i=0;i<5;i++) b.push_back(Engine::PerfSuite::RunInputDelta(c).ms);

    std::cout << "Perf (clients="<<c.clients<<", objects="<<c.objects<<", iters="<<c.iterations<<")\n";
    std::cout << "  Strategy A (FullState): mean=" << mean(a) << " ms  var=" << var(a) << "\n";
    std::cout << "  Strategy B (InputDelta): mean=" << mean(b) << " ms  var=" << var(b) << "\n";
    return 0;
}