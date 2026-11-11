// ECHO v3 round execution: M1/M2 alignment and partition, M3/M4 masked evaluation of the
// shared round, M5 demultiplex, M6 fallback.
//
// Every envelope is evaluated once (v3_match writes the words of the skeleton with the exact
// per-member mask).  M5 demultiplexes as the words arrive: every word goes to the members its
// mask names, and each member's matchings are collected in its own sink (the per-candidate
// reference collects them the same way), so the round does not hold the envelope's words a
// second time.  The member's RETURN is applied to its matchings exactly as the reference applies
// its own (v3_search.cpp project_rows).  Candidates the alignment leaves to the fallback are
// evaluated alone by the per-candidate executor; a candidate identical to an earlier one reuses
// that one's output (M1 dedup).
#include "v3_exec.h"
#include "v3_depth_chain.h"
#include "v3_point.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>

#include "v3_executor.h"
#include "v3_match.h"

namespace v3 {

Executor::Executor(const GraphStore& g, const Schema& s, Options opt) : g_(g), s_(s), opt_(opt) {}

std::vector<std::string> member_value_names(const Query& q) {
    std::vector<std::string> names;
    auto add = [&](const std::string& n) {
        if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    };
    for (const auto& pe : q.edges)
        if (!pe.var_name.empty() && q.var_index(pe.var_name) >= 0) add(pe.var_name);
    // a vertex written by a clause is part of the member's value order even without an edge
    for (const auto& c : q.clauses) {
        if (c.kind != ClauseKind::MATCH && c.kind != ClauseKind::OPTIONAL_MATCH) continue;
        for (int32_t v : c.vars) add(q.vars[(size_t)v].name);
    }
    for (const auto& pe : q.edges)
        for (int32_t v : {pe.from, pe.to}) add(q.vars[(size_t)v].name);
    return names;
}

int32_t member_var_pos(const EnvelopeSpec& E, size_t m, const Query& q, int32_t var) {
    if (var < 0 || m >= E.vmap.size()) return -1;
    const std::vector<std::string> names = member_value_names(q);
    const std::vector<int32_t>& vm = E.vmap[m];
    for (size_t s = 1; s < vm.size() && s - 1 < names.size(); ++s)
        if (names[s - 1] == q.vars[(size_t)var].name) return vm[s];
    return -1;
}

namespace {

// Canonical variable numbering of M1 (first use in clause order): two candidates with the same
// canonical key bind the same values to the same canonical variable, whatever their names.
std::vector<int32_t> canonical_ids(const Query& q) {
    std::vector<int32_t> remap(q.vars.size(), -1);
    int next = 0;
    auto rid = [&](int32_t v) {
        if (v < 0) return;
        if (remap[(size_t)v] < 0) remap[(size_t)v] = next++;
    };
    for (const auto& c : q.clauses) {
        if (c.kind == ClauseKind::MATCH || c.kind == ClauseKind::OPTIONAL_MATCH) {
            for (int32_t e : c.edges) {
                const PatternEdge& pe = q.edges[(size_t)e];
                rid(pe.from);
                rid(pe.to);
                if (!pe.var_name.empty() && pe.lo == 1 && pe.hi == 1) rid(q.var_index(pe.var_name));
            }
        } else {
            for (int32_t it : c.items) {
                const ProjectionItem& pi = q.items[(size_t)it];
                if (pi.kind == ProjectionItem::KEY) rid(pi.var);
                else if (pi.kind == ProjectionItem::AGG) rid(pi.arg_var);
            }
        }
    }
    return remap;
}

uint64_t matching_digest(const std::vector<uint32_t>& v) {
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t x : v) {
        for (size_t k = 0; k < sizeof x; ++k)
            h = (h ^ (unsigned char)((x >> (8 * k)) & 0xFFu)) * 1099511628211ULL;
    }
    return h;
}

// A masked word can carry a member whose relationship variable is not in st.rels: the
// skeleton edge was shared with members that have no variable on it, and the edge itself was
// checked as a close edge without keeping one row.  When the variable is required, recover the
// relationship from the two bound endpoints; if no row satisfies the member's type, direction
// and fixed hop range, the matching is not valid for that member and is dropped.
bool bind_missing_rel(const GraphStore& g, const Query& q, const PatternEdge& pe, Env* e) {
    const int32_t qv = q.var_index(pe.var_name);
    if (qv < 0) return true;
    if (e->v[(size_t)pe.from] == kNoVal || e->v[(size_t)pe.to] == kNoVal) return false;
    if (pe.lo != 1 || pe.hi != 1) return false;
    const AdjView ad = g.all();
    const auto& gr = g.all_rels();
    const uint32_t a = e->v[(size_t)pe.from], b = e->v[(size_t)pe.to];
    // all() is walked from pe.from to pe.to, so a stored row in that direction has dir = 1.
    // A reversed pattern still has pe.from/pe.to in its own order; the walk from a to b is what
    // the variable binds.
    for (uint32_t j = (*ad.off)[a]; j < (*ad.off)[a + 1]; ++j) {
        if ((*ad.nbr)[j] != b) continue;
        const uint32_t row = gr[j];
        if (!pe.any_dir && (uint32_t)g.edge_dir(row) != 1u) continue;
        if (!pe.types.empty()) {
            const int32_t t = g.edge_type(row);
            bool ok = false;
            for (const auto& ty : pe.types)
                if (g.schema().type_of(ty) == t) { ok = true; break; }
            if (!ok) continue;
        }
        e->v[(size_t)qv] = row;
        return true;
    }
    return false;
}

// The members' matchings, written by the masked walk (M5 in flight).
class Sink : public MaskSink {
public:
    Sink(const GraphStore& g, const Round& r, const EnvelopeSpec& env, bool digests)
        : g_(g), r_(r), env_(env), K_((int)env.members.size()), per_(env.members.size()),
          digests_(digests), dig_(env.members.size()), match_(env.members.size()) {}

