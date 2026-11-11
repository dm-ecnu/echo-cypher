// ECHO (ICDE 2027): common types and utilities shared by every method.
// All methods run on the same vendored SubgraphMatching base (vendor/SubgraphMatching,
// RapidsAtHKUST/SubgraphMatching @ bcaa935) and read the same native graph / workload files.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "configuration/types.h"
#include "graph/graph.h"

namespace echo {

using Clock = std::chrono::steady_clock;

// Wall-clock stopwatch.
class Stopwatch {
public:
    Stopwatch() : start_(Clock::now()) {}
    void reset() { start_ = Clock::now(); }
    double seconds() const { return std::chrono::duration<double>(Clock::now() - start_).count(); }
private:
    Clock::time_point start_;
};

constexpr uint32_t kWordBits = 64;

// LFTJ hook state (defined in echo_dpiso.cpp, patched into the base's EvaluateQuery.cpp).
// When g_lfrt_abort is set, LFTJ stops once g_lfrt_deadline passes.
extern std::atomic<bool>* g_lfrt_abort;
extern Clock::time_point g_lfrt_deadline;
extern uint32_t* g_lfrt_embedding;

inline uint32_t words_needed(uint32_t n) { return (n + kWordBits - 1) / kWordBits; }
inline void set_bit(std::vector<uint64_t>& w, uint32_t i) { w[i >> 6] |= 1ULL << (i & 63); }
inline bool test_bit(const std::vector<uint64_t>& w, uint32_t i) { return (w[i >> 6] >> (i & 63)) & 1ULL; }
inline void and_into(std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
    for (size_t k = 0; k < a.size(); ++k) a[k] &= b[k];
}
inline bool empty_words(const std::vector<uint64_t>& a) {
    for (uint64_t x : a) if (x) return false;
    return true;
}
inline uint32_t popcount_words(const uint64_t* w, uint32_t nwords) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < nwords; ++i) c += (uint32_t)__builtin_popcountll(w[i]);
    return c;
}

// Predicate domains are immutable, loaded once with the workload. ID zero denotes
// unconstrained input. The handle uses QueryGraph's former alignment padding, so
// legacy workloads retain their allocation sizes and cold-start heap layout.
struct PredicateDomain {
    std::vector<std::vector<uint32_t>> allowed;
    std::vector<char> constrained;
    bool permits(uint32_t u, uint32_t v) const {
        return !constrained.at(u) || std::binary_search(allowed.at(u).begin(), allowed.at(u).end(), v);
    }
};
const PredicateDomain* predicate_domain(uint32_t id);
uint32_t store_predicate_domain(PredicateDomain domain);

struct QueryGraph {
    uint32_t n = 0;
    uint32_t domain_id = 0;
    std::vector<uint32_t> label;
    std::vector<std::vector<uint32_t>> adj;
    const PredicateDomain* predicates() const { return domain_id ? predicate_domain(domain_id) : nullptr; }
    bool permits(uint32_t u, uint32_t v) const { return !domain_id || predicate_domain(domain_id)->permits(u,v); }
};

inline QueryGraph read_query(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("cannot open query file " + path);
    QueryGraph q;
    std::string tag;
    while (in >> tag) {
        if (tag == "t") {
            uint32_t m;
            in >> q.n >> m;
            q.label.assign(q.n, 0);
            q.adj.assign(q.n, {});
        } else if (tag == "v") {
            uint32_t id, lab, deg;
            in >> id >> lab >> deg;
            if (id >= q.n) throw std::runtime_error("vertex id out of range in " + path);
            q.label[id] = lab;
        } else if (tag == "e") {
            uint32_t a, b;
            in >> a >> b;
            if (a >= q.n || b >= q.n) throw std::runtime_error("edge id out of range in " + path);
            q.adj[a].push_back(b);
            q.adj[b].push_back(a);
        }
    }
    for (auto& v : q.adj) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    return q;
}

// A query graph as a base Graph object (one vertex per query vertex, label preserved).
Graph* to_base_query(const QueryGraph& q);

// Workload layout: <dir>/batch-<BB>/q<idx:03>.graph
struct Workload {
    std::string dir;
    uint32_t queries = 0;
    uint32_t batch_size = 30;
    std::vector<uint32_t> query_batches, starts, sizes;
    std::vector<QueryGraph> semantic_queries;
    uint32_t batch_start(uint32_t b) const { return starts.empty() ? b * batch_size : starts.at(b); }
    uint32_t batch_count(uint32_t b) const { return sizes.empty() ? std::min(batch_size, queries-batch_start(b)) : sizes.at(b); }
    QueryGraph read(uint32_t i) const { return semantic_queries.empty() ? read_query(query_path(i)) : semantic_queries.at(i); }
    std::string query_path(uint32_t index) const;
};

Workload read_workload_manifest(const std::string& dir);
Graph* load_data_graph(const std::string& path);

// KEY=VALUE lines printed between markers on stdout.
class Stats {
public:
    explicit Stats(const std::string& method) : method_(method) {}
    void set(const std::string& k, double v) { out_ << k << "=" << v << "\n"; }
    void seti(const std::string& k, long long v) { out_ << k << "=" << v << "\n"; }
    void sets(const std::string& k, const std::string& v) { out_ << k << "=" << v << "\n"; }
    void flush() {
        std::cout << "ECHO_STATS_BEGIN\nmethod=" << method_ << "\n" << out_.str() << "ECHO_STATS_END\n"
                  << std::flush;
    }
private:
    std::string method_;
    std::ostringstream out_;
};

struct Args {
    std::vector<std::string> positional;
    std::vector<std::pair<std::string, std::string>> options;
    std::string get(const std::string& k, const std::string& dflt = "") const {
        for (auto& kv : options)
            if (kv.first == k) return kv.second;
        return dflt;
    }
    long long geti(const std::string& k, long long dflt) const {
        std::string v = get(k);
        return v.empty() ? dflt : std::strtoll(v.c_str(), nullptr, 10);
    }
    double getd(const std::string& k, double dflt) const {
        std::string v = get(k);
        return v.empty() ? dflt : std::strtod(v.c_str(), nullptr);
    }
};

inline Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.size() > 2 && s[0] == '-' && s[1] == '-') {
            size_t eq = s.find('=');
            if (eq != std::string::npos) {
                a.options.emplace_back(s.substr(2, eq - 2), s.substr(eq + 1));
            } else if (i + 1 < argc && argv[i + 1][0] != '-') {
                a.options.emplace_back(s.substr(2), argv[++i]);
            } else {
                a.options.emplace_back(s.substr(2), "1");  // boolean flag
            }
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

// Resident-set high-water mark of this process (KiB) from /proc/self/status.
long long peak_rss_kb();

}  // namespace echo
