// ECHO v3 per-query executor: stage-by-stage evaluation of one candidate.
//
// Stages: MATCH / later MATCH / OPTIONAL MATCH extend the rows with a pattern; variable-length
// edges are joined by the depth-masked expansion (v3_vl); a WITH (grouping) aggregates rows by
// (group key); RETURN projects the rows, then DISTINCT / ORDER BY / SKIP / LIMIT.
#include "v3_executor.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <cstdio>

#include "v3_base.h"
#include "v3_exec.h"
#include "v3_vl.h"
#include "v3_target.h"

namespace v3 {
namespace {

bool eval_atoms(const GraphStore& g, const Query& q, const Env& env, const std::vector<int32_t>& atoms,
                const Row* cols = nullptr) {
    for (size_t k = 0; k < atoms.size(); ++k) {
        const Atom& a = q.atoms[atoms[k]];
        if (!QueryExec::atom_evaluable(q, env, a, cols)) continue;
        const int r = QueryExec::eval_atom(g, q, env, a, cols);
        if (a.or_group >= 0) {
            int grp = r;
            size_t j = k + 1;
            while (j < atoms.size() && q.atoms[atoms[j]].or_group == a.or_group) ++j;
            for (size_t m = k + 1; m < j; ++m) {
                const Atom& b = q.atoms[atoms[m]];
                if (!QueryExec::atom_evaluable(q, env, b, cols)) continue;
                const int x = QueryExec::eval_atom(g, q, env, b, cols);
                grp = (grp == 1 || x == 1) ? 1 : 0;
            }
            k = j - 1;
            if (grp != 1) return false;
            continue;
        }
        if (r != 1) return false;
    }
    return true;
}

inline bool labels_ok(const GraphStore& g, const PatternVar& pv, uint32_t v) {
    return vertex_matches(g, pv, v);
}

// One projected value of a row: a pattern variable / property, or a column produced by an earlier
// WITH (`cols` holds the row's WITH columns; an absent slot reads as NULL).
namespace {
bool truthy(const Value& v) {
    if (v.is_null) return false;
    if (v.is_str) return !v.s.empty();
    return v.i != 0;
}

Value eval_expr(const GraphStore& g, const Query& q, const Env& env, const Row* cols,
                const Expr& e) {
    switch (e.kind) {
        case Expr::CONST: return e.c;
        case Expr::COL:
            if (cols && e.col >= 0 && (size_t)e.col < cols->v.size()) return cols->v[(size_t)e.col];
            return Value::nul();
        case Expr::PROP:
        case Expr::VAR: {
            Value v;
            if (e.var >= 0 && (size_t)e.var < env.v.size())
                env_value(g, q, env, e.var, e.prop, &v);
            return v;
        }
        case Expr::CMP: {
            if (e.args.size() < 2) return Value::nul();
            const Value a = eval_expr(g, q, env, cols, e.args[0]);
            const Value b = eval_expr(g, q, env, cols, e.args[1]);
            if (a.is_null || b.is_null) return Value::nul();
            return Value::num(cmp_values(a, b, e.op) ? 1 : 0);
        }
        case Expr::AND: {
            for (const auto& x : e.args)
                if (!truthy(eval_expr(g, q, env, cols, x))) return Value::num(0);
            return Value::num(1);
        }
        case Expr::OR: {
            for (const auto& x : e.args)
                if (truthy(eval_expr(g, q, env, cols, x))) return Value::num(1);
            return Value::num(0);
        }
        case Expr::NOT:
            return Value::num(truthy(eval_expr(g, q, env, cols, e.args.empty() ? Expr{} : e.args[0])) ? 0 : 1);
        case Expr::IS_NULL:
            return Value::num(e.args.empty() || eval_expr(g, q, env, cols, e.args[0]).is_null ? 1 : 0);
        case Expr::LABEL: {
            if (e.var < 0 || (size_t)e.var >= env.v.size() || env.v[(size_t)e.var] == kNoVal)
                return Value::num(0);
            const uint32_t lid = g.label(env.v[(size_t)e.var]);
            for (const auto& l : e.labels) {
                const int32_t id = g.schema().label_of(l);
                if (id >= 0 && (uint32_t)id == lid) return Value::num(1);
            }
            return Value::num(0);
        }
        case Expr::CASE: {
            for (size_t i = 0; i + 1 < e.args.size(); i += 2)
                if (truthy(eval_expr(g, q, env, cols, e.args[i])))
                    return eval_expr(g, q, env, cols, e.args[i + 1]);
            if (e.args.size() & 1) return eval_expr(g, q, env, cols, e.args.back());
            return Value::nul();
        }
        case Expr::COALESCE: {
            for (const auto& x : e.args) {
                Value v = eval_expr(g, q, env, cols, x);
                if (!v.is_null) return v;
            }
            return Value::nul();
        }
        case Expr::ARITH: {
            if (e.args.size() < 2) return Value::nul();
            const Value a = eval_expr(g, q, env, cols, e.args[0]);
            const Value b = eval_expr(g, q, env, cols, e.args[1]);
            if (a.is_null || b.is_null) return Value::nul();
            if (e.op == "+") return Value::num(a.i + b.i);
            if (e.op == "-") return Value::num(a.i - b.i);
            if (e.op == "*") return Value::num(a.i * b.i);
            if (e.op == "/") return b.i == 0 ? Value::nul() : Value::num(a.i / b.i);
            return Value::nul();
        }
        case Expr::FUNC:
        default:
            return Value::nul();
    }
}
}  // namespace

Value item_value(const GraphStore& g, const Query& q, const Env& env, const Row* cols,
                 const ProjectionItem& it) {
    if (it.kind == ProjectionItem::COL) {
        if (cols && it.col >= 0 && (size_t)it.col < cols->v.size()) return cols->v[(size_t)it.col];
        return Value::nul();
    }
    if (it.kind == ProjectionItem::EXPR) {
        if (it.expr >= 0 && (size_t)it.expr < q.exprs.size())
            return eval_expr(g, q, env, cols, q.exprs[(size_t)it.expr]);
        return Value::nul();
    }
    Value v;
    env_value(g, q, env, it.var, it.prop, &v);
    return v;
}

struct StagePlan {
    std::vector<int32_t> edge_stage;
    std::vector<int32_t> var_stage;
};

StagePlan plan_stages(const Query& q, int32_t first) {
    StagePlan p;
    p.edge_stage.assign(q.edges.size(), -1);
    p.var_stage.assign(q.vars.size(), -1);
    const int ncl = (int)q.clauses.size();
    // a vertex a clause before `first` writes is already bound; its stage is that clause, so
    // an edge whose one end it is starts the walk from the bound end instead of scanning the
    // other end
    for (int c = 0; c < first && c < ncl; ++c) {
        if (q.clauses[(size_t)c].kind != ClauseKind::MATCH &&
            q.clauses[(size_t)c].kind != ClauseKind::OPTIONAL_MATCH) continue;
        for (int32_t v : q.clauses[(size_t)c].vars)
            if (p.var_stage[(size_t)v] < 0 || c < p.var_stage[(size_t)v])
                p.var_stage[(size_t)v] = c;
    }
    for (size_t e = 0; e < q.edges.size(); ++e) {
        const PatternEdge& pe = q.edges[e];
        for (int c = std::max(first, pe.clause); c < ncl; ++c) {
            const ClauseKind k = q.clauses[c].kind;
            if (k != ClauseKind::MATCH && k != ClauseKind::OPTIONAL_MATCH) continue;
            // A WHERE-pattern edge carries its clause but the builder may not append it to
            // the clause's own edge list; the edge's clause is authoritative.
            bool has = (pe.clause == c);
            if (!has)
                for (int32_t x : q.clauses[c].edges)
                    if (x == (int32_t)e) has = true;
            if (!has) continue;
            p.edge_stage[e] = c;
            for (int32_t v : {pe.from, pe.to})
                if (p.var_stage[v] < 0 || c < p.var_stage[v]) p.var_stage[v] = c;
            break;
        }
    }
    return p;
}

struct Step {
    int32_t var = -1;
    int32_t parent = -1;
    int32_t edge = -1;
};

std::vector<Step> stage_steps(const GraphStore& g, const Query& q, const StagePlan& pl, int c,
                              bool* root_scan) {
    std::vector<Step> steps;
    std::vector<char> done(q.vars.size(), 0);
    std::vector<int32_t> queue;
    // A variable an earlier stage bound is the join seed, as before.
    for (size_t e = 0; e < q.edges.size(); ++e) {
        if (pl.edge_stage[e] != c) continue;
        const PatternEdge& pe = q.edges[e];
        if (pl.var_stage[pe.from] < c) { queue.push_back(pe.from); break; }
        if (pl.var_stage[pe.to] < c) { queue.push_back(pe.to); break; }
    }
    // Otherwise choose the root exactly as the masked walk does: an anchored class first (a
    // constant in the pattern map or in WHERE), then the smallest label class.  Scanning an
    // unanchored table because it happens to be the written side of the first edge is what made
    // the serial plan expand from a whole class while v3 started from the constant.
    if (queue.empty()) {
        int32_t seed = -1;
        double best = 0.0;
        for (size_t v = 0; v < q.vars.size(); ++v) {
            if (pl.var_stage[v] != c || q.vars[v].kind != PatternVar::VERTEX) continue;
            const double card = var_root_card(g, q, (int32_t)v);
            const double sc = var_const_anchored(q, (int32_t)v) ? card * 1e-6 : card;
            if (seed < 0 || sc < best) { seed = (int32_t)v; best = sc; }
        }
        if (seed >= 0) queue.push_back(seed);
    }
    if (queue.empty()) return steps;
    // the growth starts at the root: a variable the stage binds is scanned here (parent -1), a
    // variable an earlier stage bound is extended from (the first step's parent is that root)
    *root_scan = pl.var_stage[queue[0]] >= c;
    if (*root_scan) steps.push_back(Step{queue[0], -1, -1});
    done[queue[0]] = 1;
    std::vector<char> seen(q.edges.size(), 0);
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const int32_t u = queue[qi];
        for (size_t e = 0; e < q.edges.size(); ++e) {
            if (pl.edge_stage[e] != c || seen[e]) continue;
            const PatternEdge& pe = q.edges[e];
            int32_t w = -1;
            if (pe.from == u) w = pe.to;
            else if (pe.to == u) w = pe.from;
            else continue;
            seen[e] = 1;
            if (done[w]) {
                // both ends already bound by this stage: the edge is a check of the row (a
                // cycle of the pattern), not a second binding
                steps.push_back(Step{w, u, (int32_t)e});
                continue;
            }
            done[w] = 1;
            steps.push_back(Step{w, u, (int32_t)e});
            queue.push_back(w);
        }
    }
    for (size_t v = 0; v < q.vars.size(); ++v)
        if (pl.var_stage[v] == c && !done[v]) {
            done[v] = 1;
            steps.push_back(Step{(int32_t)v, -1, -1});
        }
    return steps;
}

// The steps of stage c when one of its pattern variables is already bound by the row (the
// second MATCH of a pattern that repeats a variable): a join, not another enumeration.  A step
// binds a new variable from a vertex the row or an earlier step of this stage holds; an edge
// whose two ends are both bound is a check of the row (a step with the target already bound).
// Variables of the stage no step reaches are not enumerated by the join (the stage's edges all
// touch the connected component of the bound variable, which the schema guarantees per clause).
std::vector<Step> stage_steps_join(const Query& q, int32_t c, const std::vector<int32_t>& bound_before,
                                   std::vector<int32_t>* join_from) {
    std::vector<Step> steps;
    join_from->clear();
    std::vector<char> done(q.vars.size(), 0);
    std::vector<int32_t> queue;
    for (int32_t v : bound_before) {
        done[(size_t)v] = 1;
        queue.push_back(v);
    }
    std::vector<char> seen(q.edges.size(), 0);
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const int32_t u = queue[qi];
        for (size_t e = 0; e < q.edges.size(); ++e) {
            if (seen[e] || q.edges[e].clause != c) continue;
            const PatternEdge& pe = q.edges[e];
            int32_t w = -1;
            if (pe.from == u) w = pe.to;
            else if (pe.to == u) w = pe.from;
            else continue;
            seen[e] = 1;
            if (done[(size_t)w]) {
                // both ends bound: the edge is checked on the row (target already bound)
                steps.push_back(Step{w, u, (int32_t)e});
                join_from->push_back(u);
                continue;
            }
            done[(size_t)w] = 1;
            steps.push_back(Step{w, u, (int32_t)e});
            join_from->push_back(u);
            queue.push_back(w);
        }
    }
    return steps;
}


