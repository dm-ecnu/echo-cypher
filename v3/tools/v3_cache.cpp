// v3_cache: build or validate the shared read-only graph cache next to a sidecar.
//
//   v3_cache --sidecar DIR          build graph.cache.bin / graph.cache.meta.json
//   v3_cache --sidecar DIR --check  load through the cache (meta + source validation)
//
// The cache stores the graph arrays that the old loader copied privately in every process:
// vertices/edges columns, all-CSR, per-type adjacency, label lists, and the id index.  The
// property file stays in its existing per-column MAP_SHARED mapping.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "v3_graph.h"

int main(int argc, char** argv) {
    std::string sidecar;
    bool check = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--sidecar" && i + 1 < argc) sidecar = argv[++i];
        else if (a == "--check") check = true;
    }
    if (sidecar.empty()) {
        std::fprintf(stderr, "usage: v3_cache --sidecar DIR [--check]\n");
        return 2;
    }
    v3::GraphStore g;
    std::string err;
    if (check) {
        if (!g.load(sidecar, &err)) {
            std::fprintf(stderr, "cache check failed: %s\n", err.c_str());
            return 1;
        }
    } else {
        ::setenv("V3_GRAPH_CACHE_OFF", "1", 1);
        if (!g.load(sidecar, &err)) {
            std::fprintf(stderr, "legacy load failed: %s\n", err.c_str());
            return 1;
        }
        // The old loader materialises a type view on first use; the cache stores all of them.
        for (size_t t = 0; t < g.schema().types.size(); ++t) g.typed((uint32_t)t);
        if (!g.write_cache(sidecar, &err)) {
            std::fprintf(stderr, "cache write failed: %s\n", err.c_str());
            return 1;
        }
    }
    std::printf("%s vertices=%u rels=%llu edges=%llu labels=%zu types=%zu\n",
                check ? "cache-ok" : "cache-built", g.num_vertices(),
                (unsigned long long)g.num_rels(), (unsigned long long)g.num_edges(),
                g.schema().labels.size(), g.schema().types.size());
    return 0;
}
