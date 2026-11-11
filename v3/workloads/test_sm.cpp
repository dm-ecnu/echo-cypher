// smoke test: base Graph + dpiso_run on a tiny graph
#include <cstdio>
#include <vector>
#include "echo_dpiso.h"

int main() {
    FILE* f = std::fopen("/dev/shm/sm.graph", "w");
    // 0(A)-1(B), 1(B)-2(A), 2(A)-0(B)
    std::fprintf(f, "t 3 3\nv 0 10000 2\nv 1 10001 2\nv 2 10000 2\ne 0 1\ne 1 2\ne 0 2\n");
    std::fclose(f);
    Graph* g = new Graph(false);
    g->loadGraphFromFile("/dev/shm/sm.graph");
    std::fprintf(stderr, "loaded |V|=%u |E|=%u\n", g->getVerticesCount(), g->getEdgesCount());
    FILE* qf = std::fopen("/dev/shm/sm.q", "w");
    std::fprintf(qf, "t 2 1\nv 0 10000 1\nv 1 10001 1\ne 0 1\n");
    std::fclose(qf);
    Graph* q = new Graph(false);
    q->loadGraphFromFile("/dev/shm/sm.q");
    std::vector<std::vector<uint32_t>> sols;
    echo::LfrtControl ctl;
    echo::PerQueryResult pr = echo::dpiso_run(g, q, &sols, &ctl);
    std::fprintf(stderr, "ok=%d emb=%zu\n", (int)pr.ok, sols.size());
    for (auto& s : sols) {
        for (auto x : s) std::fprintf(stderr, "%u ", x);
        std::fprintf(stderr, "\n");
    }
    return 0;
}