// Cheap round-local candidate space of one MATCH stage: the vertices each pattern variable can
// take after label/property filtering and two rounds of neighbour refinement, as a sorted list
// and a bitmap.  It is the same idea as the base's DPiso candidate sets (the base is the
// per-query reference), specialised to the v3 store and bounded so that a class too large to
// refine cheaply keeps the plain label list.  The point is to stop the walk from entering
// branches that die on a close edge: on the MASC graphs the unfiltered walk expanded ~50x more
// adjacency rows than the base for the same answers.
struct CandSpace {
    int32_t clause = -1;
    std::vector<std::vector<uint32_t>> label_ids;          // [var]
    std::vector<std::vector<uint64_t>> type_bits;          // [edge] over schema types
    std::vector<char> no_props;                            // [edge]
    std::vector<std::vector<uint32_t>> cand;               // [var] (empty: not refined)
    std::vector<std::vector<uint64_t>> bits;               // [var]
    bool refined = false;

    bool label_ok(int32_t var, uint32_t lab) const {
        if (var < 0 || (size_t)var >= label_ids.size()) return true;
        const auto& ids = label_ids[(size_t)var];
        if (ids.empty()) return true;
        for (uint32_t x : ids)
            if (x == lab) return true;
        return false;
    }
    bool type_ok(int32_t e, uint32_t t) const {
        if (e < 0 || (size_t)e >= type_bits.size()) return true;
        const auto& b = type_bits[(size_t)e];
        if (b.empty()) return true;
        return (size_t)(t >> 6) < b.size() && ((b[(size_t)(t >> 6)] >> (t & 63)) & 1ULL);
    }
    bool has(int32_t var, uint32_t v) const {
        if (!refined || var < 0 || (size_t)var >= bits.size()) return true;
        const auto& b = bits[(size_t)var];
        if (b.empty()) return true;
        return (size_t)(v >> 6) < b.size() && ((b[(size_t)(v >> 6)] >> (v & 63)) & 1ULL);
    }
};

// Build the candidate space of clause `clause` from the stage's variables.  Only a stage that
// binds all its variables from scratch is refined (a join keeps its row bindings).
CandSpace build_cand_space(const GraphStore& g, const Query& q, const StagePlan& pl, int32_t clause,
                           const std::vector<Step>& steps) {
    CandSpace cs;
    cs.clause = clause;
    cs.label_ids.assign(q.vars.size(), {});
    cs.type_bits.assign(q.edges.size(), {});
    cs.no_props.assign(q.edges.size(), 0);
    for (size_t e = 0; e < q.edges.size(); ++e) {
        const PatternEdge& pe = q.edges[e];
        cs.no_props[e] = pe.props.empty() ? 1 : 0;
        if (g.schema().types.empty()) continue;
        cs.type_bits[e].assign((g.schema().types.size() + 63) / 64, 0ULL);
        for (const auto& T : pe.types) {
            const int32_t t = g.schema().type_of(T);
            if (t >= 0) cs.type_bits[e][(size_t)(t >> 6)] |= 1ULL << (t & 63);
        }
        if (cs.type_bits[e].empty()) cs.type_bits[e].clear();
    }
    std::vector<int32_t> vars;
    for (size_t v = 0; v < q.vars.size(); ++v) {
        if (pl.var_stage[v] != clause || q.vars[v].kind != PatternVar::VERTEX) continue;
        vars.push_back((int32_t)v);
        for (const auto& L : q.vars[v].labels) {
            const int32_t x = g.schema().label_of(L);
            if (x >= 0) cs.label_ids[v].push_back((uint32_t)x);
        }
        std::sort(cs.label_ids[v].begin(), cs.label_ids[v].end());
        cs.label_ids[v].erase(std::unique(cs.label_ids[v].begin(), cs.label_ids[v].end()),
                              cs.label_ids[v].end());
    }
    // Gate: build candidate lists only when the whole stage is small enough that two
    // refinement rounds cost far less than the branches they remove.  Larger classes keep the
    // label/type fast path (still no string lookups in the walk).
    size_t total = 0;
    for (int32_t v : vars) {
        const auto& ids = cs.label_ids[(size_t)v];
        if (ids.empty()) return cs;   // unlabelled variable: no cheap initial class
        for (uint32_t l : ids) total += g.vertices_of(l).size();
        if (total > 512) return cs;
    }
    cs.cand.assign(q.vars.size(), {});
    cs.bits.assign(q.vars.size(), {});
    for (int32_t v : vars) {
        for (uint32_t l : cs.label_ids[(size_t)v]) {
            const auto& vs = g.vertices_of(l);
            cs.cand[(size_t)v].insert(cs.cand[(size_t)v].end(), vs.begin(), vs.end());
        }
        std::sort(cs.cand[(size_t)v].begin(), cs.cand[(size_t)v].end());
        cs.cand[(size_t)v].erase(std::unique(cs.cand[(size_t)v].begin(), cs.cand[(size_t)v].end()),
                                 cs.cand[(size_t)v].end());
        if (!q.vars[(size_t)v].props.empty()) {
            auto& c = cs.cand[(size_t)v];
            c.erase(std::remove_if(c.begin(), c.end(), [&](uint32_t x) {
                        return !vertex_matches(g, q.vars[(size_t)v], x);
                    }), c.end());
        }
    }
    auto has_accepted_neighbour = [&](uint32_t x, int32_t e, int32_t other, bool forward) {
        const PatternEdge& pe = q.edges[(size_t)e];
        if (!pe.props.empty() || pe.lo != 1 || pe.hi != 1) return true;
        if ((size_t)other >= cs.cand.size() || cs.cand[(size_t)other].empty()) return false;
        const uint32_t want = forward ? 1u : 0u;
        auto ok = [&](uint32_t w, uint32_t dir) {
            if (!pe.any_dir && dir != want) return false;
            const auto& c = cs.cand[(size_t)other];
            return std::binary_search(c.begin(), c.end(), w);
        };
        if (pe.types.size() == 1) {
            const int32_t t = g.schema().type_of(pe.types[0]);
            if (t < 0) return false;
            const AdjView a = g.typed((uint32_t)t);
            for (uint32_t j = (*a.off)[x]; j < (*a.off)[x + 1]; ++j)
                if (ok((*a.nbr)[j], (uint32_t)(*a.dir)[j])) return true;
            return false;
        }
        const AdjView a = g.all();
        const auto& rels = g.all_rels();
        for (uint32_t j = (*a.off)[x]; j < (*a.off)[x + 1]; ++j) {
            if (!cs.type_ok(e, g.edge_type(rels[j]))) continue;
            if (ok((*a.nbr)[j], (uint32_t)(*a.dir)[j])) return true;
        }
        return false;
    };
    for (int round = 0; round < 2; ++round) {
        bool changed = false;
        for (int32_t v : vars) {
            auto& c = cs.cand[(size_t)v];
            size_t w = 0;
            for (uint32_t x : c) {
                bool keep = true;
                for (size_t k = 0; k < q.edges.size() && keep; ++k) {
                    const PatternEdge& pe = q.edges[k];
                    if (pl.edge_stage[k] != clause) continue;
                    int32_t other = -1;
                    bool forward = true;
                    if (pe.from == v) { other = pe.to; forward = true; }
                    else if (pe.to == v) { other = pe.from; forward = false; }
                    else continue;
                    if (pl.var_stage[(size_t)other] != clause) continue;
                    if (cs.cand[(size_t)other].empty()) continue;
                    if (!has_accepted_neighbour(x, (int32_t)k, other, forward)) keep = false;
                }
                if (keep) c[w++] = x;
                else changed = true;
            }
            c.resize(w);
        }
        if (!changed) break;
    }
    for (int32_t v : vars) {
        cs.bits[(size_t)v].assign((g.num_vertices() + 63) / 64, 0ULL);
        for (uint32_t x : cs.cand[(size_t)v]) cs.bits[(size_t)v][(size_t)(x >> 6)] |= 1ULL << (x & 63);
    }
    cs.refined = true;
    (void)steps;
    return cs;
}

