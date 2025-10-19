#include "perf.h"
#include <zmq.h>
#include <chrono>
#include <sstream>
#include <vector>
#include <random>

namespace Engine {

static void* new_sock(void* ctx, int type, const char* ep, bool bindIt) {
    void* s = zmq_socket(ctx, type);
    if (bindIt) zmq_bind(s, ep); else zmq_connect(s, ep);
    int hwm = 100000;
    zmq_setsockopt(s, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    zmq_setsockopt(s, ZMQ_RCVHWM, &hwm, sizeof(hwm));
    int timeout = 1;
    zmq_setsockopt(s, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    return s;
}

PerfResult PerfSuite::RunFullState(const PerfConfig& C) {
    PerfResult r;
    void* ctx = zmq_ctx_new();
    const char* ep = "inproc://perf_full";
    void* pub = new_sock(ctx, ZMQ_PUB, ep, true);
    std::vector<void*> subs(C.clients);
    for (int i=0;i<C.clients;i++) {
        subs[i] = new_sock(ctx, ZMQ_SUB, ep, false);
        zmq_setsockopt(subs[i], ZMQ_SUBSCRIBE, "", 0);
    }

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> U(0,1920);

    auto t0 = std::chrono::high_resolution_clock::now();

    for (int k=0;k<C.iterations;k++) {
        std::ostringstream oss;
        oss << "PLAYER_BATCH " << C.objects;
        for (int j=0;j<C.objects;j++) {
            float x=U(rng), y=U(rng);
            oss << " id" << j << " " << x << " " << y << " 0 1.0";
        }
        std::string s = oss.str();
        zmq_send(pub, s.c_str(), (int)s.size(), 0);
    }

    char buf[1<<14];
    for (int i=0;i<C.clients;i++) {
        int cnt=0;
        while (cnt < C.iterations) {
            int n = zmq_recv(subs[i], buf, sizeof(buf), 0);
            if (n > 0) cnt++;
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    r.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    zmq_close(pub);
    for (auto s: subs) zmq_close(s);
    zmq_ctx_term(ctx);
    return r;
}

PerfResult PerfSuite::RunInputDelta(const PerfConfig& C) {
    PerfResult r;
    void* ctx = zmq_ctx_new();
    const char* ep = "inproc://perf_input";
    void* pub = new_sock(ctx, ZMQ_PUB, ep, true);
    std::vector<void*> subs(C.clients);
    for (int i=0;i<C.clients;i++) {
        subs[i] = new_sock(ctx, ZMQ_SUB, ep, false);
        zmq_setsockopt(subs[i], ZMQ_SUBSCRIBE, "", 0);
    }

    std::mt19937 rng(7);
    std::uniform_int_distribution<int> B(0,1);

    auto t0 = std::chrono::high_resolution_clock::now();

    for (int k=0;k<C.iterations;k++) {
        std::ostringstream oss;
        oss << "INPUT_BATCH " << C.objects;
        for (int j=0;j<C.objects;j++) {
            int L=B(rng), R=B(rng), J=B(rng);
            oss << " id" << j << " " << L << " " << R << " " << J << " 0 0 " << k;
        }
        std::string s = oss.str();
        zmq_send(pub, s.c_str(), (int)s.size(), 0);
    }

    char buf[1<<14];
    for (int i=0;i<C.clients;i++) {
        int cnt=0;
        while (cnt < C.iterations) {
            int n = zmq_recv(subs[i], buf, sizeof(buf), 0);
            if (n > 0) cnt++;
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    r.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    zmq_close(pub);
    for (auto s: subs) zmq_close(s);
    zmq_ctx_term(ctx);
    return r;
}

} // namespace Engine