    void put(const std::vector<uint32_t>& sk, const std::vector<uint32_t>& rels,
             const std::vector<uint64_t>& absent, const std::vector<uint64_t>& mask) override {
        const uint32_t T = mwords((uint32_t)K_);
        if (vpos_.empty()) {
            vpos_.resize((size_t)K_);
            for (int k = 0; k < K_; ++k) {
                const Query& q = r_.queries[(size_t)env_.members[(size_t)k]];
                vpos_[(size_t)k].assign(q.vars.size(), -1);
                for (size_t v = 0; v < q.vars.size(); ++v)
                    vpos_[(size_t)k][v] = member_var_pos(env_, (size_t)k, q, (int32_t)v);
            }
        }
        for (int m = 0; m < K_; ++m) {
            if (!((mask[(size_t)m >> 6] >> (m & 63)) & 1ULL)) continue;
            const Query& q = r_.queries[(size_t)env_.members[(size_t)m]];
            bool bad = false;
            Env e;
            e.v.assign(q.vars.size(), kNoVal);
            for (size_t v = 0; v < q.vars.size(); ++v) {
                if (q.vars[v].kind != PatternVar::VERTEX) continue;
                const int32_t sp = vpos_[(size_t)m][v];
                if (sp < 0 || sp >= (int32_t)sk.size()) continue;
                const bool null_here =
                    absent[(size_t)sp * T + (size_t)(m >> 6)] & (1ULL << (m & 63));
                if (sk[(size_t)sp] != kNil && !null_here) e.v[v] = sk[(size_t)sp];
            }
            for (size_t pe = 0; pe < q.edges.size(); ++pe) {
                const std::string& vn = q.edges[pe].var_name;
                if (vn.empty()) continue;
                const int32_t qv = q.var_index(vn);
                const int32_t se = env_.emap[(size_t)m][pe];
                if (qv >= 0 && se >= 0 && (size_t)se < rels.size() && rels[(size_t)se] != kNil) {
                    e.v[(size_t)qv] = rels[(size_t)se];
                } else if (qv >= 0 && se >= 0 && (size_t)se < env_.edges.size()) {
                    const PatternEdge& peq = q.edges[pe];
                    const bool opt = q.clauses[(size_t)peq.clause].kind == ClauseKind::OPTIONAL_MATCH;
                    if (!bind_missing_rel(g_, q, peq, &e) && !opt)
                        bad = true;
                }
            }
            if (bad) continue;
            if (std::getenv("V3_DEBUG_SINK") && count_ < 4) {
                std::fprintf(stderr, "[sink] m=%d ci=%d mask=%llx vals:", m, env_.members[(size_t)m],
                             (unsigned long long)mask[(size_t)m >> 6]);
                for (uint32_t x : e.v) std::fprintf(stderr, " %u", x);
                std::fprintf(stderr, " vpos:");
                for (int32_t x : vpos_[(size_t)m]) std::fprintf(stderr, " %d", x);
                std::fprintf(stderr, "\n");
            }
            if (digests_) {
                dig_[(size_t)m].push_back(matching_digest(e.v));
                match_[(size_t)m].push_back(e.v);
            }
            per_[(size_t)m].push_back(std::move(e));
            ++count_;
        }
    }
    uint64_t count() const { return count_; }
    size_t matches(int m) const { return per_[(size_t)m].size(); }
    std::vector<Env>& rows(int m) { return per_[(size_t)m]; }
    const std::vector<uint64_t>& digests(int m) const { return dig_[(size_t)m]; }
    const std::vector<std::vector<uint32_t>>& matchings(int m) const { return match_[(size_t)m]; }

private:
    const GraphStore& g_;
    const Round& r_;
    const EnvelopeSpec& env_;
    int K_;
    std::vector<std::vector<Env>> per_;
    std::vector<std::vector<int32_t>> vpos_;   // member -> query var -> skeleton position
    bool digests_ = false;
    std::vector<std::vector<uint64_t>> dig_;
    std::vector<std::vector<std::vector<uint32_t>>> match_;   // exact per-matching record (check)
    uint64_t count_ = 0;
};


// ---------------------------------------------------------------- masked aggregation
// An envelope whose members all end in one grouping RETURN is aggregated by (key, mask): members
// with the same grouping key and aggregates form an aggregation class, one state is kept per
// (key value, mask restricted to the class), and each member combines at the end the states whose
// mask names it.  count / sum / min / max / collect are distributive; a DISTINCT aggregate keeps
// its argument set and the merge takes the union.
struct AggSpec {
    bool valid = false;
    bool via_with = false;
    int32_t with_clause = -1;
    std::vector<int32_t> keys, aggs, ret_items;
    std::string sig;
};

std::string expr_sig(const Query& q, const ProjectionItem& it, const std::vector<int32_t>& vpos) {
    if (it.kind == ProjectionItem::KEY) {
        const int32_t p = it.var >= 0 && (size_t)it.var < vpos.size() ? vpos[(size_t)it.var] : -1;
        return "K" + std::to_string(p) + "." + it.prop;
    }
    if (it.kind == ProjectionItem::COL) return "C" + std::to_string(it.col);
    static const char* fn[] = {"*", "CV", "C", "S", "MIN", "MAX", "COL"};
    std::string s = std::string("A") + fn[(int)it.func] + (it.distinct ? "D" : "");
    if (it.arg_col >= 0) s += "c" + std::to_string(it.arg_col);
    else if (it.arg_var >= 0 && (size_t)it.arg_var < vpos.size())
        s += "p" + std::to_string(vpos[(size_t)it.arg_var]) + "." + it.arg_prop;
    else s += "-";
    return s;
}

AggSpec build_agg_spec(const Query& q, const std::vector<int32_t>& vpos) {
    AggSpec sp;
    if (q.return_clause < 0) return sp;
    // the last real pattern clause; ORDER_BY / SKIP / LIMIT are parsed as empty MATCH clauses
    int32_t lastp = -1;
    for (size_t c = 0; c < q.clauses.size(); ++c) {
        if ((int32_t)c == q.order_by || (int32_t)c == q.skip || (int32_t)c == q.limit) continue;
        const ClauseKind k = q.clauses[c].kind;
        if (k == ClauseKind::MATCH || k == ClauseKind::OPTIONAL_MATCH) lastp = (int32_t)c;
    }
    std::vector<int32_t> withs;
    for (size_t c = (size_t)lastp + 1; c < q.clauses.size(); ++c) {
        if ((int32_t)c == q.order_by || (int32_t)c == q.skip || (int32_t)c == q.limit) continue;
        if (q.clauses[c].kind == ClauseKind::WITH) withs.push_back((int32_t)c);
    }
    if (withs.size() > 1) return sp;   // a chain of WITH stages is not this operator's v1
    if (withs.size() == 1) {
        const int32_t wc = withs[0];
        // the only clause between the WITH and the RETURN must be pseudo clauses
        for (size_t c = (size_t)wc + 1; c < q.clauses.size(); ++c) {
            if ((int32_t)c == q.order_by || (int32_t)c == q.skip || (int32_t)c == q.limit) continue;
            if (q.clauses[c].kind != ClauseKind::RETURN) return sp;
        }
        bool has_agg = false;
        for (int32_t it : q.clauses[(size_t)wc].items) {
            if (q.items[(size_t)it].kind == ProjectionItem::AGG) { has_agg = true; sp.aggs.push_back(it); }
            else {
                if (q.items[(size_t)it].kind == ProjectionItem::COL) return sp;
                sp.keys.push_back(it);
            }
        }
        if (!has_agg) return sp;
        for (int32_t it : q.clauses[(size_t)q.return_clause].items)
            if (q.items[(size_t)it].kind == ProjectionItem::AGG) return sp;   // re-aggregation
        sp.via_with = true;
        sp.with_clause = wc;
        sp.ret_items = q.clauses[(size_t)q.return_clause].items;
    } else {
        const Clause& ret = q.clauses[(size_t)q.return_clause];
        bool has_agg = false;
        for (int32_t it : ret.items) {
            if (q.items[(size_t)it].kind == ProjectionItem::AGG) { has_agg = true; sp.aggs.push_back(it); }
            else {
                if (q.items[(size_t)it].kind == ProjectionItem::COL) return sp;
                sp.keys.push_back(it);
            }
        }
        if (!has_agg) return sp;
        sp.ret_items = ret.items;
    }
    sp.sig.clear();
    for (int32_t it : sp.keys) sp.sig += "K" + expr_sig(q, q.items[(size_t)it], vpos) + ";";
    sp.sig += "|";
    for (int32_t it : sp.aggs) sp.sig += expr_sig(q, q.items[(size_t)it], vpos) + ";";
    sp.valid = true;
    return sp;
}

struct AKey {
    std::vector<Value> key;
    uint64_t mask = 0;                 // K <= 64 (the envelope cap): one mask word
    bool operator==(const AKey& o) const { return key == o.key && mask == o.mask; }
};
struct AKeyHash {
    size_t operator()(const AKey& k) const {
        uint64_t h = 1469598103934665603ULL;
        for (const auto& v : k.key) h = (h ^ v.hash()) * 1099511628211ULL;
        h = (h ^ k.mask) * 1099511628211ULL;
        return (size_t)h;
    }
};

void merge_agg(AggState& d, const AggState& s, const ProjectionItem& pi) {
    if (pi.distinct) {
        for (const auto& r : s.seen) {
            bool dup = false;
            for (const auto& x : d.seen)
                if (x == r) { dup = true; break; }
            if (!dup) d.seen.push_back(r);
        }
        d.count = d.seen.size();
        d.isum = 0;
        d.has = false;
        d.bag.clear();
        for (const auto& r : d.seen) {
            const Value& v = r.v[0];
            if (v.is_null) continue;
            switch (pi.func) {
                case AggFunc::SUM: d.isum += v.is_str ? 0 : v.i; break;
                case AggFunc::MIN: if (!d.has || v < d.best) d.best = v; d.has = true; break;
                case AggFunc::MAX: if (!d.has || d.best < v) d.best = v; d.has = true; break;
                case AggFunc::COLLECT: d.bag.push_back(v); break;
                default: break;
            }
        }
        return;
    }
    switch (pi.func) {
        case AggFunc::COUNT_STAR:
        case AggFunc::COUNT_VAR:
        case AggFunc::COUNT: d.count += s.count; break;
        case AggFunc::SUM: d.count += s.count; d.isum += s.isum; break;
        case AggFunc::MIN: if (s.has && (!d.has || s.best < d.best)) d.best = s.best, d.has = true; break;
        case AggFunc::MAX: if (s.has && (!d.has || d.best < s.best)) d.best = s.best, d.has = true; break;
        case AggFunc::COLLECT: d.bag.insert(d.bag.end(), s.bag.begin(), s.bag.end()); break;
    }
}

void finish_result(const Query& q, std::vector<Row>& rows, CandidateResult* res, bool sort,
                   bool distinct, bool apply_limit, long long row_limit) {
    const Clause& ret = q.clauses[(size_t)q.return_clause];
    if (distinct && ret.distinct) {
        std::unordered_map<Row, size_t, RowHash> seen;
        std::vector<Row> keep;
        for (auto& r : rows)
            if (seen.emplace(r, 0).second) keep.push_back(std::move(r));
        rows = std::move(keep);
    }
    if (sort && q.order_by >= 0) {
        const Clause& ob = q.clauses[(size_t)q.order_by];
        std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
            for (size_t k = 0; k < ob.order_items.size(); ++k) {
                const Value& x = a.v[(size_t)ob.order_items[k]];
                const Value& y = b.v[(size_t)ob.order_items[k]];
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
    size_t lo = 0, hi = rows.size();
    if (apply_limit && q.skip >= 0) lo = std::min(hi, (size_t)std::max<int64_t>(0, q.clauses[(size_t)q.skip].skip_n));
    if (apply_limit && q.limit >= 0)
        hi = std::min(hi, lo + (size_t)std::max<int64_t>(0, q.clauses[(size_t)q.limit].limit_n));
    if (row_limit >= 0) hi = std::min(hi, lo + (size_t)row_limit);
    res->ncols = result_columns(q);
    res->data.clear();
    for (size_t r = lo; r < hi; ++r) res->data.insert(res->data.end(), rows[r].v.begin(), rows[r].v.end());
    res->labels.clear();
    for (int32_t it : ret.items) res->labels.push_back(q.items[(size_t)it].label);
}

class AggSink : public MaskSink {
public:
    AggSink(const GraphStore& g, const Round& r, const EnvelopeSpec& env, Executor::Options opt)
        : g_(g), r_(r), env_(env), opt_(opt), K_((int)env.members.size()),
          vpos_(env.members.size()), specs_(env.members.size()), acc_(env.members.size()),
          sol_(env.members.size(), 0) {
        for (int m = 0; m < K_; ++m) {
            const Query& q = r_.queries[(size_t)env_.members[(size_t)m]];
            vpos_[(size_t)m].assign(q.vars.size(), -1);
            for (size_t v = 0; v < q.vars.size(); ++v)
                vpos_[(size_t)m][v] = member_var_pos(env_, (size_t)m, q, (int32_t)v);
            specs_[(size_t)m] = build_agg_spec(q, vpos_[(size_t)m]);
        }
        // aggregation classes: identical grouping keys and aggregates
        for (int m = 0; m < K_; ++m) {
            if (!specs_[(size_t)m].valid) continue;
            int ci = -1;
            for (size_t c = 0; c < classes_.size(); ++c)
                if (classes_[c].sig == specs_[(size_t)m].sig) ci = (int)c;
            if (ci < 0) {
                Class nc;
                nc.sig = specs_[(size_t)m].sig;
                nc.mask = 0;
                nc.k.mask = 0;
                classes_.push_back(std::move(nc));
                ci = (int)classes_.size() - 1;
            }
            classes_[(size_t)ci].members.push_back(m);
            classes_[(size_t)ci].mask |= 1ULL << m;
        }
        for (auto& c : classes_) c.spec = specs_[(size_t)c.members[0]];
    }

    bool all_valid() const {
        for (const auto& s : specs_) if (!s.valid) return false;
        return !classes_.empty();
    }

    void put(const std::vector<uint32_t>& sk, const std::vector<uint32_t>& rels,
             const std::vector<uint64_t>& absent, const std::vector<uint64_t>& mask) override {
        const uint32_t T = mwords((uint32_t)K_);
        ++words_;
        for (int m = 0; m < K_; ++m)
            if ((mask[(size_t)(m >> 6)] >> (m & 63)) & 1ULL) ++sol_[(size_t)m];
        for (auto& c : classes_) {
            int rep = -1;
            for (int m : c.members)
                if ((mask[(size_t)(m >> 6)] >> (m & 63)) & 1ULL) { rep = m; break; }
            if (rep < 0) continue;
            Env& e = env_into(c.scratch, sk, rels, absent, rep);
            const Query& qrep = r_.queries[(size_t)env_.members[(size_t)rep]];
            AKey& k = c.k;
            k.key.clear();
            for (int32_t it : c.spec.keys) {
                const ProjectionItem& pi = qrep.items[(size_t)it];
                Value v;
                env_value(g_, qrep, e, pi.var, pi.prop, &v);
                k.key.push_back(v);
            }
            k.mask = mask[0] & c.mask;
            if (T != 1) { /* the envelope cap keeps K <= 64; a wider class keeps word 0 */ }
            auto f = c.index.find(k);
            size_t si;
            if (f == c.index.end()) {
                si = c.keys.size();
                c.index.emplace(k, si);
                c.keys.push_back(k);
                c.reps.push_back(e);
                c.states.emplace_back(c.spec.aggs.size());
                for (size_t a = 0; a < c.spec.aggs.size(); ++a) c.states[si][a] = AggState();
            } else {
                si = f->second;
            }
            const Query& q = r_.queries[(size_t)env_.members[(size_t)rep]];
            for (size_t a = 0; a < c.spec.aggs.size(); ++a) {
                const ProjectionItem& pi = q.items[(size_t)c.spec.aggs[a]];
                AggState& st = c.states[si][a];
                if (pi.func == AggFunc::COUNT_STAR) { ++st.count; continue; }
                Value x;
                env_value(g_, q, e, pi.arg_var, pi.arg_is_prop ? pi.arg_prop : "", &x);
                if (pi.distinct && !x.is_null) {
                    Row rr;
                    rr.v.push_back(x);
                    bool dup = false;
                    for (const auto& s : st.seen) if (s == rr) { dup = true; break; }
                    if (dup) continue;
                    st.seen.push_back(rr);
                }
                if (x.is_null) continue;
                switch (pi.func) {
                    case AggFunc::COUNT_VAR:
                    case AggFunc::COUNT: ++st.count; break;
                    case AggFunc::SUM: ++st.count; st.isum += x.is_str ? 0 : x.i; break;
                    case AggFunc::MIN: if (!st.has || x < st.best) st.best = x; st.has = true; break;
                    case AggFunc::MAX: if (!st.has || st.best < x) st.best = x; st.has = true; break;
                    case AggFunc::COLLECT: st.bag.push_back(x); break;
                    default: break;
                }
            }
            ++updates_;
        }
    }

    uint64_t words() const { return words_; }
    uint64_t updates() const { return updates_; }
    uint64_t sol(int m) const { return sol_[(size_t)m]; }
    size_t states() const {
        size_t n = 0;
        for (const auto& c : classes_) n += c.keys.size();
        return n;
    }

    // merge the class states into each member's accumulator
    void finalize() {
        for (size_t ci = 0; ci < classes_.size(); ++ci) {
            Class& c = classes_[ci];
            for (size_t si = 0; si < c.keys.size(); ++si) {
                const AKey& k = c.keys[si];
                for (int m : c.members) {
                    if (!((k.mask >> m) & 1ULL)) continue;
                    Row kr;
                    kr.v = k.key;
                    auto& mg = acc_[(size_t)m][kr];
                    if (mg.st.empty()) {
                        mg.st.assign(c.spec.aggs.size(), AggState());
                        mg.rep = c.reps[si];
                    }
                    for (size_t a = 0; a < c.spec.aggs.size(); ++a)
                        merge_agg(mg.st[a], c.states[si][a],
                                  r_.queries[(size_t)env_.members[(size_t)m]].items[(size_t)c.spec.aggs[a]]);
                }
            }
        }
    }

    CandidateResult result(int m) {
        CandidateResult res;
        const Query& q = r_.queries[(size_t)env_.members[(size_t)m]];
        const AggSpec& sp = specs_[(size_t)m];
        const Clause& ret = q.clauses[(size_t)q.return_clause];
        std::vector<Row> rows;
        auto emit = [&](const Row& key, MemberGroup& mg) {
            Row r;
            r.v.resize(ret.items.size(), Value::nul());
            if (sp.via_with) {
                Row wr;
                wr.v.assign(q.items.size(), Value::nul());
                size_t kp = 0, ap = 0;
                for (int32_t it : sp.keys)
                    if (kp < key.v.size()) wr.v[(size_t)it] = key.v[kp++];
                for (int32_t it : sp.aggs) wr.v[(size_t)it] = agg_result(q.items[(size_t)it], mg.st[ap++]);
                for (size_t col = 0; col < ret.items.size(); ++col) {
                    const ProjectionItem& pi = q.items[(size_t)ret.items[col]];
                    if (pi.kind == ProjectionItem::COL) {
                        if (pi.col >= 0 && (size_t)pi.col < wr.v.size()) r.v[col] = wr.v[(size_t)pi.col];
                    } else if (pi.kind == ProjectionItem::KEY) {
                        Value v;
                        env_value(g_, q, mg.rep, pi.var, pi.prop, &v);
                        r.v[col] = v;
                    }
                }
            } else {
                size_t kp = 0, ap = 0;
                for (size_t col = 0; col < ret.items.size(); ++col) {
                    const ProjectionItem& pi = q.items[(size_t)ret.items[col]];
                    if (pi.kind == ProjectionItem::AGG) r.v[col] = agg_result(pi, mg.st[ap++]);
                    else if (kp < key.v.size()) r.v[col] = key.v[kp++];
                }
            }
            rows.push_back(std::move(r));
        };
        for (auto& kv : acc_[(size_t)m]) emit(kv.first, kv.second);
        // A global aggregation over an empty input still returns one row (count 0, the other
        // aggregates null / the empty collect).  The accumulator is empty only when no word of
        // this member survived, so the identity row is the whole result; a grouped aggregation
        // returns no row instead.
        if (rows.empty() && sp.keys.empty()) {
            MemberGroup empty;
            empty.st.assign(sp.aggs.size(), AggState());
            emit(Row(), empty);
        }
        finish_result(q, rows, &res, opt_.sort, opt_.distinct, opt_.apply_limit, opt_.row_limit);
        return res;
    }

private:
    struct MemberGroup {
        Env rep;
        std::vector<AggState> st;
    };
    struct Class {
        std::string sig;
        std::vector<int> members;
        uint64_t mask = 0;
        Env scratch;
        AKey k;
        AggSpec spec;
        std::unordered_map<AKey, size_t, AKeyHash> index;
        std::vector<AKey> keys;
        std::vector<Env> reps;
        std::vector<std::vector<AggState>> states;
    };

    Env& env_into(Env& e, const std::vector<uint32_t>& sk, const std::vector<uint32_t>& rels,
                  const std::vector<uint64_t>& absent, int m) const {
        const Query& q = r_.queries[(size_t)env_.members[(size_t)m]];
        const uint32_t T = mwords((uint32_t)K_);
        e.v.assign(q.vars.size(), kNoVal);
        for (size_t v = 0; v < q.vars.size(); ++v) {
            if (q.vars[v].kind != PatternVar::VERTEX) continue;
            const int32_t sp = vpos_[(size_t)m][v];
            if (sp < 0 || sp >= (int32_t)sk.size()) continue;
            const bool null_here = absent[(size_t)sp * T + (size_t)(m >> 6)] & (1ULL << (m & 63));
            if (sk[(size_t)sp] != kNil && !null_here) e.v[v] = sk[(size_t)sp];
        }
        for (size_t pe = 0; pe < q.edges.size(); ++pe) {
            if (q.edges[pe].var_name.empty()) continue;
            const int32_t qv = q.var_index(q.edges[pe].var_name);
            const int32_t se = env_.emap[(size_t)m][pe];
            if (qv >= 0 && se >= 0 && (size_t)se < rels.size() && rels[(size_t)se] != kNil)
                e.v[(size_t)qv] = rels[(size_t)se];
        }
        return e;
    }
    Env env_for(const std::vector<uint32_t>& sk, const std::vector<uint32_t>& rels,
                const std::vector<uint64_t>& absent, int m) const {
        const Query& q = r_.queries[(size_t)env_.members[(size_t)m]];
        const uint32_t T = mwords((uint32_t)K_);
        Env e;
        e.v.assign(q.vars.size(), kNoVal);
        for (size_t v = 0; v < q.vars.size(); ++v) {
            if (q.vars[v].kind != PatternVar::VERTEX) continue;
            const int32_t sp = vpos_[(size_t)m][v];
            if (sp < 0 || sp >= (int32_t)sk.size()) continue;
            const bool null_here = absent[(size_t)sp * T + (size_t)(m >> 6)] & (1ULL << (m & 63));
            if (sk[(size_t)sp] != kNil && !null_here) e.v[v] = sk[(size_t)sp];
        }
        for (size_t pe = 0; pe < q.edges.size(); ++pe) {
            if (q.edges[pe].var_name.empty()) continue;
            const int32_t qv = q.var_index(q.edges[pe].var_name);
            const int32_t se = env_.emap[(size_t)m][pe];
            if (qv >= 0 && se >= 0 && (size_t)se < rels.size() && rels[(size_t)se] != kNil)
                e.v[(size_t)qv] = rels[(size_t)se];
        }
        return e;
    }

    const GraphStore& g_;
    const Round& r_;
    const EnvelopeSpec& env_;
    Executor::Options opt_;
    int K_;
    std::vector<std::vector<int32_t>> vpos_;
    std::vector<AggSpec> specs_;
    std::vector<Class> classes_;
    std::vector<std::unordered_map<Row, MemberGroup, RowHash>> acc_;
    std::vector<uint64_t> sol_;   // per member: complete matchings seen by the masked walk
    uint64_t words_ = 0, updates_ = 0;
};

}  // namespace

// One depth-masked count expansion for a prefix-chain batch.  The frontier is shared by every
// member whose length bound is at least the current depth; a member is answered at the depth
// where its own bound ends.  The count vector aggregates all walks that end at the same vertex,
// so the intermediate tuples are aggregate states, not materialised paths.  This is exact for
// count(*) over WALK semantics, which is the VLPQ batch.
static bool run_depth_chain_count(const GraphStore& g, const Round& r, const DepthChainSpec& spec,
                                  std::vector<CandidateOut>* out, Counters* cnt,
                                  RoundPlan* plan, std::string* err) {
    if (!spec.ok || spec.members.empty()) {
        if (err) *err = "depth-chain spec empty";
        return false;
    }
    const uint32_t nv = g.num_vertices();
    const AdjView ad = g.typed((uint32_t)spec.type_id);
    const GraphArray<uint32_t>& roots = g.vertices_of((uint32_t)spec.label_id);
    std::vector<uint64_t> cur(nv, 0), nxt(nv, 0);
    std::vector<uint32_t> active(roots.begin(), roots.end()), next_active;
    std::vector<uint32_t> mark(nv, 0);
    for (uint32_t v : roots) cur[v] = 1;
    if (cnt) cnt->init_tuples += (uint64_t)roots.size();

    std::vector<int32_t> member_of_len((size_t)spec.max_len + 1, -1);
    for (size_t m = 0; m < spec.members.size(); ++m) {
        const int32_t len = spec.lengths[m];
        if (len <= 0 || len > spec.max_len) {
            if (err) *err = "depth-chain length out of range";
            return false;
        }
        member_of_len[(size_t)len] = (int32_t)m;
    }
    std::vector<uint64_t> counts(spec.members.size(), 0);
    if (plan) {
        plan->dc_counts.assign((size_t)spec.max_len + 1, 0);
        plan->dc_edges.assign((size_t)spec.max_len + 1, 0);
        plan->dc_states.assign((size_t)spec.max_len + 1, 0);
        plan->dc_logical_edges.assign((size_t)spec.max_len + 1, 0);
    }

    uint32_t gen = 0;
    uint64_t total_solutions = 0;
    for (int32_t d = 1; d <= spec.max_len; ++d) {
        ++gen;
        next_active.clear();
        uint64_t edges = 0, logical_edges = 0;
        for (uint32_t v : active) {
            const uint64_t c = cur[v];
            if (!c) continue;
            const uint32_t b = (*ad.off)[v], e = (*ad.off)[v + 1];
            uint64_t fdeg = 0;
            for (uint32_t j = b; j < e; ++j) {
                if ((*ad.dir)[j] != 1) continue;   // follow the CSV direction only
                const uint32_t w = (*ad.nbr)[j];
                nxt[w] += c;
                ++edges;
                ++fdeg;
                if (mark[w] != gen) {
                    mark[w] = gen;
                    next_active.push_back(w);
                }
            }
            logical_edges += c * fdeg;
        }
        uint64_t total = 0;
        for (uint32_t w : next_active) total += nxt[w];
        const int32_t mi = member_of_len[(size_t)d];
        if (mi >= 0) {
            counts[(size_t)mi] = total;
            total_solutions += total;
        }
        if (plan) {
            plan->dc_counts[(size_t)d] = total;
            plan->dc_edges[(size_t)d] = edges;
            plan->dc_states[(size_t)d] = active.size();
            plan->dc_logical_edges[(size_t)d] = logical_edges;
        }
        // The frontier before this expansion is consumed; the next vector (the old cur) is
        // already zero because every active entry was cleared when it stopped being current.
        for (uint32_t v : active) cur[v] = 0;
        cur.swap(nxt);
        active.swap(next_active);
        if (cnt) {
            cnt->edges_expanded += edges;
            cnt->tuples += (uint64_t)active.size();   // aggregate (depth, vertex) states
        }
    }

    for (size_t m = 0; m < spec.members.size(); ++m) {
        const int32_t ci = spec.members[m];
        const Query& q = r.queries[(size_t)ci];
        CandidateOut& co = out->at((size_t)ci);
        co = CandidateOut();
        co.ncols = 1;
        co.data.push_back(Value::num((int64_t)counts[m]));
        co.labels.push_back(q.items.empty() ? std::string("count(*)") : q.items[0].label);
        co.planned = true;
        co.aggregated = true;
        if (plan) plan->why[(size_t)ci] = "depth_chain";
        if (cnt) {
            cnt->rows_agg_in += (uint64_t)counts[m];
            cnt->rows_agg_out += 1;
        }
    }
    if (cnt) {
        // The batch tool's convention: `tuples` contains the complete matchings and its
        // intermediate count is tuples - solutions.  The aggregate states are already in
        // `tuples`; add the unmaterialised result count so that subtraction leaves the states.
        cnt->tuples += total_solutions;
        cnt->solutions += total_solutions;
    }
    return true;
}

bool Executor::run(const Round& r, std::vector<CandidateOut>* out, Counters* cnt, RoundPlan* plan,
                   std::string* err) {
    // A round whose every candidate is a constant-anchored vertex or one fixed typed hop is
    // executed by the shared point plan compiled with the round.  ECHO and the per-query
    // baseline enter the same function, so the batching is part of the execution底座 rather
    // than a private ECHO shortcut.
    double point_plan_sec = 0.0, point_exec_sec = 0.0;
    if (run_point_round(g_, r, out, cnt, plan, &point_plan_sec, &point_exec_sec, err)) {
        stats.plan_sec = point_plan_sec;
        stats.env_sec = 0.0;
        stats.fallback_sec = point_exec_sec;
        stats.fallbacks = plan->fallback.size();
        stats.replays = 0;
        for (int32_t ri : plan->replay)
            if (ri >= 0) ++stats.replays;
        return true;
    }
    const size_t k = r.queries.size();
    out->assign(k, CandidateOut());
    std::vector<uint64_t> sol_i(k, 0);   // per candidate, for M1 replay accounting
    tdc_.clear();                        // one target-distance cache per round
    if (r.tiny) {
        // Below the measured planning + C_S break-even the round is evaluated one candidate at
        // a time; the canonical replay map was prepared before the timed cell.  The per-query
        // baseline takes the same path, so the comparison is not inflated by ECHO's planner.
        const double tiny_t0 = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        plan->why.assign(k, "");
        plan->replay = r.tiny_replay;
        plan->fallback = r.tiny_uniq;
        QueryExec qe(g_, err, &tdc_);
        for (int32_t ci : r.tiny_uniq) {
            Counters c;
            CandidateResult res;
            if (!qe.run(r.queries[(size_t)ci], &res, &c)) {
                if (err) *err = "tiny " + r.queries[(size_t)ci].id;
                return false;
            }
            (*out)[(size_t)ci] = CandidateOut();
            (*out)[(size_t)ci].ncols = res.ncols;
            (*out)[(size_t)ci].data = std::move(res.data);
            (*out)[(size_t)ci].labels = std::move(res.labels);
            (*out)[(size_t)ci].planned = false;
            sol_i[(size_t)ci] = c.solutions;
            if (cnt) cnt->add(c);
            plan->why[(size_t)ci] = "tiny";
            ++stats.fallbacks;
        }
        for (size_t i = 0; i < k; ++i)
            if (plan->replay[i] >= 0) {
                out->at(i) = (*out)[(size_t)plan->replay[i]];
                sol_i[i] = sol_i[(size_t)plan->replay[i]];
                if (cnt) cnt->solutions += sol_i[i];
                ++stats.replays;
            }
        stats.plan_sec = 0.0;
        stats.fallback_sec = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count() - tiny_t0;
        return true;
    }
    const double plan_t0 = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    *plan = align_and_partition(g_, r, (uint32_t)k, opt_.serial_only);
    {
        const double plan_t1 = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        stats.plan_sec = plan_t1 - plan_t0;
        if (std::getenv("V3_DEBUG_TIME"))
            std::fprintf(stderr, "[plan] %.3fs envs=%zu fallback=%zu replay=%zu\n",
                         stats.plan_sec, plan->envelopes.size(), plan->fallback.size(),
                         plan->replay.size());
        if (std::getenv("V3_DEBUG_TIME")) std::fflush(stderr);
    }
    if (plan->depth_chain) {
        DepthChainSpec spec;
        if (!detect_depth_chain(g_, r, &spec) || !run_depth_chain_count(g_, r, spec, out, cnt, plan, err))
            return false;
        stats.envelopes = 1;
        stats.fallbacks = 0;
        stats.env_sec = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count() - plan_t0;
        stats.solutions = cnt ? cnt->solutions : 0;
        return true;
    }
    for (auto& env : plan->envelopes) {
        env.q.clear();
        for (int32_t ci : env.members) env.q.push_back(&r.queries[(size_t)ci]);
        if (std::getenv("V3_DEBUG_VMAP")) {
            for (size_t m = 0; m < env.members.size(); ++m) {
                const int32_t ci = env.members[m];
                const Query& q = r.queries[(size_t)ci];
                std::fprintf(stderr, "[vmap] member %zu ci %d q %s size %zu:", m, ci, q.id.c_str(),
                             env.vmap[m].size());
                for (int32_t x : env.vmap[m]) std::fprintf(stderr, " %d", x);
                std::fprintf(stderr, " names:");
                for (const auto& n : member_value_names(q)) std::fprintf(stderr, " %s", n.c_str());
                std::fprintf(stderr, "\n");
            }
        }
    }
    QueryExec qe(g_, err, &tdc_, opt_.base_graph);
    std::vector<int32_t> fallback = plan->fallback;
    for (size_t i = 0; i < k; ++i) {
        bool in_env = false;
        for (const auto& e : plan->envelopes)
            for (int32_t m : e.members) in_env = in_env || m == (int32_t)i;
        // Prefix members are handled by the prefix loop below; do not add them to the full
        // Fallback list (that reran every prefix candidate once more and overwrote its result).
        for (const auto& e : plan->prefix_envelopes)
            for (int32_t m : e.members) in_env = in_env || m == (int32_t)i;
        if (!in_env && plan->replay[i] < 0 &&
            std::find(fallback.begin(), fallback.end(), (int32_t)i) == fallback.end())
            fallback.push_back((int32_t)i);
    }
    // M3/M4 + M5: every envelope once, its words demultiplexed as they arrive
    const double env_all_t0 = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    for (auto& env : plan->envelopes) {
        ++stats.envelopes;
        const int K = (int)env.members.size();
        const bool dbg_time = std::getenv("V3_DEBUG_TIME") != nullptr;
        const double env_t0 = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const bool stage_dbg = std::getenv("V3_STAGE_PROFILE") != nullptr;
        double stage_construct = 0.0, stage_walk = 0.0, stage_finish = 0.0;
        if (dbg_time) {
            std::fprintf(stderr, "[env] K=%d budget=%.6f pos:", K, env.budget_sec);
            for (const auto& p : env.pos) std::fprintf(stderr, " %s", p.name.c_str());
            std::fprintf(stderr, " levels=%zu\n", env.levels.size());
            std::fflush(stderr);
        }
        if (opt_.enforce_budget && env.budget_sec > 0.0) {
            const double now = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            env.deadline = now;
            if (std::getenv("V3_DEBUG_BUDGET"))
                std::fprintf(stderr, "budget env K=%d B=%.9f work_limit=%.0f\n", K,
                             env.budget_sec, env.budget_sec / 1e-6);
        } else {
            env.deadline = 0.0;
        }
        // masked aggregation when every member ends in one grouping RETURN; otherwise the words
        // are demultiplexed and the per-candidate pipeline projects them
        std::unique_ptr<AggSink> agg;
        const double stage_c0 = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (opt_.masked_agg) {
            agg.reset(new AggSink(g_, r, env, opt_));
            if (!agg->all_valid()) agg.reset();
        }
        stage_construct = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count() - stage_c0;
        if (agg) {
            const uint64_t sol0 = cnt ? cnt->solutions : 0;
            if (err) err->clear();
            const double stage_w0 = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const bool agg_ok = MaskedMatch::run_impl(g_, s_, env, K, 0, agg.get(), cnt, err, &tdc_);
            stage_walk = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count() - stage_w0;
            if (!agg_ok) {
                if (err && *err == "budget") {
                    if (cnt) cnt->solutions = sol0;
                    for (int32_t ci : env.members) {
                        plan->why[(size_t)ci] = "budget";
                        plan->fallback.push_back(ci);
                        fallback.push_back(ci);
                    }
                    continue;
                }
                return false;
            }
            const double stage_f0 = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            agg->finalize();
            if (std::getenv("V3_DEBUG_AGG"))
                std::fprintf(stderr, "agg envelope K=%d words=%llu updates=%llu states=%zu\n", K,
                             (unsigned long long)agg->words(), (unsigned long long)agg->updates(),
                             agg->states());
            if (dbg_time) {
                const double t1 = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                std::fprintf(stderr, "[env done] agg K=%d wall=%.3f\n", K, t1 - env_t0);
                std::fflush(stderr);
            }
            for (int m = 0; m < K; ++m) {
                const int32_t ci = env.members[(size_t)m];
                sol_i[(size_t)ci] = agg->sol(m);
                CandidateResult res = agg->result(m);
                (*out)[(size_t)ci].ncols = res.ncols;
                (*out)[(size_t)ci].data = std::move(res.data);
                (*out)[(size_t)ci].labels = std::move(res.labels);
                (*out)[(size_t)ci].aggregated = true;
                (*out)[(size_t)ci].planned = true;
            }
            stage_finish = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count() - stage_f0;
            if (stage_dbg) {
                const double t1 = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                std::fprintf(stderr,
                             "[stage-env] K=%d agg=1 construct=%.6f walk=%.6f finish=%.6f total=%.6f\n",
                             K, stage_construct, stage_walk, stage_finish, t1 - env_t0);
                std::fflush(stderr);
            }
            continue;
        }
        Sink sink(g_, r, env, opt_.digests);
        const uint64_t sol0 = cnt ? cnt->solutions : 0;
        if (err) err->clear();
        const bool sink_c0 = true;
        (void)sink_c0;
        const double stage_w0 = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const bool sink_ok = MaskedMatch::run_impl(g_, s_, env, K, 0, &sink, cnt, err, &tdc_);
        stage_walk = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count() - stage_w0;
        if (!sink_ok) {
            if (err && *err == "budget") {
                // The partial matchings are not results: the fallback recomputes every member.
                if (cnt) cnt->solutions = sol0;
                for (int32_t ci : env.members) {
                    plan->why[(size_t)ci] = "budget";
                    fallback.push_back(ci);
                }
                continue;
            }
            return false;
        }
        if (dbg_time) {
            const double t1 = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            std::fprintf(stderr, "[env done] K=%d wall=%.3f\n", K, t1 - env_t0);
            std::fflush(stderr);
        }
        if (std::getenv("V3_DEBUG_MATCH"))
            for (int m = 0; m < K; ++m)
                std::fprintf(stderr, "member %d ci %d matches %zu\n", m, env.members[(size_t)m], sink.matches(m));
        const double stage_f0 = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        for (int m = 0; m < K; ++m) {
            const int32_t ci = env.members[(size_t)m];
            sol_i[(size_t)ci] = (uint64_t)sink.rows(m).size();
            const Query& q = r.queries[(size_t)ci];
            if (opt_.collect) {
                CandidateResult res;
                QueryExec::project_only(g_, q, sink.rows(m), &res, opt_.sort, opt_.distinct, opt_.apply_limit,
                                        opt_.row_limit, cnt);
                (*out)[(size_t)ci].ncols = res.ncols;
                (*out)[(size_t)ci].data = std::move(res.data);
                (*out)[(size_t)ci].labels = std::move(res.labels);
            }
            // The per-matching record of this member (v3_check's second assertion).  It was lost
            // when 5586a25 added the prefix loop; without it every planned candidate that returns
            // rows rather than an aggregate fails the digest comparison with zero matchings.
            if (opt_.digests) {
                (*out)[(size_t)ci].digests = sink.digests(m);
                (*out)[(size_t)ci].matchings = sink.matchings(m);
            }
            (*out)[(size_t)ci].planned = true;
        }
        stage_finish = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count() - stage_f0;
        if (stage_dbg) {
            const double t1 = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            std::fprintf(stderr,
                         "[stage-env] K=%d agg=0 construct=%.6f walk=%.6f finish=%.6f total=%.6f\n",
                         K, stage_construct, stage_walk, stage_finish, t1 - env_t0);
            std::fflush(stderr);
        }
    }
    // Common-prefix envelopes: the shared first MATCH is walked once; each member's private
    // suffix starts from the prefix rows it received (QueryExec::run_from).
    for (auto& env : plan->prefix_envelopes) {
        ++stats.envelopes;
        ++stats.prefix_envelopes;
        const int K = (int)env.members.size();
        // The prefix rows are not complete matchings, so they are never exposed as the check's
        // per-matching record; the suffix result is what the check compares.
        Sink sink(g_, r, env, false);
        const uint64_t sol0 = cnt ? cnt->solutions : 0;
        if (err) err->clear();
        if (!MaskedMatch::run_impl(g_, s_, env, K, 0, &sink, cnt, err, &tdc_)) {
            if (err && *err == "budget") {
                if (cnt) cnt->solutions = sol0;
                for (int32_t ci : env.members) {
                    plan->why[(size_t)ci] = "prefix_budget";
                    plan->fallback.push_back(ci);
                    fallback.push_back(ci);
                }
                continue;
            }
            return false;
        }
        if (std::getenv("V3_DEBUG_TIME")) {
            std::fprintf(stderr, "[prefix done] K=%d walks=%llu\n", K,
                         (unsigned long long)sink.count());
            std::fflush(stderr);
        }
        // The prefix rows are intermediate, not complete matchings: restore the solution
        // counter (the suffix adds the real matching count).
        if (cnt) cnt->solutions = sol0;
        for (int m = 0; m < K; ++m) {
            const int32_t ci = env.members[(size_t)m];
            const Query& q = r.queries[(size_t)ci];
            int32_t dep = env.prefix_end;
            if ((size_t)m < env.depart_clause.size()) {
                const int32_t dc = env.depart_clause[(size_t)m];
                if (dc >= 0) dep = dc;
                else if (dc == -2) dep = -1;   // the prefix covers the whole pattern
            }
            const std::vector<Env>& pre_rows = sink.rows(m);
            if (dep >= 0) {
                // Departure: the member's prefix is fixed; the base enumerator completes the
                // private suffix from the first MATCH, so edges and predicates it does not
                // share are evaluated under its own semantics.
                ++stats.departures;
                if (opt_.collect) {
                    CandidateResult res;
                    std::vector<Env> full;
                    QueryExec qe2(g_, err, &tdc_, opt_.base_graph);
                    Counters c2;
                    // The base re-enumerates the whole first MATCH from the prefix vertices.
                    // Relationship variables of the prefix are cleared and identical vertex
                    // prefixes are collapsed, otherwise every prefix relationship row would be
                    // multiplied by the base's own enumeration of the same relationships.
                    std::vector<Env> prefix_rows;
                    prefix_rows.reserve(pre_rows.size());
                    std::map<std::vector<uint32_t>, char> seen_prefix;
                    for (const Env& e : pre_rows) {
                        Env x = e;
                        for (size_t v = 0; v < q.vars.size(); ++v)
                            if (q.vars[v].kind == PatternVar::EDGE) x.v[(size_t)v] = kNoVal;
                        if (!seen_prefix.emplace(x.v, 0).second) continue;
                        prefix_rows.push_back(std::move(x));
                    }
                    if (!qe2.run_from_bound(q, dep, prefix_rows, &res, &c2,
                                            opt_.digests ? &full : nullptr)) {
                        if (err) *err = "prefix " + q.id;
                        return false;
                    }
                    if (cnt) cnt->add(c2);
                    sol_i[(size_t)ci] += c2.solutions;
                    if (opt_.digests) {
                        auto& mm = (*out)[(size_t)ci].matchings;
                        mm.clear();
                        mm.reserve(full.size());
                        for (const auto& e : full) mm.push_back(e.v);
                        auto& dg = (*out)[(size_t)ci].digests;
                        dg.clear();
                        dg.reserve(mm.size());
                        for (const auto& v : mm) dg.push_back(matching_digest(v));
                    }
                    (*out)[(size_t)ci].ncols = res.ncols;
                    (*out)[(size_t)ci].data = std::move(res.data);
                    (*out)[(size_t)ci].labels = std::move(res.labels);
                }
            } else {
                // The shared prefix covers the whole pattern: the rows are complete matchings.
                sol_i[(size_t)ci] += (uint64_t)pre_rows.size();
                if (cnt) cnt->solutions += (uint64_t)pre_rows.size();
                if (opt_.collect) {
                    CandidateResult res;
                    QueryExec::project_only(g_, q, pre_rows, &res, opt_.sort, opt_.distinct,
                                            opt_.apply_limit, opt_.row_limit, cnt);
                    (*out)[(size_t)ci].ncols = res.ncols;
                    (*out)[(size_t)ci].data = std::move(res.data);
                    (*out)[(size_t)ci].labels = std::move(res.labels);
                }
                if (opt_.digests) {
                    auto& mm = (*out)[(size_t)ci].matchings;
                    mm.clear();
                    mm.reserve(pre_rows.size());
                    for (const auto& e : pre_rows) mm.push_back(e.v);
                    auto& dg = (*out)[(size_t)ci].digests;
                    dg.clear();
                    dg.reserve(mm.size());
                    for (const auto& v : mm) dg.push_back(matching_digest(v));
                }
            }
            (*out)[(size_t)ci].planned = true;
            ++stats.prefix_members;
        }
    }
    stats.env_sec = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count() - env_all_t0;
    // M6 fallback: candidates the alignment leaves out, evaluated by the per-candidate executor
    const double fb_t0_all = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    for (int32_t ci : fallback) {
        if (plan->why[(size_t)ci].empty()) plan->why[(size_t)ci] = "not_planned";
        const bool dbg_time = std::getenv("V3_DEBUG_TIME") != nullptr;
        const double fb_t0 = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (dbg_time) {
            std::fprintf(stderr, "[fallback] %s\n", r.queries[(size_t)ci].id.c_str());
            std::fflush(stderr);
        }
        Counters c;
        CandidateResult res;
        if (!qe.run(r.queries[(size_t)ci], &res, &c)) {
            if (err) *err = "fallback " + r.queries[(size_t)ci].id;
            return false;
        }
        out->at((size_t)ci) = CandidateOut();
        sol_i[(size_t)ci] = c.solutions;
        if (opt_.collect) {
            (*out)[(size_t)ci].ncols = res.ncols;
            (*out)[(size_t)ci].data = std::move(res.data);
            (*out)[(size_t)ci].labels = std::move(res.labels);
        }
        (*out)[(size_t)ci].planned = false;
        cnt->add(c);
        ++stats.fallbacks;
        if (dbg_time) {
            const double t1 = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            std::fprintf(stderr, "[fallback done] %s wall=%.3f\n",
                         r.queries[(size_t)ci].id.c_str(), t1 - fb_t0);
            std::fflush(stderr);
        }
    }
    stats.fallback_sec = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count() - fb_t0_all;
    // identical candidates (M1 dedup) read the first one's output; the check's per-matching
    // record is carried over in the candidate's own variable order
    for (size_t i = 0; i < k; ++i)
        if (plan->replay[i] >= 0) {
            CandidateOut rep = (*out)[(size_t)plan->replay[i]];
            if (opt_.digests && !rep.matchings.empty()) {
                const Query& src = r.queries[(size_t)plan->replay[i]];
                const Query& dst = r.queries[i];
                const std::vector<int32_t> si = canonical_ids(src), di = canonical_ids(dst);
                std::vector<int32_t> src_of(dst.vars.size(), -1);
                for (size_t v = 0; v < dst.vars.size(); ++v)
                    for (size_t u = 0; u < src.vars.size(); ++u)
                        if (di[v] >= 0 && si[u] == di[v]) src_of[v] = (int32_t)u;
                std::vector<std::vector<uint32_t>> remapped;
                remapped.reserve(rep.matchings.size());
                for (const auto& row : rep.matchings) {
                    std::vector<uint32_t> x(dst.vars.size(), kNoVal);
                    for (size_t v = 0; v < dst.vars.size(); ++v)
                        if (src_of[v] >= 0 && (size_t)src_of[v] < row.size()) x[v] = row[(size_t)src_of[v]];
                    remapped.push_back(std::move(x));
                }
                rep.matchings = std::move(remapped);
            }
            (*out)[i] = std::move(rep);
            sol_i[i] = sol_i[(size_t)plan->replay[i]];
            if (cnt) cnt->solutions += sol_i[i];
            ++stats.replays;
        }
    return true;
}

}  // namespace v3