class Walker {
public:
    Walker(const GraphStore& g, const Query& q, int32_t clause, bool root_scan, Scratch* sc,
           const CandSpace* cs, TargetDistCache* tdc)
        : g_(g), q_(q), clause_(clause), root_scan_(root_scan), sc_(sc), cs_(cs), tdc_(tdc) {
        // Relationship uniqueness is a Cypher rule of one MATCH clause: every relationship of the
        // clause is distinct, while vertices may repeat.  A tree of fixed edges is not exempt:
        // when two variables collapse to one vertex, the two pattern edges can be the same
        // relationship.  The ledger therefore runs for every clause with at least two edges, and
        // for every variable-length edge (its path may bounce on one undirected relationship).
        // WALK is the only semantics that turns it off.
        bool has_vl = false;
        for (const auto& e : q.edges)
            if (e.lo != 1 || e.hi != 1) has_vl = true;
        need_vis_ = q.sem != Semantics::WALK && (q.edges.size() >= 2 || has_vl);
        injective_ = q.sem == Semantics::ISO || q.sem == Semantics::SIMPLE;
        // Constant equality predicates of this clause are scan filters, the same push-down the
        // masked path performs in accepted(): `WHERE x.id = c` and `{id: c}` filter a vertex as
        // soon as it is bound instead of at the leaf.
        early_.assign(q.vars.size(), {});
        for (size_t v = 0; v < q.vars.size(); ++v)
            early_[v] = const_atoms_of(q, (int32_t)v, clause);
    }
    std::vector<Step> steps;
    // stage steps started from a binding of the row itself (a variable bound before this stage);
    // -1: the stage scans it
    std::vector<int32_t> join_from;
    // Vertex variables of this clause that the input row already binds (a prefix departure, or
    // a stage that repeats a variable bound by an earlier MATCH).  ISO/SIMPLE distinctness
    // applies to them too: the step list alone misses a bound vertex that is only a walk root.
    std::vector<int32_t> prebound;

    void visit(Env* env, Counters* cnt, const std::function<void()>& leaf) {
        env_ = env;
        leaf_ = &leaf;
        if (steps.empty()) return;
        extend(0, cnt);
    }

    // one step of the growth from a value the row already binds (a join): the same adjacency
    // walk as extend, from `from` instead of the scanned parent
    void step_from(size_t k, uint32_t from, Counters* cnt) {
        const Step& st = steps[k];
        const PatternEdge& pe = q_.edges[st.edge];
        const PatternVar& pv = q_.vars[st.var];
        int32_t ev = -1;
        if (!pe.var_name.empty()) ev = q_.var_index(pe.var_name);
        auto take = [&](uint32_t w, uint32_t rel, bool count_edge = true) {
            if (vertex_taken(st.var, w)) return;
            if (cnt && count_edge) ++cnt->edges_expanded;
            env_->v[st.var] = w;
            if (ev >= 0) env_->v[ev] = rel;
            if (early_ok(st.var, w)) extend(k + 1, cnt);
            if (ev >= 0) env_->v[ev] = kNoVal;
            env_->v[st.var] = kNoVal;
        };
        if (pe.lo != 1 || pe.hi != 1) {
            const int32_t hi = vl_hi_bound(g_, pe);
            VlStats vst;
            vl_expand(g_, pe, std::max(0, pe.lo), hi, from, st.parent == pe.from, pe.any_dir, sc_,
                      [&](uint32_t w, int32_t) { if (labels_ok(g_, pv, w)) take(w, kNil, false); },
                      &vst, nullptr, vl_dist(st));
            if (cnt) cnt->vl_steps += vst.steps;
            return;
        }
        expand_from(st, k, from, take, cnt);
    }

private:
    const GraphStore& g_;
    const Query& q_;
    int32_t clause_;
    bool root_scan_;
    bool need_vis_ = false;
    bool injective_ = false;
    Scratch* sc_ = nullptr;
    const CandSpace* cs_ = nullptr;
    TargetDistCache* tdc_ = nullptr;
    Env* env_ = nullptr;
    const std::function<void()>* leaf_ = nullptr;
    std::vector<std::vector<int32_t>> early_;   // var -> constant equality atoms of this clause

    // The push-down predicate of a vertex that is about to be bound.  Constant RHS only, so the
    // atom is decidable with this variable alone; the leaf still evaluates the clause's atoms.
    bool early_ok(int32_t var, uint32_t x) {
        if (var < 0 || (size_t)var >= early_.size() || early_[(size_t)var].empty()) return true;
        const uint32_t old = env_->v[(size_t)var];
        env_->v[(size_t)var] = x;
        bool ok = true;
        for (int32_t ai : early_[(size_t)var])
            if (QueryExec::eval_atom(g_, q_, *env_, q_.atoms[(size_t)ai]) != 1) { ok = false; break; }
        env_->v[(size_t)var] = old;
        return ok;
    }

    // ISO/SIMPLE: a new binding may not reuse a data vertex already bound to another pattern
    // variable of this clause.  The step list holds exactly the variables this stage binds, so a
    // repeated occurrence of the same variable is allowed and a cross-clause reuse is not
    // restricted; `prebound` adds the clause's variables the input row already binds.
    bool vertex_taken(int32_t var, uint32_t x) const {
        if (!injective_ || var < 0) return false;
        for (const auto& st : steps) {
            if (st.var < 0 || st.var == var) continue;
            if (q_.vars[(size_t)st.var].kind != PatternVar::VERTEX) continue;
            if (env_->v[(size_t)st.var] == x) return true;
        }
        for (int32_t v : prebound)
            if (v != var && env_->v[(size_t)v] == x) return true;
        return false;
    }

    // A relationship between two vertices the row already binds (a close edge of the clause).
    // Every matching row is tried, because another edge of the clause may already hold the
    // first one: Cypher requires the relationships of one MATCH to be distinct.
    void join_bound(const Step& st, size_t k, Counters* cnt) {
        const PatternEdge& pe = q_.edges[st.edge];
        const uint32_t u = env_->v[(size_t)st.parent];
        const uint32_t w = env_->v[(size_t)st.var];
        const bool forward = st.parent == pe.from;
        int32_t ev = -1;
        if (!pe.var_name.empty()) ev = q_.var_index(pe.var_name);
        auto take_rel = [&](uint32_t rel) {
            const uint32_t lid = g_.edge_row(rel);
            if (need_vis_ && sc_ && sc_->vis_[lid]) return;
            if (need_vis_ && sc_) sc_->vis_[lid] = 1;
            if (ev >= 0) env_->v[(size_t)ev] = rel;
            extend(k + 1, cnt);
            if (ev >= 0) env_->v[(size_t)ev] = kNoVal;
            if (need_vis_ && sc_) sc_->vis_[lid] = 0;
        };
        if (pe.lo != 1 || pe.hi != 1) {
            // A variable-length edge between two vertices the row already binds: every path of
            // the member's hop range is one matching, so enumerate the paths instead of testing
            // a single adjacency row.  A departed prefix row therefore keeps its multiplicity.
            if (!sc_) return;
            const int32_t hi = vl_hi_bound(g_, pe);
            VlStats vst;
            vl_expand(g_, pe, std::max(0, pe.lo), hi, u, forward, pe.any_dir, sc_,
                      [&](uint32_t x, int32_t rel) {
                          if (x != w) return;
                          if (ev >= 0) env_->v[(size_t)ev] = rel >= 0 ? (uint32_t)rel : kNoVal;
                          extend(k + 1, cnt);
                          if (ev >= 0) env_->v[(size_t)ev] = kNoVal;
                      },
                      &vst, nullptr, nullptr);
            if (cnt) cnt->vl_steps += vst.steps;
            return;
        }
        const AdjView a = g_.all();
        for (uint32_t j = (*a.off)[u]; j < (*a.off)[u + 1]; ++j) {
            if ((*a.nbr)[j] != w) continue;
            if (!pe.any_dir && (*a.dir)[j] != (forward ? 1 : 0)) continue;
            const uint32_t rel = g_.all_rels()[j];
            if (!edge_ok(st.edge, rel)) continue;
            take_rel(rel);
        }
    }

    bool edge_ok(int32_t e, uint32_t rel) const {
        const PatternEdge& pe = q_.edges[e];
        const uint32_t t = g_.edge_type(rel);
        if (cs_) {
            if (!cs_->type_ok(e, t)) return false;
        } else if (!pe.types.empty()) {
            bool ok = false;
            for (const auto& T : pe.types)
                if ((uint32_t)g_.schema().type_of(T) == t) { ok = true; break; }
            if (!ok) return false;
        }
        for (const auto& p : pe.props) {
            const int32_t col = g_.type_col(t, p.first);
            if (col < 0) return false;
            if (!g_.edge_prop_eq(rel, t, (uint32_t)col, p.second)) return false;
        }
        return true;
    }

    // Reverse-BFS distance from the stage's walked endpoint to the edge's target, from the
    // round cache (one BFS per target spec, not one per input row).
    const std::vector<int32_t>* vl_dist(const Step& st) {
        if (!tdc_ || st.var < 0 || st.edge < 0) return nullptr;
        const PatternEdge& pe = q_.edges[(size_t)st.edge];
        if (pe.lo == 1 && pe.hi == 1) return nullptr;
        if (pe.hi < 0) return nullptr;   // unbounded: the bound never prunes
        return tdc_->get(g_, q_, pe, st.parent, st.var, pe.hi, st.parent == pe.from);
    }

