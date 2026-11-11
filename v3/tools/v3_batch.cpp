// v3_batch: process a list of rounds in one process, loading the sidecar once.
//
//   v3_batch --sidecar DIR --list FILE --out FILE [--reps N] [--run-id ID] [--dataset NAME]
//            [--cell-cap SEC] [--base-graph PATH]
//
// Each round is evaluated by the normal executor and by --serial (one candidate per fallback),
// on the same process and in the same adjacency/page-cache state, with the method order rotated
// by repeat.  Every cell runs in a forked child with a wall-clock cap: the graph is loaded once
// (copy-on-write), a cell that exceeds the cap is killed and recorded as a timeout, and the shard
// survives it.  The output is JSONL with the fields e4_analyze.py uses.  Graph load is outside
// wall_sec (the paper's timing boundary); it is reported once in the first row as load_sec.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "v3_exec.h"
#include "v3_json.h"
#include "v3_masc_ldbc.h"
#include "v3_plan.h"
#include "v3_point.h"

#include "graph/graph.h"

using namespace v3;

// Empty: the original v3/serial pair.  Otherwise one of masc/batchenum/vlpq: the
// second method of every repeat is that port, with the unsupported candidates
// evaluated through the same serial executor as the per-candidate row.
static std::string g_opponent;
static std::vector<std::string> g_opponents;
// The v2 graph view of the same data, for the base's pure count pipeline in Fallback.
static const Graph* g_base_graph = nullptr;
// Optional per-candidate result capture for the shared-cache A/B check.  It is off in every
// benchmark run; when set, each real ECHO cell appends one line per candidate.
static std::string g_golden_path;

static double now_sec() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

struct CellRes {
    double wall = 0.0;
    uint64_t edges = 0, inter = 0, rows = 0, sol = 0;
    uint64_t mcs_edges = 0, mcs_vertices = 0, mcs_bytes = 0;
    double plan_sec = 0.0, env_sec = 0.0, fallback_sec = 0.0, mcs_build_sec = 0.0;
    uint64_t envelopes = 0, fallback = 0, departures = 0, env_blocks = 0;
    // Peak resident set of the cell's child (inherited graph pages included).  The two methods
    // run in separate children of the same parent, so this is the per-cell max RSS the table
    // reports; a killed child leaves it at 0.
    uint64_t rss_kb = 0;
    int block_envelope = 0;
    int rc = 0;
    // Opponent cells: the share of candidates the port evaluated jointly, and the
    // split of the cell's wall time between the port and the serial remainder.
    uint64_t covered = 0, attempted = 0;
    double masc_sec = 0.0;
    int dc_max = 0;
    uint64_t dc_count[64] = {0}, dc_edge[64] = {0}, dc_state[64] = {0}, dc_ledge[64] = {0};
};

// VmHWM of the calling process: the high-water mark of the whole cell (graph pages inherited
// through the fork included), read after the executor returns and before the child exits.
static uint64_t peak_rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string k;
    while (f >> k) {
        if (k == "VmHWM:") {
            uint64_t v = 0;
            f >> v;
            return v;
        }
    }
    return 0;
}

static void hash_add(uint64_t* h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) *h = (*h ^ b[i]) * 1099511628211ULL;
}

// A digest of the RETURN rows in their deterministic (sorted) order.  The A/B cache check
// compares this per candidate between the old and new binaries.
static uint64_t result_digest(const CandidateOut& cd) {
    uint64_t h = 1469598103934665603ULL;
    hash_add(&h, &cd.ncols, sizeof cd.ncols);
    for (const Value& x : cd.data) {
        const uint8_t tag = (uint8_t)((x.is_null ? 1 : 0) | (x.is_str ? 2 : 0));
        hash_add(&h, &tag, 1);
        hash_add(&h, &x.i, sizeof x.i);
        hash_add(&h, x.s.data(), x.s.size());
    }
    return h;
}

