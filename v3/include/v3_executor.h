// ECHO v3 per-query executor: enumeration of one candidate, projection of its result.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "v3_graph.h"
#include "v3_ir.h"

class Graph;

namespace v3 {

class TargetDistCache;

constexpr uint32_t kNoVal = 0xFFFFFFFFu;

struct Counters;

// One solution of a candidate's pattern: the value (vertex id / relationship row) of every
// pattern variable (kNoVal: unbound, e.g. an absent OPTIONAL branch).
struct Env {
    std::vector<uint32_t> v;
};

// One aggregate's partial state: count, integer sum, best (min/max), collect bag and the
// DISTINCT argument set.  Shared by the per-candidate pipeline and the masked aggregation.
struct AggState {
    uint64_t count = 0;
    int64_t isum = 0;
    bool has = false;
    Value best;
    std::vector<Value> bag;
    std::vector<Row> seen;
};

inline Value agg_result(const ProjectionItem& pi, const AggState& st) {
    switch (pi.func) {
        case AggFunc::COUNT_STAR:
        case AggFunc::COUNT_VAR:
        case AggFunc::COUNT: return Value::num((int64_t)st.count);
        case AggFunc::SUM: return st.count ? Value::num(st.isum) : Value::nul();
        case AggFunc::MIN:
        case AggFunc::MAX: return st.has ? st.best : Value::nul();
        case AggFunc::COLLECT: {
            Value v = Value::str("");
            for (const auto& b : st.bag) {
                if (!v.s.empty()) v.s += ",";
                v.s += b.is_str ? b.s : std::to_string(b.i);
            }
            return v;
        }
    }
    return Value::nul();
}

struct CandidateResult {
    uint32_t ncols = 0;
    std::vector<Value> data;            // row-major
    std::vector<std::string> labels;    // result column names
};

class QueryExec {
public:
    QueryExec(const GraphStore& g, std::string* err, TargetDistCache* tdc = nullptr,
              const Graph* base = nullptr);
    // Enumerate the matchings.  `follow` (optional) extends every matching with its local
    // variables; it is then the recorder (opt_null(i, env) = true appends the optional null row
    // of optional attempt i) and the opt_size(i, env) source of each OPTIONAL MATCH.
    struct Drv {
        virtual ~Drv() {}
        virtual void opt_enter(int attempt, uint64_t size) { (void)attempt; (void)size; }
        virtual void opt_null(int attempt) { (void)attempt; }
        virtual void record(int attempt, const Env& env, uint64_t size) { (void)attempt; (void)env; (void)size; }
    };
    bool enumerate(const Query& q, std::vector<Env>* out, Counters* cnt, long long limit,
                   Drv* drv, std::string* err);
    bool run(const Query& q, CandidateResult* res, Counters* cnt);
    // Evaluate the clauses from `start_clause` on, starting from prefix rows already produced
    // by a common-prefix envelope.  The prefix rows bind the variables of the shared pattern;
    // the continuation runs the member's private MATCH / OPTIONAL / WITH / RETURN stages.
    bool run_from(const Query& q, int32_t start_clause, const std::vector<Env>& in,
                  CandidateResult* res, Counters* cnt,
                  std::vector<Env>* out_matchings = nullptr);
    // Complete a partial binding inside `start_clause`: only the unbound variables are
    // enumerated, growing from the rows' bound vertices, then the later clauses run.
    bool run_from_bound(const Query& q, int32_t start_clause, const std::vector<Env>& in,
                        CandidateResult* res, Counters* cnt,
                        std::vector<Env>* out_matchings = nullptr);

    // The candidate's result from matchings the caller produced (the demultiplexed ones): the
    // WITH / RETURN pipeline over them, then DISTINCT / ORDER BY / SKIP / LIMIT.  `patterns_done`
    // starts at the first clause after the pattern (the masked path already evaluated it).
    static void project_only(const GraphStore& g, const Query& q, const std::vector<Env>& envs,
                             CandidateResult* res, bool sort, bool distinct, bool apply_limit,
                             long long row_limit, Counters* cnt);
    static int eval_atom(const GraphStore& g, const Query& q, const Env& env, const Atom& a,
                         const Row* cols = nullptr);
    static bool atom_evaluable(const Query& q, const Env& env, const Atom& a,
                               const Row* cols = nullptr);
    // The candidate's result rows from its matchings (RETURN with aggregation / DISTINCT /
    // ORDER BY / SKIP / LIMIT).  `envs` may be unsorted; the output order is deterministic.

    // True when the candidate has a grouping RETURN or a DISTINCT / ORDER BY / SKIP / LIMIT.
    static bool needs_records(const Query& q);

private:
    const GraphStore& g_;
    TargetDistCache* tdc_ = nullptr;
    const Graph* base_ = nullptr;   // v2 graph for base_count_pure (null: v3 path only)
};

bool env_value(const GraphStore& g, const Query& q, const Env& env, int32_t var,
               const std::string& prop, Value* out);
uint32_t result_columns(const Query& q);

}  // namespace v3