    // Label test of a neighbour: resolved integer ids from the candidate space (no string map
    // lookup per edge); a variable with a property map still goes through vertex_matches.
    bool label_pass(int32_t var, uint32_t x) const {
        if (cs_ && q_.vars[(size_t)var].props.empty()) return cs_->label_ok(var, g_.label(x));
        return labels_ok(g_, q_.vars[(size_t)var], x);
    }

    void scan(size_t k, Counters* cnt) {
        Env& env = *env_;
        const Step& st = steps[k];
        const PatternVar& pv = q_.vars[st.var];
        // W counts the candidate rows a scan examines, one per candidate, not one per scan call:
        // a scan of a 7.4M-row class and a scan of an id-index lookup are not the same work.
        // The masked path charges its root candidates the same way.
        auto take = [&](uint32_t v) {
            if (vertex_taken(st.var, v)) return;
            if (!early_ok(st.var, v)) return;
            env.v[st.var] = v;
            extend(k + 1, cnt);
            env.v[st.var] = kNoVal;
        };
        if (cs_ && cs_->refined && !cs_->cand[st.var].empty()) {
            for (uint32_t v : cs_->cand[st.var]) {
                if (cnt) ++cnt->init_tuples;
                take(v);
            }
            return;
        }
        // A constant `id` root is an index lookup, not a scan of the label class.  The
        // lookup is the candidate row; when the id is absent one row was still examined.
        std::vector<uint32_t> id_vs;
        if (id_anchored_vertices(g_, q_, st.var, &id_vs)) {
            if (id_vs.empty()) {
                if (cnt) ++cnt->init_tuples;
                return;
            }
            for (uint32_t v : id_vs) {
                if (cnt) ++cnt->init_tuples;
                take(v);
            }
            return;
        }
        // A constant property anchor is an index probe too (same contract as the id anchor).
        std::vector<uint32_t> prop_vs;
        if (anchored_vertices(g_, q_, st.var, &prop_vs)) {
            if (prop_vs.empty()) {
                if (cnt) ++cnt->init_tuples;
                return;
            }
            for (uint32_t v : prop_vs) {
                if (cnt) ++cnt->init_tuples;
                take(v);
            }
            return;
        }
        if (pv.labels.empty()) {
            for (uint32_t v = 0; v < g_.num_vertices(); ++v) {
                if (cnt) ++cnt->init_tuples;
                if (labels_ok(g_, pv, v)) take(v);
            }
            return;
        }
        std::vector<const GraphArray<uint32_t>*> lists;
        for (const auto& L : pv.labels) {
            const int32_t id = g_.schema().label_of(L);
            if (id >= 0) lists.push_back(&g_.vertices_of((uint32_t)id));
        }
        if (lists.size() == 1) {
            for (uint32_t v : *lists[0]) {
                if (cnt) ++cnt->init_tuples;
                if (labels_ok(g_, pv, v)) take(v);
            }
            return;
        }
        std::vector<uint32_t> cand;
        for (const auto* l : lists) cand.insert(cand.end(), l->begin(), l->end());
        std::sort(cand.begin(), cand.end());
        cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
        for (uint32_t v : cand) {
            if (cnt) ++cnt->init_tuples;
            if (labels_ok(g_, pv, v)) take(v);
        }
    }

    void extend(size_t k, Counters* cnt) {
        Env& env = *env_;
        // Same growth-row unit as the masked path: a row at level k (k >= 1) is one tuple,
        // including the complete matching at the leaf.  Both ledgers therefore count the same
        // logical rows; aggregation rows are not part of the growth.
        if (cnt && k > 0) ++cnt->tuples;
        if (k == steps.size()) {
            if (!q_.clauses[clause_].where.empty() && !eval_atoms(g_, q_, env, q_.clauses[clause_].where))
                return;
            (*leaf_)();
            return;
        }
        const Step& st = steps[k];
        if (st.parent < 0) {
            // a root: scan it when the row does not bind it yet, else it is already fixed
            if (env.v[st.var] == kNoVal) scan(k, cnt);
            else extend(k + 1, cnt);
            return;
        }
        const PatternEdge& pe = q_.edges[st.edge];
        if (st.var >= 0 && env.v[st.var] != kNoVal) {
            // both ends bound: the edge is a check on the row (a repeated edge of a MATCH); the
            // relationship variable still binds to the relationship that satisfies the check
            if (st.parent >= 0 && env.v[st.parent] != kNoVal) join_bound(st, k, cnt);
            return;
        }
        if (k < join_from.size() && join_from[k] >= 0) {
            // the step starts from the row's own binding of the parent variable.  A NULL parent
            // (an absent OPTIONAL branch, or a prefix row that does not bind it) cannot be
            // walked: the row contributes no extension and the OPTIONAL stage pads it.
            const uint32_t pf = env.v[join_from[k]];
            if (pf == kNoVal) return;
            step_from(k, pf, cnt);
            return;
        }
        const uint32_t from = env.v[st.parent];
        if (from == kNoVal) return;
        const PatternVar& pv = q_.vars[st.var];
        int32_t ev = -1;
        if (!pe.var_name.empty()) ev = q_.var_index(pe.var_name);
        auto take = [&](uint32_t w, uint32_t rel, bool count_edge = true) {
            if (vertex_taken(st.var, w)) return;
            if (cnt && count_edge) ++cnt->edges_expanded;
            env.v[st.var] = w;
            if (ev >= 0) env.v[ev] = rel;
            if (early_ok(st.var, w)) extend(k + 1, cnt);
            if (ev >= 0) env.v[ev] = kNoVal;
            env.v[st.var] = kNoVal;
        };
        if (pe.lo != 1 || pe.hi != 1) {
            const int32_t hi = vl_hi_bound(g_, pe);
            VlStats vst;
            vl_expand(g_, pe, std::max(0, pe.lo), hi, from, st.parent == pe.from, pe.any_dir, sc_,
                      [&](uint32_t w, int32_t) { if (labels_ok(g_, pv, w)) take(w, kNil, false); },
                      &vst, nullptr, vl_dist(st));
            if (cnt) cnt->vl_steps += vst.steps;
            return;
        }
        expand_from(st, k, from, take, cnt);
    }

    // the neighbours of `from` along the step's edge, with the edge's type set (a multi-type edge
    // tests every relationship against its own type list instead of walking each type in turn)
    template <class Take>
    void expand_from(const Step& st, size_t k, uint32_t from, Take& take, Counters* cnt) {
        const PatternEdge& pe = q_.edges[st.edge];
        const bool forward = st.parent == pe.from;
        if (pe.types.size() == 1) {
            const int32_t t = g_.schema().type_of(pe.types[0]);
            if (t < 0) return;
            const AdjView a = g_.typed((uint32_t)t);
            const auto& rels = g_.typed_rels((uint32_t)t);
            for (uint32_t j = (*a.off)[from]; j < (*a.off)[from + 1]; ++j) {
                const uint32_t rel = rels[j];
                const uint32_t lid = g_.edge_row(rel);
                const uint32_t nbr = (*a.nbr)[j];
                if (cs_ && cs_->refined) {
                    if (!cs_->has(st.var, nbr)) continue;
                } else if (!label_pass(st.var, nbr)) {
                    continue;
                }
                if ((need_vis_ && sc_->vis_[lid]) || (!pe.any_dir && (*a.dir)[j] != (forward ? 1 : 0)) ||
                    !edge_ok(st.edge, rel)) continue;
                if (need_vis_) sc_->vis_[lid] = 1;
                take((*a.nbr)[j], rel);
                if (need_vis_) sc_->vis_[lid] = 0;
            }
            return;
        }
        const AdjView a = g_.all();
        for (uint32_t j = (*a.off)[from]; j < (*a.off)[from + 1]; ++j) {
            const uint32_t rel = g_.all_rels()[j];
            const uint32_t lid = g_.edge_row(rel);
            const uint32_t nbr = (*a.nbr)[j];
            if (cs_ && cs_->refined) {
                if (!cs_->has(st.var, nbr)) continue;
            } else if (!label_pass(st.var, nbr)) {
                continue;
            }
            if ((need_vis_ && sc_->vis_[lid]) || (!pe.any_dir && (*a.dir)[j] != (forward ? 1 : 0)) ||
                !edge_ok(st.edge, rel)) continue;
            if (need_vis_) sc_->vis_[lid] = 1;
            take((*a.nbr)[j], rel);
            if (need_vis_) sc_->vis_[lid] = 0;
        }
        (void)k;
        (void)cnt;
    }
};

// The relationship ledger is a per-relationship visited bitmap.  A single fixed edge (the
// IS1/IS3/IS4/IS5 point lookups) cannot repeat a relationship, so it needs no ledger; allocating
// and zeroing one per candidate was the fixed cost that hid the id-index win on those rounds.
bool needs_vis_ledger(const Query& q) {
    if (q.sem == Semantics::WALK) return false;
    if (q.edges.size() >= 2) return true;
    for (const auto& e : q.edges)
        if (e.lo != 1 || e.hi != 1) return true;
    return false;
}

std::vector<Env> stage_extend(const GraphStore& g, const Query& q, Counters* cnt,
                              const std::vector<Env>& in, const std::vector<Step>& steps,
                              bool root_scan, int32_t clause, Scratch* sc,
                              const CandSpace* cs, TargetDistCache* tdc,
                              const std::vector<int32_t>& join_from = {}) {
    if (needs_vis_ledger(q) && sc && sc->vis_.size() < g.num_rels())
        sc->vis_.assign(g.num_rels(), 0);
    std::vector<Env> out;
    for (const auto& r : in) {
        Env t = r;
        Walker w(g, q, clause, root_scan, sc, cs, tdc);
        w.steps = steps;
        w.join_from = join_from;
        w.visit(&t, cnt, [&]() {
            if (cnt) ++cnt->tuples;
            out.push_back(t);
        });
    }
    return out;
}