static uint64_t result_multiset(const CandidateOut& cd) {
    const size_t ncols = cd.ncols;
    if (!ncols) return 0;
    const size_t nrows = cd.data.size() / ncols;
    uint64_t sum = 0, xr = 0;
    for (size_t r = 0; r < nrows; ++r) {
        uint64_t h = 1469598103934665603ULL;
        hash_add(&h, &ncols, sizeof ncols);
        for (size_t c = 0; c < ncols; ++c) {
            const Value& x = cd.data[r * ncols + c];
            const uint8_t tag = (uint8_t)((x.is_null ? 1 : 0) | (x.is_str ? 2 : 0));
            hash_add(&h, &tag, 1);
            hash_add(&h, &x.i, sizeof x.i);
            hash_add(&h, x.s.data(), x.s.size());
        }
        sum += h;
        xr ^= h;
    }
    return (sum * 1099511628211ULL) ^ xr;
}

static void write_golden(const Round& r, const std::vector<CandidateOut>& out,
                         const RoundPlan& plan, uint64_t work, size_t round_index, int rep) {
    if (g_golden_path.empty()) return;
    std::ostringstream text;
    for (size_t i = 0; i < out.size() && i < r.queries.size(); ++i) {
        const CandidateOut& c = out[i];
        const size_t nrows = c.ncols ? c.data.size() / c.ncols : 0;
        text << "{\"round_index\":" << round_index
             << ",\"round\":\"" << esc(r.family) << "\""
             << ",\"candidate\":" << i
             << ",\"qid\":\"" << esc(r.queries[i].id) << "\""
             << ",\"ncols\":" << c.ncols << ",\"nrows\":" << nrows
             << ",\"row_digest\":\"" << std::hex << std::setw(16) << std::setfill('0')
             << result_digest(c) << "\""
             << ",\"row_multiset\":\"" << std::hex << std::setw(16) << std::setfill('0')
             << result_multiset(c) << std::dec << "\""
             << ",\"planned\":" << (c.planned ? "true" : "false")
             << ",\"aggregated\":" << (c.aggregated ? "true" : "false")
             << ",\"matchings\":" << c.matchings.size()
             << ",\"envelopes\":" << (plan.depth_chain ? 1 : plan.envelopes.size())
             << ",\"prefix_envelopes\":" << plan.prefix_envelopes.size()
             << ",\"fallback\":" << plan.fallback.size()
             << ",\"depth_chain\":" << (plan.depth_chain ? "true" : "false")
             << ",\"why\":\"" << esc(i < plan.why.size() ? plan.why[i] : std::string()) << "\""
             << ",\"env\":" << (i < plan.env_of.size() ? plan.env_of[i] : -1)
             << ",\"prefix\":" << (i < plan.prefix_of.size() ? plan.prefix_of[i] : -1)
             << ",\"work\":" << work << ",\"repeat\":" << rep << "}\n";
    }
    const std::string s = text.str();
    const int fd = ::open(g_golden_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    ::flock(fd, LOCK_EX);
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::write(fd, s.data() + off, s.size() - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    ::flock(fd, LOCK_UN);
    ::close(fd);
}

// Run one cell in a child process.  The parent waits at most `cap` seconds and kills the child;
// a timed-out cell carries rc = -1.  The graph and the round live in the parent, so the fork
// shares their pages and the child starts with the same warm adjacency state.
static CellRes run_cell(const GraphStore& g, const Round& r, bool serial, double cap,
                        bool golden = false, size_t round_index = 0, int rep = -1) {
    CellRes res;
    int fd[2];
    if (pipe(fd) != 0) { res.rc = -2; return res; }
    const double t0 = now_sec();
    const pid_t pid = fork();
    if (pid < 0) { res.rc = -2; close(fd[0]); close(fd[1]); return res; }
    if (pid == 0) {
        close(fd[0]);
        Executor::Options opt;
        opt.collect = true;
        opt.serial_only = serial;
        opt.masked_agg = true;
        opt.enforce_budget = true;
        opt.base_graph = g_base_graph;
        Executor ex(g, g.schema(), opt);
        std::vector<CandidateOut> out;
        Counters cnt;
        RoundPlan plan;
        std::string err;
        CellRes c;
        const double t1 = now_sec();
        c.rc = ex.run(r, &out, &cnt, &plan, &err) ? 0 : 1;
        const double t2 = now_sec();
        c.wall = t2 - t1;
        c.edges = cnt.edges_expanded + cnt.vl_steps + cnt.mcs_edges;
        c.inter = cnt.init_tuples + cnt.mcs_vertices +
                  (cnt.tuples > cnt.solutions ? cnt.tuples - cnt.solutions : 0);
        c.sol = cnt.solutions;
        for (const auto& x : out) c.rows += x.ncols ? (uint64_t)(x.data.size() / x.ncols) : 0;
        c.mcs_edges = cnt.mcs_edges;
        c.mcs_vertices = cnt.mcs_vertices;
        c.mcs_bytes = cnt.mcs_bytes;
        c.mcs_build_sec = cnt.mcs_build_sec;
        c.plan_sec = ex.stats.plan_sec;
        c.env_sec = ex.stats.env_sec;
        c.fallback_sec = ex.stats.fallback_sec;
        c.envelopes = plan.depth_chain ? 1
                      : plan.envelopes.size() + (ex.stats.prefix_members > 0 ? 1 : 0);
        c.fallback = plan.fallback.size();
        c.departures = ex.stats.departures;
        if (plan.depth_chain) { c.env_blocks = 1; c.block_envelope = 1; }
        for (const auto& env : plan.envelopes)
            if (env.members.size() > 1) { ++c.env_blocks; c.block_envelope = 1; }
        for (const auto& env : plan.prefix_envelopes)
            if (env.members.size() > 1) { ++c.env_blocks; c.block_envelope = 1; }
        c.rss_kb = peak_rss_kb();
        if (golden && c.rc == 0) write_golden(r, out, plan, c.edges + c.inter, round_index, rep);
        if (plan.depth_chain) {
            c.dc_max = plan.dc_max_len;
            for (int32_t d = 1; d <= plan.dc_max_len && d < 64; ++d) {
                c.dc_count[d] = plan.dc_counts.empty() ? 0 : plan.dc_counts[(size_t)d];
                c.dc_edge[d] = plan.dc_edges.empty() ? 0 : plan.dc_edges[(size_t)d];
                c.dc_state[d] = plan.dc_states.empty() ? 0 : plan.dc_states[(size_t)d];
                c.dc_ledge[d] = plan.dc_logical_edges.empty() ? 0 : plan.dc_logical_edges[(size_t)d];
            }
        }
        ssize_t w = write(fd[1], &c, sizeof c);
        (void)w;
        close(fd[1]);
        _exit(0);
    }
    close(fd[1]);
    bool timed_out = false;
    for (;;) {
        int st = 0;
        const pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0) res.rc = 0;
            else res.rc = 1;
            break;
        }
        if (w < 0) { res.rc = -2; break; }
        if (cap > 0.0 && now_sec() - t0 > cap) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            timed_out = true;
            res.rc = -1;
            break;
        }
        usleep(2000);
    }
    CellRes c;
    const ssize_t got = read(fd[0], &c, sizeof c);
    close(fd[0]);
    if (!timed_out && got == (ssize_t)sizeof c) {
        res.wall = c.wall;
        res.edges = c.edges;
        res.inter = c.inter;
        res.rows = c.rows;
        res.sol = c.sol;
        res.mcs_edges = c.mcs_edges;
        res.mcs_vertices = c.mcs_vertices;
        res.mcs_bytes = c.mcs_bytes;
        res.mcs_build_sec = c.mcs_build_sec;
        res.plan_sec = c.plan_sec;
        res.env_sec = c.env_sec;
        res.fallback_sec = c.fallback_sec;
        res.envelopes = c.envelopes;
        res.fallback = c.fallback;
        res.departures = c.departures;
        res.env_blocks = c.env_blocks;
        res.rss_kb = c.rss_kb;
        res.block_envelope = c.block_envelope;
        res.dc_max = c.dc_max;
        for (int32_t d = 0; d < 64; ++d) {
            res.dc_count[d] = c.dc_count[d];
            res.dc_edge[d] = c.dc_edge[d];
            res.dc_state[d] = c.dc_state[d];
            res.dc_ledge[d] = c.dc_ledge[d];
        }
        // Do not let a child's zeroed result overwrite a nonzero wait status: a child killed
        // by an exception before writing its struct must stay an error, never look like a
        // legitimate empty cell.
        if (res.rc == 0) res.rc = c.rc;
    } else if (timed_out) {
        res.wall = cap;
    } else if (res.rc == 0) {
        res.rc = 1;
    }
    return res;
}