// A stage whose pattern variables the row already binds (a second MATCH that repeats a
// variable) is a join: its steps start from the row's bindings, and a row without them is
// dropped (the pattern cannot hold for it).
std::vector<Env> stage_join(const GraphStore& g, const Query& q, Counters* cnt,
                            const std::vector<Env>& in, int32_t clause, Scratch* sc) {
    std::vector<Env> out;
    std::vector<int32_t> bound;
    for (int32_t v = 0; v < (int32_t)q.vars.size(); ++v) {
        bool here = false;
        for (size_t e = 0; e < q.edges.size(); ++e)
            if (q.edges[e].clause == clause && (q.edges[e].from == v || q.edges[e].to == v)) here = true;
        bool before = false;
        for (size_t e = 0; e < q.edges.size(); ++e)
            if (q.edges[e].from == v || q.edges[e].to == v)
                if (q.edges[e].clause < clause) before = true;
        if (here && before) bound.push_back(v);
    }
    if (needs_vis_ledger(q) && sc && sc->vis_.size() < g.num_rels())
        sc->vis_.assign(g.num_rels(), 0);
    for (const auto& r : in) {
        bool any = false;
        for (int32_t v : bound)
            if (v < (int32_t)r.v.size() && r.v[(size_t)v] != kNoVal) any = true;
        if (!any) {
            out.push_back(r);   // an optional stage with no binding: padded by the caller
            continue;
        }
        Env t = r;
        std::vector<int32_t> jf;
        const std::vector<Step> steps = stage_steps_join(q, clause, bound, &jf);
        Walker w(g, q, clause, false, sc, nullptr, nullptr);
        w.steps = steps;
        w.join_from = jf;
        // `bound` are this clause's variables the row already holds; distinctness covers them.
        for (int32_t v : bound)
            if (q.vars[(size_t)v].kind == PatternVar::VERTEX) w.prebound.push_back(v);
        w.visit(&t, cnt, [&]() {
            if (cnt) ++cnt->tuples;
            out.push_back(t);
        });
    }
    return out;
}

void filter_compare(const GraphStore& g, const Query& q, const std::vector<int32_t>& atoms,
                    std::vector<Env>* rows) {
    if (atoms.empty()) return;
    std::vector<Env> keep;
    keep.reserve(rows->size());
    for (auto& r : *rows)
        if (eval_atoms(g, q, r, atoms)) keep.push_back(std::move(r));
    *rows = std::move(keep);
}

std::vector<int32_t> compare_atoms(const Query& q, int32_t c) {
    std::vector<int32_t> out;
    for (int32_t a : q.clauses[c].where)
        if (q.atoms[a].cmp.rhs == Comparison::VAR && q.atoms[a].prop.empty()) out.push_back(a);
    return out;
}

struct Candidate {
    const GraphStore& g;
    const Query& q;
    Counters* cnt;
    std::vector<Env> rows;
    std::vector<std::vector<Row>> arcs;   // per row: the group key of each grouping WITH passed
    std::vector<Row> cols;                // per row: the WITH output columns (indexed by q.items)
    std::vector<char> null_row;
    Scratch scratch_{0};   // the relationship ledger of the variable-length expansion, reused
                           // across the rows of one candidate (never allocated per row)
    TargetDistCache* tdc = nullptr;
    std::map<int32_t, CandSpace> cspace_;   // per MATCH clause, built once per candidate

    Candidate(const GraphStore& gg, const Query& qq, Counters* c, TargetDistCache* tdc)
        : g(gg), q(qq), cnt(c), tdc(tdc) {}

    void ensure_cols() {
        while (cols.size() < rows.size()) cols.emplace_back();
    }

    // patterns_done: the masked path already evaluated every MATCH / OPTIONAL clause; run the
    // WITH / RETURN pipeline only.  stop_before_with: enumerate the pattern matchings only (the
    // per-matching reference of v3_check), applying the post filters of the first WITH / RETURN.
    bool run(bool patterns_done = false, bool stop_before_with = false) {
        ensure_cols();
        if (std::getenv("V3_DEBUG_MATCH")) std::fprintf(stderr, "run start clauses=%zu rows=%zu\n", q.clauses.size(), rows.size());
        if (patterns_done) return run_tail(stop_before_with);
        for (int c = 0; c < (int)q.clauses.size(); ++c) {
            const Clause& cl = q.clauses[c];
            if (cl.kind == ClauseKind::RETURN) {
                if (!cl.pre_where.empty()) filter_pre(cl);
                return true;
            }
            if (cl.kind == ClauseKind::WITH) {
                if (!cl.pre_where.empty()) filter_pre(cl);
                // a transparent WITH only renames / carries variables: the pattern continues and
                // the per-matching reference keeps enumerating
                if (stop_before_with && needs_with(cl)) return true;
                if (needs_with(cl) && !regroup(cl)) return false;
                continue;
            }
            if (cl.kind == ClauseKind::OPTIONAL_MATCH) {
                if (!optional_stage(c, cl)) return false;
            } else if (cl.kind == ClauseKind::MATCH) {
                if (cl.edges.empty()) {
                    if (!single_node_stage(c)) return false;
                } else if (!match_stage(c)) return false;
            }
        }
        return true;
    }

    // Complete a shared prefix inside one MATCH: the rows bind some variables of `start`;
    // only the unbound variables are enumerated, growing from the bound ones.  The base then
    // continues with the later clauses as run_from does.
    bool run_bound(int32_t start) {
        ensure_cols();
        std::vector<int32_t> bound;
        if (!rows.empty())
            for (size_t v = 0; v < rows[0].v.size(); ++v)
                if (rows[0].v[v] != kNoVal) bound.push_back((int32_t)v);
        std::vector<int32_t> jf;
        const std::vector<Step> steps = stage_steps_join(q, start, bound, &jf);
        if (steps.empty()) return run_from(start + 1);
        // The clause's vertex variables the row binds stay subject to ISO/SIMPLE distinctness,
        // including the ones no step of this stage visits again (the walk root of the prefix).
        std::vector<char> on_edge(q.vars.size(), 0);
        for (const auto& e : q.edges)
            if (e.clause == start) {
                on_edge[(size_t)e.from] = 1;
                on_edge[(size_t)e.to] = 1;
            }
        std::vector<int32_t> pre;
        for (int32_t v : bound)
            if (on_edge[(size_t)v] && q.vars[(size_t)v].kind == PatternVar::VERTEX)
                pre.push_back(v);
        if (needs_vis_ledger(q) && scratch_.vis_.size() < g.num_rels())
            scratch_.vis_.assign(g.num_rels(), 0);
        std::vector<Env> out;
        out.reserve(rows.size());
        for (const auto& r : rows) {
            Env t = r;
            Walker w(g, q, start, false, &scratch_, nullptr, tdc);
            w.steps = steps;
            w.join_from = jf;
            w.prebound = pre;
            w.visit(&t, cnt, [&]() { out.push_back(t); });
        }
        rows.swap(out);
        // The prefix rows multiplied inside the first MATCH: keep the per-row grouping state
        // aligned with them (a departed prefix has no WITH before the pattern, so the new rows
        // carry the empty state).
        arcs.resize(rows.size());
        cols.resize(rows.size());
        null_row.assign(rows.size(), 0);
        return run_from(start + 1);
    }

    // Evaluate the clauses from `start` on.  The rows already bind the variables of a shared
    // prefix; a MATCH stage extends them from those bindings, and a WITH stage regroups them.
    bool run_from(int32_t start) {
        ensure_cols();
        for (int c = start; c < (int)q.clauses.size(); ++c) {
            const Clause& cl = q.clauses[(size_t)c];
            if (cl.kind == ClauseKind::RETURN) {
                if (!cl.pre_where.empty()) filter_pre(cl);
                return true;
            }
            if (cl.kind == ClauseKind::WITH) {
                if (!cl.pre_where.empty()) filter_pre(cl);
                if (needs_with(cl) && !regroup(cl)) return false;
                continue;
            }
            if (cl.kind == ClauseKind::OPTIONAL_MATCH) {
                if (!optional_stage(c, cl)) return false;
            } else if (cl.kind == ClauseKind::MATCH) {
                if (cl.edges.empty()) {
                    if (!single_node_stage(c)) return false;
                } else if (!match_stage(c)) return false;
            }
        }
        return true;
    }