// Run one opponent cell in a child.  The MASC port evaluates its supported subset and
// the serial executor evaluates the same round with those candidates removed; their
// work and wall time are added, and the peak RSS is the child's VmHWM.
static CellRes run_opp_cell(const GraphStore& g, const Round& r, double cap) {
    CellRes res;
    int fd[2];
    if (pipe(fd) != 0) { res.rc = -2; return res; }
    const double t0 = now_sec();
    const pid_t pid = fork();
    if (pid < 0) { res.rc = -2; close(fd[0]); close(fd[1]); return res; }
    if (pid == 0) {
        close(fd[0]);
        Round fb = r;
        CellRes c;
        c.attempted = r.queries.size();
        MascLdbcRound m;
        if (g_opponent == "masc") {
            // The MASC fallback is a subset of the round: the point plan and the legacy tiny
            // replay map index the full candidate list and must not be reused on the smaller
            // vector.  A non-MASC opponent keeps the whole round and its point plan, so serial
            // and ECHO execute the same cell.
            fb.queries.clear();
            fb.tiny = false;
            fb.tiny_est = 0.0;
            fb.tiny_uniq.clear();
            fb.tiny_replay.clear();
            fb.point_ready = false;
            fb.point.clear();
            fb.point_uniq.clear();
            fb.point_replay.clear();
            // The MASC cell is the whole adapter call: per-candidate shape analysis, the
            // shared adjacency scan, Env construction, projection and the destruction of the
            // adapter's intermediate matchings.  Only the result rows leave the call.  Timing
            // just MascLdbcRound::sec used to omit classification and the Env teardown, which
            // the ECHO/serial executor pays inside its own cell.
            const double masc_t0 = now_sec();
            m = masc_ldbc_run(g, r, cap);
            c.wall = now_sec() - masc_t0;
            for (size_t i = 0; i < r.queries.size(); ++i)
                if (!m.per_query[i].covered) fb.queries.push_back(r.queries[i]);
            c.covered = m.covered;
            c.masc_sec = m.sec;
            c.edges = m.edges;
            c.inter = m.tuples;
            c.sol = m.solutions;
            for (const auto& q : m.per_query)
                if (q.covered) c.rows += q.out_rows;
            // c.wall was set around the adapter call above; m.sec remains the adapter's own
            // phase ledger for diagnostics and must not overwrite the cell wall.
        } else {
            // No supported candidate: the whole round is the serial remainder.
            fb = r;
            c.covered = 0;
            c.attempted = r.queries.size();
        }
        if (!fb.queries.empty()) {
            Executor::Options opt;
            opt.collect = true;
            opt.serial_only = true;
            opt.masked_agg = true;
            opt.enforce_budget = true;
            opt.base_graph = g_base_graph;
            Executor ex(g, g.schema(), opt);
            std::vector<CandidateOut> out;
            Counters cnt;
            RoundPlan plan;
            std::string err;
            const double t1 = now_sec();
            c.rc = ex.run(fb, &out, &cnt, &plan, &err) ? 0 : 1;
            const double fb_wall = now_sec() - t1;
            c.wall += fb_wall;
            c.fallback_sec = fb_wall;
            c.edges += cnt.edges_expanded + cnt.vl_steps + cnt.mcs_edges;
            c.inter += cnt.init_tuples + cnt.mcs_vertices +
                       (cnt.tuples > cnt.solutions ? cnt.tuples - cnt.solutions : 0);
            c.sol += cnt.solutions;
            for (const auto& x : out) c.rows += x.ncols ? (uint64_t)(x.data.size() / x.ncols) : 0;
            c.mcs_edges = cnt.mcs_edges;
            c.mcs_vertices = cnt.mcs_vertices;
            c.mcs_bytes = cnt.mcs_bytes;
            c.plan_sec = ex.stats.plan_sec;
            c.env_sec = ex.stats.env_sec;
            c.mcs_build_sec = cnt.mcs_build_sec;
            c.envelopes = plan.envelopes.size() + (ex.stats.prefix_members > 0 ? 1 : 0);
            c.fallback = plan.fallback.size();
            c.departures = ex.stats.departures;
            for (const auto& env : plan.envelopes)
                if (env.members.size() > 1) { ++c.env_blocks; c.block_envelope = 1; }
            for (const auto& env : plan.prefix_envelopes)
                if (env.members.size() > 1) { ++c.env_blocks; c.block_envelope = 1; }
        } else {
            c.rc = 0;
        }
        c.rss_kb = peak_rss_kb();
        ssize_t w = write(fd[1], &c, sizeof c);
        (void)w;
        close(fd[1]);
        _exit(0);
    }
    close(fd[1]);
    bool timed_out = false;
    for (;;) {
        int st = 0;
        const pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0) res.rc = 0;
            else res.rc = 1;
            break;
        }
        if (w < 0) { res.rc = -2; break; }
        if (cap > 0.0 && now_sec() - t0 > cap) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            timed_out = true;
            res.rc = -1;
            break;
        }
        usleep(2000);
    }
    CellRes c;
    const ssize_t got = read(fd[0], &c, sizeof c);
    close(fd[0]);
    if (!timed_out && got == (ssize_t)sizeof c) {
        res = c;
        if (res.rc == 0 && c.rc != 0) res.rc = c.rc;
    } else if (timed_out) {
        res.wall = cap;
    } else if (res.rc == 0) {
        res.rc = 1;
    }
    return res;
}