    // The tail after the pattern: WITH (grouping / projection / DISTINCT / ORDER / LIMIT) and
    // RETURN.  Used by the masked path on the demultiplexed matchings.
    bool run_tail(bool stop_before_with = false) {
        for (int c = 0; c < (int)q.clauses.size(); ++c) {
            const Clause& cl = q.clauses[c];
            if (cl.kind == ClauseKind::MATCH || cl.kind == ClauseKind::OPTIONAL_MATCH) continue;
            if (cl.kind == ClauseKind::RETURN) {
                if (!cl.pre_where.empty()) filter_pre(cl);
                return true;
            }
            if (!cl.pre_where.empty()) filter_pre(cl);
            if (stop_before_with) return true;
            if (needs_with(cl) && !regroup(cl)) return false;
        }
        return true;
    }

private:
    // A MATCH with no pattern edge binds the stand-alone vertex variables (an IS1/IS3 anchor
    // such as `MATCH (p:Person) WHERE p.id = ...`).  Variables that some edge binds are left to
    // that edge's stage.
    bool single_node_stage(int c) {
        const Clause& cl = q.clauses[(size_t)c];
        if (std::getenv("V3_DEBUG_MATCH")) std::fprintf(stderr, "single_node start c=%d vars=%zu rows=%zu\n", c, cl.vars.size(), rows.size());
        // the vertices this clause writes; an earlier edge may already bind one (the row's value
        // is then checked, not scanned).  A plan without the clause variable list falls back to
        // the vertices no edge of this or an earlier clause binds.
        std::vector<int32_t> free_vars = cl.vars;
        if (free_vars.empty()) {
            for (int32_t v = 0; v < (int32_t)q.vars.size(); ++v) {
                if (q.vars[(size_t)v].kind != PatternVar::VERTEX) continue;
                bool on_edge = false;
                for (const auto& e : q.edges)
                    if (e.clause <= c && (e.from == v || e.to == v)) on_edge = true;
                if (!on_edge) free_vars.push_back(v);
            }
        }
        std::vector<Env> out;
        std::vector<std::vector<Row>> out_arcs;
        std::vector<Row> out_cols;
        for (size_t i = 0; i < rows.size(); ++i) {
            const std::vector<Row> arc = i < arcs.size() ? arcs[i] : std::vector<Row>();
            const Row col = i < cols.size() ? cols[i] : Row();
            std::function<void(size_t, Env&)> rec = [&](size_t k, Env& env) {
                if (k == free_vars.size()) {
                    if (eval_atoms(g, q, env, cl.where, &col)) {
                        out.push_back(env);
                        out_arcs.push_back(arc);
                        out_cols.push_back(col);
                    }
                    return;
                }
                const int32_t v = free_vars[k];
                const PatternVar& pv = q.vars[(size_t)v];
                // A stand-alone scan is a root scan of the growth; each candidate it examines is
                // one initial tuple, the same unit the walk's scan() charges (not one per
                // variable), so the serial and masked ledgers agree on single-node candidates.
                const std::vector<int32_t> early_atoms = const_atoms_of(q, v, c);
                auto early = [&](uint32_t x) {
                    if (early_atoms.empty()) return true;
                    const uint32_t old = env.v[(size_t)v];
                    env.v[(size_t)v] = x;
                    const bool ok = eval_atoms(g, q, env, early_atoms, &col);
                    env.v[(size_t)v] = old;
                    return ok;
                };
                if (env.v[(size_t)v] != kNoVal) {
                    if (vertex_matches(g, pv, env.v[(size_t)v]) &&
                        early(env.v[(size_t)v]))
                        rec(k + 1, env);
                    return;
                }
                auto take = [&](uint32_t x) {
                    if (cnt) ++cnt->init_tuples;
                    if (!vertex_matches(g, pv, x) || !early(x)) return;
                    env.v[(size_t)v] = x;
                    rec(k + 1, env);
                    env.v[(size_t)v] = kNoVal;
                };
                std::vector<uint32_t> id_vs;
                if (id_anchored_vertices(g, q, v, &id_vs)) {
                    if (id_vs.empty()) { if (cnt) ++cnt->init_tuples; }
                    for (uint32_t x : id_vs) take(x);
                    return;
                }
                std::vector<uint32_t> prop_vs;
                if (anchored_vertices(g, q, v, &prop_vs)) {
                    if (prop_vs.empty()) { if (cnt) ++cnt->init_tuples; }
                    for (uint32_t x : prop_vs) take(x);
                    return;
                }
                if (pv.labels.empty()) {
                    for (uint32_t x = 0; x < g.num_vertices(); ++x) take(x);
                } else {
                    for (const auto& L : pv.labels) {
                        const int32_t id = g.schema().label_of(L);
                        if (id < 0) continue;
                        for (uint32_t x : g.vertices_of((uint32_t)id)) take(x);
                    }
                }
            };
            Env env = rows[i];
            rec(0, env);
        }
        rows = std::move(out);
        arcs = std::move(out_arcs);
        cols = std::move(out_cols);
        null_row.assign(rows.size(), 0);
        if (std::getenv("V3_DEBUG_MATCH"))
            std::fprintf(stderr, "single_node c=%d free=%zu rows=%zu\n", c, free_vars.size(), rows.size());
        return true;
    }