int main(int argc, char** argv) {
    std::string sidecar, list_path, out_path, run_id = "batch", dataset = "ldbc-sf10";
    int reps = 1, rep_base = 0;
    double warm_cap = -1.0;
    size_t offset = 0;
    double cell_cap = 3600.0;
    int load_hold = 0;
    bool warmup = false, plan_only = false, meta_only = false, v3_only = false;
    std::string tag, base_graph;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--sidecar" && i + 1 < argc) sidecar = argv[++i];
        else if (a == "--list" && i + 1 < argc) list_path = argv[++i];
        else if (a == "--out" && i + 1 < argc) out_path = argv[++i];
        else if (a == "--reps" && i + 1 < argc) reps = std::atoi(argv[++i]);
        else if (a == "--rep-base" && i + 1 < argc) rep_base = std::atoi(argv[++i]);
        else if (a == "--warm-cap" && i + 1 < argc) warm_cap = std::atof(argv[++i]);
        else if (a == "--run-id" && i + 1 < argc) run_id = argv[++i];
        else if (a == "--dataset" && i + 1 < argc) dataset = argv[++i];
        else if (a == "--offset" && i + 1 < argc) offset = (size_t)std::atoll(argv[++i]);
        else if (a == "--cell-cap" && i + 1 < argc) cell_cap = std::atof(argv[++i]);
        else if (a == "--base-graph" && i + 1 < argc) base_graph = argv[++i];
        else if (a == "--warmup") warmup = true;
        else if (a == "--plan-only") plan_only = true;
        else if (a == "--golden" && i + 1 < argc) g_golden_path = argv[++i];
        else if (a == "--meta-only") meta_only = true;
        else if (a == "--v3-only") v3_only = true;
        else if (a == "--load-hold" && i + 1 < argc) load_hold = std::atoi(argv[++i]);
        else if (a == "--tag" && i + 1 < argc) tag = argv[++i];
        else if (a == "--opponent" && i + 1 < argc) g_opponent = argv[++i];
        else if (a == "--opponents" && i + 1 < argc) {
            std::string v = argv[++i];
            size_t p = 0;
            while (p <= v.size()) {
                const size_t q = v.find(',', p);
                const std::string x = v.substr(p, q == std::string::npos ? std::string::npos : q - p);
                if (!x.empty()) g_opponents.push_back(x);
                if (q == std::string::npos) break;
                p = q + 1;
            }
        }
        else if (a == "--no-mcs") setenv("V3_MCS_OFF", "1", 1);
        else if (a == "--no-depth-layer") setenv("V3_MCS_DEPTH_OFF", "1", 1);
    }
    if (!g_opponent.empty()) g_opponents.push_back(g_opponent);
    if (g_opponents.empty() && !v3_only) g_opponents.push_back("serial");
    for (const std::string& o : g_opponents) {
        if (o != "masc" && o != "batchenum" && o != "vlpq" && o != "serial") {
            std::fprintf(stderr, "unknown --opponent %s\n", o.c_str());
            return 2;
        }
    }
    if (sidecar.empty() || list_path.empty() || out_path.empty()) {
        std::fprintf(stderr, "usage: v3_batch --sidecar DIR --list FILE --out FILE [--reps N]\n");
        return 2;
    }
    GraphStore g;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    if (!g.load(sidecar, &err, meta_only)) {
        std::fprintf(stderr, "load: %s\n", err.c_str());
        return 1;
    }
    // Read the shard and collect the relationship types it actually touches.  Materialise only
    // those typed views before the first timed cell: both methods then start from the same
    // adjacency state, without paying for fifteen types per process.
    std::ifstream in(list_path);
    if (!in) {
        std::fprintf(stderr, "cannot open list\n");
        return 1;
    }
    std::vector<Round> rounds;
    std::vector<std::string> paths;
    std::vector<char> used(g.schema().types.size(), 0);
    std::string path;
    while (std::getline(in, path)) {
        if (path.empty() || path[0] == '#') continue;
        Round r;
        if (!load_round(path, &r, &err)) {
            std::fprintf(stderr, "round %s: %s\n", path.c_str(), err.c_str());
            return 1;
        }
        for (const auto& q : r.queries)
            for (const auto& e : q.edges)
                for (const auto& ty : e.types) {
                    const int32_t t = g.schema().type_of(ty);
                    if (t >= 0) used[(size_t)t] = 1;
                }
        // Query compilation: the canonical form is static text, computed once before any
        // timed cell, like parsing.  The point executor's dedup then does not rebuild it.
        for (auto& q : r.queries) q.canon = canonical_key(q);
        // Static point plan: one compiled shape per candidate and the replay map.  This is
        // query compilation, not data-dependent planning, and runs before every timed cell.
        prepare_point_round(g, &r);
        rounds.push_back(std::move(r));
        paths.push_back(path);
    }
    if (!meta_only) {
        for (uint32_t t = 0; t < used.size(); ++t)
            if (used[t]) g.typed(t);
        // Warm only the property columns the rounds touch.  The map itself stays outside the
        // timed cells, and every forked cell inherits it; without this each cell paid one
        // mmap per RETURN column, which is the fixed cost that remained after the id lookup.
        for (const auto& r : rounds)
            for (const auto& q : r.queries) g.warm_query(q);
        // The equality index of every anchor column is a load-time structure, shared by the
        // per-query executor and the masked planner in every forked cell.
        for (const auto& r : rounds)
            for (const auto& q : r.queries) warm_anchors(g, q);
        // The old load-time tiny decision is not used: point rounds take the compiled point
        // plan, and all data-dependent planning stays inside the timed cell.
        // The base view is loaded once, outside the timed cells, and inherited by the forked
        // cell; it is the v2 graph used by the pure-count Fallback.
        Graph* base_loaded = nullptr;
        if (!base_graph.empty() && !meta_only) {
            base_loaded = new Graph(true);
            base_loaded->loadGraphFromFile(base_graph);
            g_base_graph = base_loaded;
            std::printf("[base] %s vertices=%u edges=%llu\n", base_graph.c_str(),
                        base_loaded->getVerticesCount(),
                        (unsigned long long)base_loaded->getEdgesCount());
            std::fflush(stdout);
        }
    }
    const double load_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (load_hold > 0) {
        std::printf("[load-hold] %.3fs pid=%ld\n", load_sec, (long)getpid());
        std::fflush(stdout);
        sleep((unsigned)load_hold);
        return 0;
    }
    std::ofstream out(out_path, std::ios::app);
    if (!out) {
        std::fprintf(stderr, "cannot open out\n");
        return 1;
    }
    if (plan_only) {
        for (size_t idx = offset; idx < rounds.size(); ++idx) {
            Round& r = rounds[idx];
            const double t0p = now_sec();
            RoundPlan plan = align_and_partition(g, r, (uint32_t)r.queries.size(), false);
            const double dt = now_sec() - t0p;
            double full_est = 0.0;
            for (const auto& q : r.queries) full_est += estimate_query(g, q);
            out << "{\"run_id\":\"" << esc(run_id) << "\",\"round\":\"" << esc(tag) << "r";
            char rn[16];
            std::snprintf(rn, sizeof rn, "%04zu", idx);
            out << rn << "\",\"round_index\":" << idx
                << ",\"family\":\"" << esc(r.family) << "\",\"method\":\"plan\""
                << ",\"plan_sec\":" << dt
                << ",\"envelopes\":" << (plan.depth_chain ? (size_t)1 : plan.envelopes.size())
                << ",\"prefix_envelopes\":" << plan.prefix_envelopes.size()
                << ",\"fallback\":" << plan.fallback.size()
                << ",\"depth_chain\":" << (plan.depth_chain ? "true" : "false")
                << ",\"full_est\":" << full_est
                << ",\"tiny\":" << (r.tiny ? "true" : "false")
                << ",\"tiny_est\":" << r.tiny_est
                << ",\"queries\":" << r.queries.size()
                << ",\"est\":[";
            for (size_t e = 0; e < plan.envelopes.size(); ++e) {
                if (e) out << ',';
                const auto& env = plan.envelopes[e];
                out << "{\"members\":" << env.members.size()
                    << ",\"sum\":" << env.est_sum
                    << ",\"work\":" << env.est_work
                    << ",\"build\":" << env.est_build
                    << ",\"gain\":" << env.est_gain
                    << ",\"mcs\":" << (env.mcs_worth ? "true" : "false")
                    << ",\"gate\":\"" << esc(env.mcs_gate) << "\""
                    << ",\"budget\":" << env.budget_sec << "}";
            }
            out << "],\"prefix\":[";
            for (size_t e = 0; e < plan.prefix_envelopes.size(); ++e) {
                if (e) out << ',';
                const auto& env = plan.prefix_envelopes[e];
                out << "{\"members\":" << env.members.size()
                    << ",\"end\":" << env.prefix_end
                    << ",\"sum\":" << env.est_sum
                    << ",\"work\":" << env.est_work
                    << ",\"build\":" << env.est_build
                    << ",\"gain\":" << env.est_gain
                    << ",\"mcs\":" << (env.mcs_worth ? "true" : "false")
                    << ",\"gate\":\"" << esc(env.mcs_gate) << "\""
                    << ",\"budget\":" << env.budget_sec << "}";
            }
            out << "],\"cand\":[";
            for (size_t i = 0; i < plan.why.size(); ++i) {
                if (i) out << ',';
                out << "{\"why\":\"" << esc(plan.why[i]) << "\""
                    << ",\"own\":" << (i < plan.est_own.size() ? plan.est_own[i] : 0.0)
                    << ",\"before\":" << (i < plan.est_before.size() ? plan.est_before[i] : 0.0)
                    << ",\"after\":" << (i < plan.est_after.size() ? plan.est_after[i] : 0.0)
                    << ",\"env\":" << (i < plan.env_of.size() ? plan.env_of[i] : -1)
                    << ",\"prefix\":" << (i < plan.prefix_of.size() ? plan.prefix_of[i] : -1)
                    << "}";
            }
            out << "],\"why\":[";
            for (size_t i = 0; i < plan.why.size(); ++i) {
                if (i) out << ',';
                out << '"' << esc(plan.why[i]) << '"';
            }
            out << "],\"v3_round\":\"" << esc(paths[idx]) << "\"}\n";
            out.flush();
            std::printf("[plan] r%04zu env=%zu fallback=%zu %.3fs%s\n", idx,
                        plan.depth_chain ? (size_t)1 : plan.envelopes.size(),
                        plan.fallback.size(), dt, plan.depth_chain ? " depth_chain" : "");
        }
        return 0;
    }
    for (size_t idx = offset; idx < rounds.size(); ++idx) {
        Round& r = rounds[idx];
        const std::string& rp = paths[idx];
        // Both methods must see the same page-cache state: property columns are mmap'd lazily,
        // so without a warm-up the first method of the first repeat pays the column faults and
        // the other one measures a warm cache.  The warm-up run is not recorded.
        if (warmup) {
            CellRes w = run_cell(g, r, true, warm_cap > 0 ? warm_cap : cell_cap,
                                 false, idx, -1);
            std::printf("[warm] r%04zu wall=%.3fs work=%llu\n", idx, w.wall,
                        (unsigned long long)(w.edges + w.inter));
            std::fflush(stdout);
        }
        for (int rep = 0; rep < std::max(1, reps); ++rep) {
            const int rep_out = rep_base + rep;
            std::vector<std::string> methods = {"v3"};
            for (const std::string& o : g_opponents) methods.push_back(o);
            if (rep_out & 1) std::reverse(methods.begin(), methods.end());
            for (const std::string& method : methods) {
                const bool serial = method == "serial";
                const bool opp = method != "v3" && method != "serial";
                g_opponent = opp ? method : "";
                const double cell_t0 = now_sec();
                CellRes c = opp ? run_opp_cell(g, r, cell_cap)
                                : run_cell(g, r, serial, cell_cap, !serial, idx, rep_out);
                const double cell_t1 = now_sec();
                const double wall = c.rc == -1 ? cell_t1 - cell_t0 : c.wall;
                const uint64_t work = c.edges + c.inter;
                out << "{\"run_id\":\"" << esc(run_id) << "\",\"dataset\":\"" << esc(dataset)
                    << "\",\"round\":\"" << esc(tag) << "r";
                char rn[16];
                std::snprintf(rn, sizeof rn, "%04zu", idx);
                out << rn << "\",\"round_index\":" << idx
                    << ",\"family\":\"" << esc(r.family) << "\",\"method\":\"" << method
                    << "\",\"repeat\":" << rep_out << ",\"wall_sec\":" << wall
                    << ",\"work\":" << work << ",\"work_kind\":\"edges_expanded+tuples\""
                    << ",\"edges_expanded\":" << c.edges << ",\"tuples\":" << c.inter
                    << ",\"output_tuples\":" << c.rows << ",\"solutions\":" << c.sol
                    << ",\"mcs_edges\":" << c.mcs_edges << ",\"mcs_vertices\":" << c.mcs_vertices
                    << ",\"mcs_bytes\":" << c.mcs_bytes
                    << ",\"plan_sec\":" << c.plan_sec
                    << ",\"env_sec\":" << c.env_sec
                    << ",\"fallback_sec\":" << c.fallback_sec
                    << ",\"mcs_build_sec\":" << c.mcs_build_sec
                    << ",\"envelopes\":" << c.envelopes << ",\"fallback\":" << c.fallback
                    << ",\"env_blocks\":" << c.env_blocks
                    << ",\"block_envelope\":" << c.block_envelope
                    << ",\"departures\":" << c.departures
                    << ",\"tiny\":" << (r.tiny ? "true" : "false")
                    << ",\"tiny_est\":" << r.tiny_est
                    << ",\"peak_rss_kb\":" << c.rss_kb
                    << ",\"covered_candidates\":" << c.covered
                    << ",\"attempted_candidates\":" << c.attempted
                    << ",\"coverage\":" << (c.attempted ? (double)c.covered / (double)c.attempted : 0.0)
                    << ",\"masc_sec\":" << c.masc_sec
                    << ",\"depth_chain_max\":" << c.dc_max
                    << ",\"depth_counts\":[";
                for (int32_t d = 1; d <= c.dc_max && d < 64; ++d)
                    out << (d > 1 ? "," : "") << c.dc_count[d];
                out << "],\"depth_edges\":[";
                for (int32_t d = 1; d <= c.dc_max && d < 64; ++d)
                    out << (d > 1 ? "," : "") << c.dc_edge[d];
                out << "],\"depth_states\":[";
                for (int32_t d = 1; d <= c.dc_max && d < 64; ++d)
                    out << (d > 1 ? "," : "") << c.dc_state[d];
                out << "],\"depth_logical_edges\":[";
                for (int32_t d = 1; d <= c.dc_max && d < 64; ++d)
                    out << (d > 1 ? "," : "") << c.dc_ledge[d];
                out << "]"
                    << ",\"status\":\"" << (c.rc == -1 ? "timeout" : (c.rc == 0 ? "ok" : "error"))
                    << "\",\"rc\":" << (c.rc == -1 ? 124 : c.rc)
                    << ",\"timeout\":" << (c.rc == -1 ? "true" : "false")
                    << ",\"wall_includes_load\":false,\"load_sec\":" << (idx == 0 && rep == 0 ? load_sec : 0.0)
                    << ",\"v3_round\":\"" << esc(rp) << "\",\"tend\":" << now_sec() << "}\n";
                out.flush();
                std::printf("[ok] r%04zu r%d %s wall=%.3fs work=%llu env=%llu%s\n", idx, rep,
                            method.c_str(), wall, (unsigned long long)work,
                            (unsigned long long)c.envelopes, c.rc == -1 ? " TIMEOUT" : "");
                std::fflush(stdout);
            }
        }
    }
    return 0;
}