    // post atoms (`friend <> p`, a join predicate the builder placed after the last MATCH) are
    // decided on the matching before the first WITH / RETURN
    void filter_pre(const Clause& cl) {
        std::vector<Env> keep;
        std::vector<std::vector<Row>> karc;
        std::vector<Row> kcol;
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row* cp = i < cols.size() ? &cols[i] : nullptr;
            if (!eval_atoms(g, q, rows[i], cl.pre_where, cp)) continue;
            keep.push_back(std::move(rows[i]));
            if (i < arcs.size()) karc.push_back(std::move(arcs[i]));
            if (i < cols.size()) kcol.push_back(std::move(cols[i]));
        }
        rows = std::move(keep);
        arcs = std::move(karc);
        cols = std::move(kcol);
        ensure_cols();
        null_row.assign(rows.size(), 0);
    }

    bool repeats_bound(int c) const {
        for (size_t e = 0; e < q.edges.size(); ++e) {
            if (q.edges[e].clause != c) continue;
            for (int32_t v : {q.edges[e].from, q.edges[e].to})
                for (size_t e2 = 0; e2 < q.edges.size(); ++e2)
                    if (q.edges[e2].clause < c && (q.edges[e2].from == v || q.edges[e2].to == v)) return true;
        }
        return false;
    }

    bool match_stage(int c) {
        const StagePlan pl = plan_stages(q, c);
        bool root_scan = true;
        const std::vector<Step> steps = stage_steps(g, q, pl, c, &root_scan);
        if (std::getenv("V3_DEBUG_MATCH"))
            std::fprintf(stderr, "match c=%d rows=%zu steps=%zu root_scan=%d repeats=%d\n", c, rows.size(),
                         steps.size(), (int)root_scan, (int)repeats_bound(c));
        if (repeats_bound(c)) {
            std::vector<Env> out = stage_join(g, q, cnt, rows, c, &scratch_);
            std::vector<std::vector<Row>> out_arcs;
            std::vector<Row> out_cols;
            out_arcs.reserve(out.size());
            out_cols.reserve(out.size());
            for (size_t i = 0; i < out.size(); ++i) {
                out_arcs.push_back(arcs.empty() ? std::vector<Row>() : arcs[i < arcs.size() ? i : arcs.size() - 1]);
                out_cols.push_back(cols.empty() ? Row() : cols[i < cols.size() ? i : cols.size() - 1]);
            }
            const std::vector<int32_t> q2 = compare_atoms(q, c);
            if (!q2.empty()) {
                std::vector<Env> keep;
                std::vector<std::vector<Row>> karc;
                std::vector<Row> kcol;
                for (size_t i = 0; i < out.size(); ++i)
                    if (eval_atoms(g, q, out[i], q2, &out_cols[i])) {
                        keep.push_back(std::move(out[i]));
                        karc.push_back(std::move(out_arcs[i]));
                        kcol.push_back(std::move(out_cols[i]));
                    }
                out.swap(keep);
                out_arcs.swap(karc);
                out_cols.swap(kcol);
            }
            rows = std::move(out);
            arcs = std::move(out_arcs);
            cols = std::move(out_cols);
            null_row.assign(rows.size(), 0);
            return true;
        }
        const std::vector<int32_t> quals = compare_atoms(q, c);
        CandSpace* cs = nullptr;
        {
            auto it = cspace_.find(c);
            if (it == cspace_.end()) {
                it = cspace_.emplace(c, build_cand_space(g, q, pl, c, steps)).first;
            }
            cs = &it->second;
        }
        std::vector<Env> out;
        std::vector<std::vector<Row>> out_arcs;
        std::vector<Row> out_cols;
        std::vector<char> out_null;
        for (size_t i = 0; i < rows.size(); ++i) {
            std::vector<Env> mine = stage_extend(g, q, cnt, {rows[i]}, steps, root_scan, c, &scratch_, cs, tdc);
            filter_compare(g, q, quals, &mine);
            const std::vector<Row> arc = i < arcs.size() ? arcs[i] : std::vector<Row>();
            const Row col = i < cols.size() ? cols[i] : Row();
            for (auto& e : mine) {
                out.push_back(std::move(e));
                out_arcs.push_back(arc);
                out_cols.push_back(col);
                out_null.push_back(0);
            }
        }
        rows = std::move(out);
        arcs = std::move(out_arcs);
        cols = std::move(out_cols);
        null_row = std::move(out_null);
        return true;
    }

    bool optional_stage(int c, const Clause& cl) {
        const StagePlan pl = plan_stages(q, c);
        bool root_scan = true;
        const std::vector<Step> steps = stage_steps(g, q, pl, c, &root_scan);
        const std::vector<int32_t> quals = compare_atoms(q, c);
        std::vector<int32_t> local;
        for (size_t v = 0; v < q.vars.size(); ++v) {
            if (pl.var_stage[v] != c) continue;
            // a variable the row already binds (an earlier clause mentions it) is carried, not
            // nulled by this OPTIONAL branch
            bool before = false;
            for (int cc = 0; cc < c; ++cc) {
                const Clause& pc = q.clauses[(size_t)cc];
                if (pc.kind != ClauseKind::MATCH && pc.kind != ClauseKind::OPTIONAL_MATCH) continue;
                for (int32_t x : pc.vars)
                    if (x == (int32_t)v) before = true;
            }
            if (!before) local.push_back((int32_t)v);
        }
        for (int32_t e : cl.edges)
            if (q.edges[e].lo == 1 && q.edges[e].hi == 1 && !q.edges[e].var_name.empty()) {
                const int32_t ev = q.var_index(q.edges[e].var_name);
                if (ev >= 0) local.push_back(ev);
            }
        std::vector<Env> out;
        std::vector<std::vector<Row>> out_arcs;
        std::vector<Row> out_cols;
        std::vector<char> out_null;
        for (size_t i = 0; i < rows.size(); ++i) {
            std::vector<Env> mine = stage_extend(g, q, cnt, {rows[i]}, steps, root_scan, c, &scratch_,
                                                 nullptr, tdc);
            filter_compare(g, q, quals, &mine);
            if (mine.empty()) {
                Env nul = rows[i];
                for (int32_t v : local) nul.v[v] = kNoVal;
                out.push_back(std::move(nul));
                out_arcs.push_back(arcs[i]);
                out_cols.push_back(i < cols.size() ? cols[i] : Row());
                out_null.push_back(1);
                if (cnt) ++cnt->tuples;
                continue;
            }
            const std::vector<Row> arc = i < arcs.size() ? arcs[i] : std::vector<Row>();
            const Row col = i < cols.size() ? cols[i] : Row();
            for (auto& e : mine) {
                out.push_back(std::move(e));
                out_arcs.push_back(arc);
                out_cols.push_back(col);
                out_null.push_back(0);
            }
        }
        rows = std::move(out);
        arcs = std::move(out_arcs);
        cols = std::move(out_cols);
        null_row = std::move(out_null);
        if (std::getenv("V3_DEBUG_MATCH"))
            std::fprintf(stderr, "optional c=%d in=%zu out=%zu local=%zu\n", c, rows.size(), out.size(), local.size());
        return true;
    }

    bool needs_with(const Clause& cl) const {
        if (cl.distinct || !cl.order_items.empty() || cl.limit_n >= 0 || cl.skip_n > 0) return true;
        for (int32_t it : cl.items) {
            const ProjectionItem& pi = q.items[(size_t)it];
            if (pi.kind != ProjectionItem::KEY || !pi.prop.empty()) return true;
        }
        return false;
    }

    static bool row_value_equal(const Row& a, const Row& b) { return a == b; }

    // One grouping WITH: keys from its non-aggregate items, aggregates accumulated per group, the
    // output row carries the group's representative env, the group key (arcs) and its columns.
    bool regroup(const Clause& cl) {
        std::vector<int32_t> keys, aggs;
        for (int32_t it : cl.items) {
            if (q.items[(size_t)it].kind == ProjectionItem::AGG) aggs.push_back(it);
            else keys.push_back(it);
        }
        std::unordered_map<Row, size_t, RowHash> where;
        std::vector<std::vector<Row>> new_arcs;
        std::vector<Row> new_cols;
        std::vector<std::vector<AggState>> states;
        std::vector<Env> new_rows;
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row* cp = i < cols.size() ? &cols[i] : nullptr;
            Row composite;
            std::vector<Value> keyval;
            for (int32_t it : keys) {
                Value v = item_value(g, q, rows[i], cp, q.items[(size_t)it]);
                keyval.push_back(v);
                composite.v.push_back(v);
            }
            std::vector<Row> arc = i < arcs.size() ? arcs[i] : std::vector<Row>();
            for (const auto& r : arc)
                for (const auto& v : r.v) composite.v.push_back(v);
            auto f = where.find(composite);
            size_t gi;
            if (f == where.end()) {
                gi = new_rows.size();
                where.emplace(composite, gi);
                new_rows.push_back(rows[i]);
                std::vector<Row> na = arc;
                Row krow;
                krow.v = keyval;
                na.push_back(std::move(krow));
                new_arcs.push_back(std::move(na));
                new_cols.push_back(cp ? *cp : Row());
                states.emplace_back(aggs.size());
            } else {
                gi = f->second;
            }
            if (cnt) ++cnt->rows_agg_in;
            for (size_t k = 0; k < aggs.size(); ++k) {
                const ProjectionItem& pi = q.items[(size_t)aggs[k]];
                AggState& st = states[gi][k];
                if (pi.func == AggFunc::COUNT_STAR) {
                    ++st.count;
                    continue;
                }
                Value x = pi.arg_col >= 0
                    ? ((cp && (size_t)pi.arg_col < cp->v.size()) ? cp->v[(size_t)pi.arg_col] : Value::nul())
                    : ([&]() {
                          ProjectionItem tmp;
                          tmp.kind = ProjectionItem::KEY;
                          tmp.var = pi.arg_var;
                          tmp.prop = pi.arg_prop;
                          return item_value(g, q, rows[i], nullptr, tmp);
                      })();
                if (pi.distinct && !x.is_null) {
                    Row rr;
                    rr.v.push_back(x);
                    bool dup = false;
                    for (const auto& s : st.seen)
                        if (s == rr) { dup = true; break; }
                    if (dup) continue;
                    st.seen.push_back(rr);
                }
                if (x.is_null) continue;
                switch (pi.func) {
                    case AggFunc::COUNT_VAR:
                    case AggFunc::COUNT: ++st.count; break;
                    case AggFunc::SUM:
                        ++st.count;
                        st.isum += x.is_str ? 0 : x.i;
                        break;
                    case AggFunc::MIN:
                        if (!st.has || x < st.best) st.best = x;
                        st.has = true;
                        break;
                    case AggFunc::MAX:
                        if (!st.has || st.best < x) st.best = x;
                        st.has = true;
                        break;
                    case AggFunc::COLLECT: st.bag.push_back(x); break;
                    default: break;
                }
            }
        }
        if (new_rows.empty() && keys.empty()) {
            new_rows.push_back(Env{std::vector<uint32_t>(q.vars.size(), kNoVal)});
            new_arcs.push_back({Row()});
            new_cols.push_back(Row());
            states.emplace_back(aggs.size());
        }
        // the WITH output columns of every group
        for (size_t gi = 0; gi < new_rows.size(); ++gi) {
            const Row* cp = gi < new_cols.size() ? &new_cols[gi] : nullptr;
            Row out = cp ? *cp : Row();
            out.v.resize(q.items.size(), Value::nul());
            for (int32_t it : cl.items) {
                const ProjectionItem& pi = q.items[(size_t)it];
                if (pi.kind == ProjectionItem::AGG) {
                    out.v[(size_t)it] = agg_result(pi, states[gi][(size_t)(std::find(aggs.begin(), aggs.end(), it) - aggs.begin())]);
                } else {
                    out.v[(size_t)it] = item_value(g, q, new_rows[gi], cp, pi);
                }
            }
            new_cols[gi] = std::move(out);
        }
        // HAVING (`WITH ... WHERE cnt > 1`) on the group rows
        if (!cl.where.empty()) {
            std::vector<Env> keep;
            std::vector<std::vector<Row>> karc;
            std::vector<Row> kcol;
            for (size_t i = 0; i < new_rows.size(); ++i)
                if (eval_atoms(g, q, new_rows[i], cl.where, &new_cols[i])) {
                    keep.push_back(std::move(new_rows[i]));
                    karc.push_back(std::move(new_arcs[i]));
                    kcol.push_back(std::move(new_cols[i]));
                }
            new_rows.swap(keep);
            new_arcs.swap(karc);
            new_cols.swap(kcol);
        }
        // DISTINCT on the projected WITH row
        if (cl.distinct) {
            std::unordered_map<Row, size_t, RowHash> seen;
            std::vector<Env> keep;
            std::vector<std::vector<Row>> karc;
            std::vector<Row> kcol;
            for (size_t i = 0; i < new_rows.size(); ++i) {
                Row key;
                for (int32_t it : cl.items) key.v.push_back(new_cols[i].v[(size_t)it]);
                if (!seen.emplace(key, 0).second) continue;
                keep.push_back(std::move(new_rows[i]));
                karc.push_back(std::move(new_arcs[i]));
                kcol.push_back(std::move(new_cols[i]));
            }
            new_rows.swap(keep);
            new_arcs.swap(karc);
            new_cols.swap(kcol);
        }
        if (!cl.order_items.empty()) {
            std::vector<size_t> ord(new_rows.size());
            for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
            std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) {
                for (size_t k = 0; k < cl.order_items.size(); ++k) {
                    const Value& x = new_cols[a].v[(size_t)cl.order_items[k]];
                    const Value& y = new_cols[b].v[(size_t)cl.order_items[k]];
                    if (x == y) continue;
                    const bool less = x < y;
                    return k < cl.order_desc.size() && cl.order_desc[k] ? !less : less;
                }
                return false;
            });
            std::vector<Env> r2;
            std::vector<std::vector<Row>> a2;
            std::vector<Row> c2;
            for (size_t i : ord) {
                r2.push_back(std::move(new_rows[i]));
                a2.push_back(std::move(new_arcs[i]));
                c2.push_back(std::move(new_cols[i]));
            }
            new_rows.swap(r2);
            new_arcs.swap(a2);
            new_cols.swap(c2);
        }
        size_t lo = 0, hi = new_rows.size();
        if (cl.skip_n > 0) lo = std::min(hi, (size_t)cl.skip_n);
        if (cl.limit_n >= 0) hi = std::min(hi, lo + (size_t)std::max<int64_t>(0, cl.limit_n));
        if (lo > 0 || hi < new_rows.size()) {
            std::vector<Env> r2(new_rows.begin() + lo, new_rows.begin() + hi);
            std::vector<std::vector<Row>> a2(new_arcs.begin() + lo, new_arcs.begin() + hi);
            std::vector<Row> c2(new_cols.begin() + lo, new_cols.begin() + hi);
            new_rows.swap(r2);
            new_arcs.swap(a2);
            new_cols.swap(c2);
        }
        rows = std::move(new_rows);
        arcs = std::move(new_arcs);
        cols = std::move(new_cols);
        null_row.assign(rows.size(), 0);
        if (cnt) cnt->rows_agg_out += rows.size();
        return true;
    }
};

// RETURN over the candidate's rows: plain or grouped by (arc, key); DISTINCT / ORDER BY /
// SKIP / LIMIT.
void project_rows(const GraphStore& g, const Query& q, const std::vector<Env>& rows,
                  const std::vector<std::vector<Row>>* arcs, const std::vector<Row>* cols,
                  CandidateResult* res, bool sort, bool distinct, bool apply_limit, long long row_limit,
                  Counters* cnt) {
    const Clause& ret = q.clauses[q.return_clause];
    bool has_agg = false;
    for (int32_t it : ret.items)
        if (q.items[it].kind == ProjectionItem::AGG) has_agg = true;
    std::vector<Row> out;
    if (!has_agg) {
        out.reserve(rows.size());
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row* cp = cols && i < cols->size() ? &(*cols)[i] : nullptr;
            Row r;
            for (int32_t it : ret.items) r.v.push_back(item_value(g, q, rows[i], cp, q.items[(size_t)it]));
            out.push_back(std::move(r));
        }
    } else {
        std::vector<int32_t> keys, aggs;
        for (int32_t it : ret.items)
            if (q.items[it].kind == ProjectionItem::AGG) aggs.push_back(it);
            else keys.push_back(it);
        std::unordered_map<Row, size_t, RowHash> where;
        std::vector<Row> key_order;
        std::vector<Row> col_order;
        std::vector<std::vector<AggState>> states;
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row* cp = cols && i < cols->size() ? &(*cols)[i] : nullptr;
            Row composite;
            if (arcs && i < arcs->size())
                for (const auto& r : (*arcs)[i])
                    for (const auto& v : r.v) composite.v.push_back(v);
            Row key;
            for (int32_t it : keys) {
                Value v = item_value(g, q, rows[i], cp, q.items[(size_t)it]);
                key.v.push_back(v);
                composite.v.push_back(v);
            }
            auto f = where.find(composite);
            size_t gi;
            if (f == where.end()) {
                gi = key_order.size();
                where.emplace(composite, gi);
                key_order.push_back(key);
                col_order.push_back(cp ? *cp : Row());
                states.emplace_back(aggs.size());
            } else {
                gi = f->second;
            }
            if (cnt) ++cnt->rows_agg_in;
            for (size_t k = 0; k < aggs.size(); ++k) {
                const ProjectionItem& pi = q.items[(size_t)aggs[k]];
                AggState& st = states[gi][k];
                if (pi.func == AggFunc::COUNT_STAR) {
                    ++st.count;
                    continue;
                }
                Value x = pi.arg_col >= 0
                    ? ((cp && (size_t)pi.arg_col < cp->v.size()) ? cp->v[(size_t)pi.arg_col] : Value::nul())
                    : ([&]() {
                          ProjectionItem tmp;
                          tmp.kind = ProjectionItem::KEY;
                          tmp.var = pi.arg_var;
                          tmp.prop = pi.arg_prop;
                          return item_value(g, q, rows[i], nullptr, tmp);
                      })();
                if (pi.distinct && !x.is_null) {
                    Row rr;
                    rr.v.push_back(x);
                    bool dup = false;
                    for (const auto& s : st.seen)
                        if (s == rr) { dup = true; break; }
                    if (dup) continue;
                    st.seen.push_back(rr);
                }
                if (x.is_null) continue;
                switch (pi.func) {
                    case AggFunc::COUNT_VAR:
                    case AggFunc::COUNT: ++st.count; break;
                    case AggFunc::SUM:
                        ++st.count;
                        st.isum += x.is_str ? 0 : x.i;
                        break;
                    case AggFunc::MIN:
                        if (!st.has || x < st.best) st.best = x;
                        st.has = true;
                        break;
                    case AggFunc::MAX:
                        if (!st.has || st.best < x) st.best = x;
                        st.has = true;
                        break;
                    case AggFunc::COLLECT: st.bag.push_back(x); break;
                    default: break;
                }
            }
        }
        if (keys.empty() && key_order.empty()) {
            key_order.push_back(Row());
            col_order.push_back(Row());
            states.emplace_back(aggs.size());
        }
        for (size_t gi = 0; gi < key_order.size(); ++gi) {
            Row r;
            r.v.resize(ret.items.size(), Value::nul());
            size_t key_pos = 0, agg_pos = 0;
            for (size_t col = 0; col < ret.items.size(); ++col) {
                const ProjectionItem& pi = q.items[(size_t)ret.items[col]];
                if (pi.kind == ProjectionItem::AGG) {
                    r.v[col] = agg_result(pi, states[gi][agg_pos++]);
                } else if (pi.kind == ProjectionItem::COL) {
                    r.v[col] = (size_t)pi.col < col_order[gi].v.size() ? col_order[gi].v[(size_t)pi.col]
                                                                      : Value::nul();
                } else {
                    r.v[col] = key_pos < key_order[gi].v.size() ? key_order[gi].v[key_pos++] : Value::nul();
                }
            }
            out.push_back(std::move(r));
        }
        if (cnt) cnt->rows_agg_out += out.size();
    }
    if (distinct && ret.distinct) {
        std::unordered_map<Row, size_t, RowHash> seen;
        std::vector<Row> keep;
        for (auto& r : out)
            if (seen.emplace(r, 0).second) keep.push_back(std::move(r));
        out = std::move(keep);
    }
    if (sort && q.order_by >= 0) {
        const Clause& ob = q.clauses[q.order_by];
        std::stable_sort(out.begin(), out.end(), [&](const Row& a, const Row& b) {
            for (size_t k = 0; k < ob.order_items.size(); ++k) {
                const Value& x = a.v[ob.order_items[k]];
                const Value& y = b.v[ob.order_items[k]];
                if (x == y) continue;
                const bool less = x < y;
                return k < ob.order_desc.size() && ob.order_desc[k] ? !less : less;
            }
            for (size_t c = 0; c < a.v.size(); ++c) {
                if (a.v[c] == b.v[c]) continue;
                return a.v[c] < b.v[c];
            }
            return false;
        });
    }
    size_t lo = 0, hi = out.size();
    if (apply_limit && q.skip >= 0) lo = std::min(hi, (size_t)std::max<int64_t>(0, q.clauses[q.skip].skip_n));
    if (apply_limit && q.limit >= 0)
        hi = std::min(hi, lo + (size_t)std::max<int64_t>(0, q.clauses[q.limit].limit_n));
    if (row_limit >= 0) hi = std::min(hi, lo + (size_t)row_limit);
    res->ncols = result_columns(q);
    res->data.clear();
    for (size_t r = lo; r < hi; ++r) res->data.insert(res->data.end(), out[r].v.begin(), out[r].v.end());
    res->labels.clear();
    for (int32_t it : ret.items) res->labels.push_back(q.items[it].label);
}

}  // namespace

// ---------------------------------------------------------------- public interface
QueryExec::QueryExec(const GraphStore& g, std::string* err, TargetDistCache* tdc,
                     const Graph* base)
    : g_(g), tdc_(tdc), base_(base) { (void)err; }

bool QueryExec::enumerate(const Query& q, std::vector<Env>* out, Counters* cnt, long long,
                          Drv* drv, std::string* err) {
    Candidate c(g_, q, cnt, tdc_);
    c.rows.push_back(Env{std::vector<uint32_t>(q.vars.size(), kNoVal)});
    c.arcs.push_back({});
    c.cols.emplace_back();
    // the per-matching reference is the pattern, before any WITH / RETURN
    if (!c.run(false, true)) {
        if (err) *err = "stage plan of " + q.id;
        return false;
    }
    if (drv) {
        for (size_t i = 0; i < c.rows.size(); ++i)
            if (c.null_row[i]) drv->opt_null(0);
    }
    *out = std::move(c.rows);
    if (err) err->clear();
    return true;
}

void QueryExec::project_only(const GraphStore& g, const Query& q, const std::vector<Env>& envs,
                             CandidateResult* res, bool sort, bool distinct, bool apply_limit,
                             long long row_limit, Counters* cnt) {
    Candidate c(g, q, cnt, nullptr);   // projection only: no pattern enumeration
    c.rows = envs;
    c.arcs.assign(envs.size(), {});
    c.cols.assign(envs.size(), Row());
    if (!c.run(true, false)) {
        res->ncols = 0;
        res->data.clear();
        res->labels.clear();
        return;
    }
    project_rows(g, q, c.rows, &c.arcs, &c.cols, res, sort, distinct, apply_limit, row_limit, cnt);
}

bool QueryExec::run(const Query& q, CandidateResult* res, Counters* cnt) {
    // A pure COUNT(*) pattern is answered by the base's own DPiso + LFTJ pipeline: the
    // fallback is required to cost no more than per-query execution, and counting through the
    // v3 growth materialises one row per matching (the k64 blocks).
    //
    // The base pays a fixed filter / query-graph cost, so it is used when the pattern is wide
    // (|V_q| >= 32, the size tests) or when every class is a large fraction of the graph: there
    // a per-candidate walk scans and grows from hundreds of roots, while the base's filtered
    // candidate lists prune first.  Small classes (Figeys, ~20 vertices per label) are cheaper
    // through the v3 growth.
    if (base_ != nullptr) {
        int nv = 0;
        double min_card = 1e300;
        for (size_t v = 0; v < q.vars.size(); ++v) {
            if (q.vars[v].kind != PatternVar::VERTEX) continue;
            ++nv;
            min_card = std::min(min_card, var_root_card(g_, q, (int32_t)v));
        }
        const bool wide = nv >= 32 || min_card >= 256.0;
        uint64_t n = 0;
        std::string why;
        if (wide && base_count_pure(base_, q, &n, &why)) {
            if (cnt) cnt->solutions += n;
            res->ncols = 1;
            res->data.clear();
            res->data.push_back(Value::num((int64_t)n));
            res->labels.clear();
            res->labels.push_back(q.items.empty() || q.items[0].label.empty() ? "count(*)"
                                                                              : q.items[0].label);
            return true;
        }
    }
    Candidate c(g_, q, cnt, tdc_);
    c.rows.push_back(Env{std::vector<uint32_t>(q.vars.size(), kNoVal)});
    c.arcs.push_back({});
    c.cols.emplace_back();
    if (!c.run()) return false;
    if (cnt) cnt->solutions += c.rows.size();
    project_rows(g_, q, c.rows, &c.arcs, &c.cols, res, true, true, true, -1, cnt);
    return true;
}

bool QueryExec::run_from_bound(const Query& q, int32_t start_clause, const std::vector<Env>& in,
                               CandidateResult* res, Counters* cnt,
                               std::vector<Env>* out_matchings) {
    Candidate c(g_, q, cnt, tdc_);
    c.rows = in;
    c.arcs.assign(in.size(), {});
    c.cols.assign(in.size(), Row());
    c.null_row.assign(in.size(), 0);
    if (!c.run_bound(start_clause)) return false;
    if (cnt) cnt->solutions += c.rows.size();
    if (out_matchings) *out_matchings = c.rows;
    project_rows(g_, q, c.rows, &c.arcs, &c.cols, res, true, true, true, -1, cnt);
    return true;
}

bool QueryExec::run_from(const Query& q, int32_t start_clause, const std::vector<Env>& in,
                         CandidateResult* res, Counters* cnt, std::vector<Env>* out_matchings) {
    Candidate c(g_, q, cnt, tdc_);
    c.rows = in;
    c.arcs.assign(in.size(), {});
    c.cols.assign(in.size(), Row());
    c.null_row.assign(in.size(), 0);
    if (!c.run_from(start_clause)) return false;
    if (cnt) cnt->solutions += c.rows.size();
    if (out_matchings) *out_matchings = c.rows;
    project_rows(g_, q, c.rows, &c.arcs, &c.cols, res, true, true, true, -1, cnt);
    return true;
}

}  // namespace v3
