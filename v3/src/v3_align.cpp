// M1: canonical form / dedup and round alignment; M2: envelope partition.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <sstream>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "v3_align.h"
#include "v3_depth_chain.h"
#include "v3_exec.h"

namespace v3 {

namespace {
std::string join(const std::vector<std::string>& v, const char* sep = ",") {
    std::string o;
    for (size_t k = 0; k < v.size(); ++k) {
        if (k) o += sep;
        o += v[k];
    }
    return o;
}
std::string val_str(const Value& v) {
    if (v.is_null) return "null";
    if (v.is_str) return "s:" + v.s;
    return "i:" + std::to_string(v.i);
}
std::vector<std::string> norm(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    return v;
}
std::string prop_str(const std::vector<std::pair<std::string, Value>>& ps) {
    std::vector<std::string> xs;
    for (const auto& p : ps) xs.push_back(p.first + "=" + val_str(p.second));
    std::sort(xs.begin(), xs.end());
    return join(xs, ";");
}
std::string atom_str(const Atom& a, const std::vector<int32_t>* remap) {
    auto r = [&](int32_t v) { return remap && v >= 0 && v < (int32_t)remap->size() ? (*remap)[v] : v; };
    std::string s = (a.col >= 0 ? "c" + std::to_string(a.col) : "v" + std::to_string(r(a.var))) + "." + a.prop + " " + a.cmp.op;
    if (a.cmp.rhs == Comparison::VAR) {
        s += " v" + std::to_string(r(a.cmp.var));
    } else if (a.cmp.op == "IN") {
        std::vector<std::string> xs;
        for (const auto& v : a.cmp.consts) xs.push_back(val_str(v));
        std::sort(xs.begin(), xs.end());
        s += "[" + join(xs) + "]";
    } else {
        for (const auto& v : a.cmp.consts) s += " " + val_str(v);
    }
    if (a.negated) s = "NOT(" + s + ")";
    return s;
}
}  // namespace

// Canonical text of a projection expression.  Two candidates whose CASE / COALESCE / arithmetic
// differs must not dedup to one: the replay would give the other candidate's values.
static std::string expr_key(const Query& q, const Expr& e, const std::vector<int32_t>& remap) {
    auto rid = [&](int32_t v) { return (v >= 0 && (size_t)v < remap.size() && remap[(size_t)v] >= 0)
                                         ? remap[(size_t)v] : v; };
    switch (e.kind) {
        case Expr::CONST: return "K" + val_str(e.c);
        case Expr::PROP: return "P" + std::to_string(rid(e.var)) + "." + e.prop;
        case Expr::VAR: return "V" + std::to_string(rid(e.var));
        case Expr::COL: return "C" + std::to_string(e.col);
        case Expr::LABEL: {
            std::string s = "L" + std::to_string(rid(e.var)) + "(";
            for (const auto& l : e.labels) s += l + "|";
            return s + ")";
        }
        case Expr::CMP:
        case Expr::AND:
        case Expr::OR:
        case Expr::COALESCE:
        case Expr::ARITH:
        case Expr::FUNC: {
            std::string s = std::to_string((int)e.kind) + e.op + "(";
            for (const auto& a : e.args) s += expr_key(q, a, remap) + ",";
            return s + ")";
        }
        case Expr::NOT:
        case Expr::IS_NULL: {
            std::string s = std::to_string((int)e.kind) + "(";
            for (const auto& a : e.args) s += expr_key(q, a, remap) + ",";
            return s + ")";
        }
        case Expr::CASE: {
            std::string s = "CASE(";
            for (const auto& a : e.args) s += expr_key(q, a, remap) + ",";
            return s + ")";
        }
        default: break;
    }
    (void)q;
    return "?";
}

// M1 canonical form: variables numbered by first use in clause order; everything that decides
// the result multiset and the output columns is covered, names are not.
std::string canonical_key(const Query& q) {
    std::vector<int32_t> remap(q.vars.size(), -1);
    int next = 0;
    auto rid = [&](int32_t v) {
        if (v < 0) return -1;
        if (remap[v] < 0) remap[v] = next++;
        return remap[v];
    };
    std::ostringstream o;
    o << "SEM" << (int)q.sem << ";";
    for (const auto& c : q.clauses) {
        o << (c.kind == ClauseKind::MATCH ? "MATCH"
             : c.kind == ClauseKind::OPTIONAL_MATCH ? "OPT"
             : c.kind == ClauseKind::WITH ? (c.items.empty() ? "WITH_PLAIN" : "WITH_AGG")
             : "RETURN");
        if (c.kind == ClauseKind::MATCH || c.kind == ClauseKind::OPTIONAL_MATCH) {
            for (int32_t e : c.edges) {
                const PatternEdge& pe = q.edges[e];
                o << " " << rid(pe.from) << (pe.any_dir ? "-" : ">") << rid(pe.to) << "["
                  << join(norm(pe.types), "|") << ":" << pe.lo << ".." << pe.hi << "]{"
                  << prop_str(pe.props) << "}";
                if (!pe.var_name.empty() && pe.lo == 1 && pe.hi == 1) {
                    const int32_t ev = q.var_index(pe.var_name);
                    if (ev >= 0) o << "E" << rid(ev);
                }
            }
            std::vector<std::string> ws;
            for (int32_t a : c.where) ws.push_back(atom_str(q.atoms[a], &remap));
            std::sort(ws.begin(), ws.end());
            for (const auto& s : ws) o << " WHERE " << s;
            std::vector<std::string> vs;
            for (size_t v = 0; v < q.vars.size(); ++v) {
                if (remap[v] < 0 || q.vars[v].kind != PatternVar::VERTEX) continue;
                vs.push_back("V" + std::to_string(remap[v]) + "(" + join(norm(q.vars[v].labels), "|") +
                             "){" + prop_str(q.vars[v].props) + "}");
            }
            std::sort(vs.begin(), vs.end());
            for (const auto& s : vs) o << " " << s;
        } else {
            if (c.distinct) o << " DISTINCT";
            std::vector<std::string> its;
            for (int32_t it : c.items) {
                const ProjectionItem& pi = q.items[it];
                if (pi.kind == ProjectionItem::KEY) {
                    its.push_back("K" + std::to_string(rid(pi.var)) + "." + pi.prop);
                } else if (pi.kind == ProjectionItem::AGG) {
                    static const char* fn[] = {"*", "CV", "C", "S", "MIN", "MAX", "COL"};
                    its.push_back(std::string("A") + fn[(int)pi.func] + (pi.distinct ? "D" : "") +
                                  (pi.arg_col >= 0 ? "c" + std::to_string(pi.arg_col)
                                                   : (pi.arg_var >= 0 ? "v" + std::to_string(rid(pi.arg_var)) + pi.arg_prop : "-")));
                } else if (pi.kind == ProjectionItem::COL) {
                    its.push_back("C" + std::to_string(pi.col));
                } else if (pi.kind == ProjectionItem::CONST) {
                    its.push_back("K" + pi.label);
                } else if (pi.kind == ProjectionItem::EXPR) {
                    its.push_back("E(" + (pi.expr >= 0 && (size_t)pi.expr < q.exprs.size()
                                              ? expr_key(q, q.exprs[(size_t)pi.expr], remap)
                                              : std::string("?")) + ")");
                } else {
                    its.push_back("X");
                }
            }
            for (const auto& s : its) o << " " << s << ";";
            std::vector<std::string> ws;
            for (int32_t a : c.where) ws.push_back(atom_str(q.atoms[a], &remap));
            for (int32_t a : c.pre_where) ws.push_back("POST" + atom_str(q.atoms[a], &remap));
            std::sort(ws.begin(), ws.end());
            for (const auto& s : ws) o << " WHERE " << s;
        }
    }
    if (q.limit >= 0 && (size_t)q.limit < q.clauses.size())
        o << " LIMIT" << q.clauses[(size_t)q.limit].limit_n;
    if (q.skip >= 0 && (size_t)q.skip < q.clauses.size())
        o << " SKIP" << q.clauses[(size_t)q.skip].skip_n;
    if (q.order_by >= 0 && (size_t)q.order_by < q.clauses.size()) {
        o << " ORDER";
        const Clause& oc = q.clauses[(size_t)q.order_by];
        for (size_t k = 0; k < oc.order_items.size(); ++k)
            o << " " << oc.order_items[k] << (k < oc.order_desc.size() && oc.order_desc[k] ? "d" : "a");
    }
    return o.str();
}

// ---------------------------------------------------------------- M1 / M2 alignment
namespace {

struct Placed {
    int32_t cand = -1;
    std::vector<std::string> vnames;   // the member's pattern variables in the envelope order
    std::vector<int32_t> vmap;   // query var -> skeleton pos (-1: local / not in the pattern)
    std::vector<int32_t> emap;   // query edge -> skeleton edge (-1: not on the skeleton)
    // private suffix starts here; -2: the shared prefix covers the whole pattern; -1: not set
    // (a placement used by the older common-prefix envelopes, which depart at prefix_end)
    int32_t depart_clause = -2;
};

struct Skeleton {
    std::vector<SkeletonPos> pos;
    std::vector<std::string> pos_names;
    std::vector<SkeletonEdge> edge;
    std::vector<int32_t> clause_level;   // clause -> pattern level (-1: WITH / RETURN)
    std::vector<int32_t> clause_nedges;  // pattern edges written by the clause
    std::vector<uint8_t> clause_opt;     // clause is OPTIONAL MATCH
    std::vector<uint8_t> level_opt;      // pattern level -> OPTIONAL MATCH
    const Query* seed_q = nullptr;       // the seed whose vertex order defines `pos`
    Semantics sem = Semantics::CYPHER;   // all members of one envelope share the semantics
    int32_t first_match = -1;
    std::vector<int32_t> members;
    // Topological envelopes: the common core plus a mask-partitioned private suffix trie.
    // `birth_parent` is the position that introduced a private position (-1: a root scan),
    // `birth_edge` the skeleton edge of that step (-1: root), `users` the member bits that
    // bind the position.  All users of a private position own its birth edge, so a member
    // never passes the level that binds a position it uses.
    bool topo = false;
    std::vector<int32_t> birth_parent, birth_edge;
    std::vector<uint32_t> users;
    std::vector<uint8_t> anchored;
    int find_edge(int u, int w) const {
        for (size_t k = 0; k < edge.size(); ++k)
            if (edge[k].u == u && edge[k].w == w) return (int)k;
        return -1;
    }
};

// The pattern level of every clause.  Consecutive plain MATCH clauses of one pipeline stage
// share a level: they are joins, and one walk roots at the bound side of the union, so a join
// written after a large pattern no longer forces the large class to be scanned first (IC12's
// `MATCH (c:Comment)-... MATCH (c)-[:HAS_CREATOR]->(friend)`).  A WITH / UNWIND / CALL boundary,
// an OPTIONAL MATCH, or the MATCH after one starts a new level, because those change which rows
// exist before the pattern is extended.
std::vector<int32_t> clause_levels(const Query& q) {
    std::vector<int32_t> cl(q.clauses.size(), -1);
    int32_t cur = -1;
    bool first = true;
    bool pipeline = false;   // a WITH / UNWIND / CALL since the last pattern clause
    bool after_opt = false;  // the previous pattern clause was OPTIONAL MATCH
    for (size_t c = 0; c < q.clauses.size(); ++c) {
        const ClauseKind k = q.clauses[c].kind;
        if (k != ClauseKind::MATCH && k != ClauseKind::OPTIONAL_MATCH) {
            if (k == ClauseKind::WITH) pipeline = true;
            continue;
        }
        if (first) {
            cur = 0;
            first = false;
            pipeline = false;
            after_opt = false;
        } else if (k == ClauseKind::OPTIONAL_MATCH) {
            ++cur;
            pipeline = false;
            after_opt = true;
        } else if (pipeline || after_opt) {
            ++cur;
            pipeline = false;
            after_opt = false;
        }
        cl[c] = cur;
    }
    return cl;
}

// The structural vertex order of a candidate: clause variables in clause order, then edge
// endpoints in edge order (the order place_member assigns skeleton positions for the seed).
std::vector<int32_t> pattern_vertex_order(const Query& q) {
    std::vector<int32_t> out;
    auto add = [&](int32_t v) {
        if (v >= 0 && std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    };
    for (const auto& c : q.clauses) {
        if (c.kind != ClauseKind::MATCH && c.kind != ClauseKind::OPTIONAL_MATCH) continue;
        for (int32_t v : c.vars) add(v);
    }
    for (const auto& e : q.edges) { add(e.from); add(e.to); }
    return out;
}

// The vertices of the clauses up to `end` (a common-prefix envelope uses only these).
std::vector<int32_t> vertex_order_upto(const Query& q, int32_t end) {
    std::vector<int32_t> out;
    auto add = [&](int32_t v) {
        if (v >= 0 && std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    };
    for (size_t c = 0; c < q.clauses.size(); ++c) {
        if (end >= 0 && (int32_t)c > end) break;
        const Clause& cl = q.clauses[c];
        if (cl.kind != ClauseKind::MATCH && cl.kind != ClauseKind::OPTIONAL_MATCH) continue;
        for (int32_t v : cl.vars) add(v);
    }
    for (const auto& e : q.edges) {
        if (end >= 0 && e.clause > end) continue;
        add(e.from);
        add(e.to);
    }
    return out;
}

// The vertices bound by the mandatory MATCH clauses.  An OPTIONAL subpattern does not enter
// the grouping bucket (paper §IV-A); it is aligned on top of this core.
std::vector<int32_t> required_vertex_order(const Query& q) {
    std::vector<int32_t> out;
    auto add = [&](int32_t v) {
        if (v >= 0 && std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    };
    for (const auto& c : q.clauses) {
        if (c.kind != ClauseKind::MATCH) continue;
        for (int32_t v : c.vars) add(v);
    }
    for (const auto& e : q.edges)
        if (e.clause >= 0 && (size_t)e.clause < q.clauses.size() &&
            q.clauses[(size_t)e.clause].kind == ClauseKind::MATCH) {
            add(e.from);
            add(e.to);
        }
    return out;
}

// Greedy vertex alignment used to map a member onto the seed's skeleton positions.  This is the
// same score as the grouping test; it is kept local so placement uses the alignment, not the
// candidate's spelling order (the two can differ: IC8 writes the anchored Person first or third).
std::vector<int32_t> align_to_seed(const Query& seed, const Query& q) {
    const std::vector<int32_t> sv = pattern_vertex_order(seed);
    const std::vector<int32_t> qv = pattern_vertex_order(q);
    auto deg = [](const Query& x, int32_t v) {
        int32_t d = 0;
        for (const auto& e : x.edges)
            if (e.from == v || e.to == v) ++d;
        return d;
    };
    auto has = [](const Query& x, int32_t a, int32_t b) {
        for (const auto& e : x.edges)
            if ((e.from == a && e.to == b) || (e.from == b && e.to == a)) return true;
        return false;
    };
    std::vector<int32_t> order(sv.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int32_t)i;
    std::stable_sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
        return deg(seed, sv[(size_t)a]) > deg(seed, sv[(size_t)b]);
    });
    std::vector<int32_t> q_of_pos(sv.size(), -1);
    std::vector<char> used(qv.size(), 0);
    for (int32_t sp : order) {
        const int32_t u = sv[(size_t)sp];
        int best = INT32_MIN, best_q = -1;
        for (size_t qi = 0; qi < qv.size(); ++qi) {
            if (used[qi]) continue;
            const int32_t v = qv[qi];
            int score = norm(seed.vars[(size_t)u].labels) == norm(q.vars[(size_t)v].labels) ? 1000 : 0;
            // The constant anchor is the root of the walk; never pair it with an unanchored
            // vertex that happens to carry the same label (IC11's p and friend are both Person).
            if (var_const_anchored(seed, u) != var_const_anchored(q, v)) score -= 100000;
            for (size_t p2 = 0; p2 < sv.size(); ++p2) {
                const int32_t q2 = q_of_pos[p2];
                if (q2 < 0) continue;
                score += (has(seed, u, sv[p2]) == has(q, v, q2)) ? 1 : -1;
            }
            if (score > best) { best = score; best_q = (int32_t)qi; }
        }
        if (best_q >= 0 && best > 0) {
            q_of_pos[(size_t)sp] = qv[(size_t)best_q];
            used[(size_t)best_q] = 1;
        }
    }
    return q_of_pos;
}

// Place candidate q's whole pattern on the skeleton (growing a copy); "" on success.  A joining
// candidate must have exactly the seed's clause sequence and, per MATCH / OPTIONAL clause, the
// same number of edges between the same skeleton positions.  Labels, relationship types,
// directions and hop ranges may differ: they are per-member tests of the envelope.
std::string place_member(const Query& q, Skeleton* sk, Placed* out, int32_t prefix_end = -1) {
    if (q.first_match < 0) return "no_match";
    const bool seed = sk->pos.empty() && sk->edge.empty();
    Skeleton loc = *sk;
    // The masked walk enforces one uniqueness rule for the whole envelope; candidates with a
    // different matching semantics stay in the fallback instead of sharing a skeleton.
    if (!seed && q.sem != sk->sem) return "semantics";
    Placed pl;
    pl.vmap.assign(q.vars.size(), -1);
    pl.emap.assign(q.edges.size(), -1);
    const std::vector<int32_t> clev = clause_levels(q);
    if (!seed) {
        const std::vector<int32_t> vo = prefix_end >= 0 ? vertex_order_upto(q, prefix_end)
                                                        : pattern_vertex_order(q);
        if (vo.size() != loc.pos.size()) return "vertex_count";
        if (!loc.seed_q) return "no_seed";
        if (prefix_end >= 0) {
            // The group key already fixed the first-MATCH topology; map its vertices by their
            // position in the shared prefix order (names may differ between members).
            const std::vector<int32_t> qv = vertex_order_upto(q, prefix_end);
            if (qv.size() != loc.pos.size()) return "vertex_count";
            for (size_t p = 0; p < qv.size(); ++p) pl.vmap[(size_t)qv[p]] = (int32_t)p;
        } else {
            const std::vector<int32_t> aligned = align_to_seed(*loc.seed_q, q);
            for (size_t p = 0; p < aligned.size(); ++p)
                if (aligned[p] >= 0) pl.vmap[(size_t)aligned[p]] = (int32_t)p;
        }
    }
    if (seed) {
        loc.clause_level = clev;
        loc.seed_q = &q;
        loc.sem = q.sem;
        loc.clause_nedges.assign(q.clauses.size(), 0);
        loc.clause_opt.assign(q.clauses.size(), 0);
        loc.first_match = q.first_match;
        int32_t maxlev = 0;
        for (int32_t lv : clev) maxlev = std::max(maxlev, lv);
        loc.level_opt.assign((size_t)maxlev + 1, 0);
        for (size_t ci = 0; ci < q.clauses.size(); ++ci) {
            if (prefix_end >= 0 && (int32_t)ci > prefix_end) break;
            const ClauseKind k = q.clauses[ci].kind;
            if (k != ClauseKind::MATCH && k != ClauseKind::OPTIONAL_MATCH) continue;
            if (clev[ci] >= 0) loc.level_opt[(size_t)clev[ci]] = k == ClauseKind::OPTIONAL_MATCH ? 1 : 0;
        }
    } else {
        // A member may split the same pattern over a different number of consecutive plain
        // MATCH clauses: those clauses share one level, so the edge count per clause is not a
        // member constraint.  The level sequence (and its OPTIONAL flags) must agree; every
        // candidate edge is then mapped to the skeleton edge at its own level.
        if (q.first_match != loc.first_match) return "first_match";
        for (size_t ci = 0; ci < q.clauses.size(); ++ci) {
            if (prefix_end >= 0 && (int32_t)ci > prefix_end) break;
            const ClauseKind k = q.clauses[ci].kind;
            if (k != ClauseKind::MATCH && k != ClauseKind::OPTIONAL_MATCH) continue;
            const int32_t lv = clev[ci];
            if (lv < 0) continue;
            if ((size_t)lv >= loc.level_opt.size()) return "extra_level";
            // A level may be OPTIONAL for one member and mandatory for another.  The level walk
            // keeps the per-member optional bits (L.opt), so padding only applies to the members
            // that declared the subpattern OPTIONAL; the mandatory member is dropped when its
            // extension is missing.  The flag is therefore not a placement constraint.
        }
    }
    auto ensure_pos = [&](int32_t v) -> int32_t {
        if (pl.vmap[(size_t)v] >= 0) return pl.vmap[(size_t)v];
        if (!seed) return -1;
        const int32_t s = (int32_t)loc.pos.size();
        SkeletonPos sp;
        sp.name = q.vars[(size_t)v].name;
        sp.labels = q.vars[(size_t)v].labels;
        loc.pos.push_back(sp);
        pl.vmap[(size_t)v] = s;
        return s;
    };
    for (size_t ci = 0; ci < q.clauses.size(); ++ci) {
        if (prefix_end >= 0 && (int32_t)ci > prefix_end) break;
        const Clause& cl = q.clauses[ci];
        const ClauseKind k = cl.kind;
        if (k == ClauseKind::MATCH || k == ClauseKind::OPTIONAL_MATCH) {
            // The edges of a clause are the pattern edges whose `clause` is this one.  A
            // WHERE-pattern edge is written by the builder with its clause but not appended to
            // the clause's own edge list, so reading `cl.edges` alone leaves it unplaced.
            std::vector<int32_t> cedges;
            for (size_t ei = 0; ei < q.edges.size(); ++ei)
                if (q.edges[ei].clause == (int32_t)ci &&
                    (prefix_end < 0 || q.edges[ei].clause <= prefix_end))
                    cedges.push_back((int32_t)ei);
            if (seed) {
                loc.clause_nedges[ci] = (int32_t)cedges.size();
                loc.clause_opt[ci] = k == ClauseKind::OPTIONAL_MATCH ? 1 : 0;
            }
            // every vertex the clause writes gets a position, even when it has no edge yet
            for (int32_t v : cl.vars) {
                const int32_t p = ensure_pos(v);
                if (p < 0) return "vertex_shape";
            }
            for (int32_t e : cedges) {
                const PatternEdge& pe = q.edges[(size_t)e];
                const int32_t u = ensure_pos(pe.from);
                const int32_t w = ensure_pos(pe.to);
                if (u < 0 || w < 0) return "vertex_shape";
                if (u == w) return "self_loop";
                int32_t se = loc.find_edge(u, w);
                if (se < 0 && !seed) se = loc.find_edge(w, u);   // the member reverses the edge
                if (se < 0) {
                    if (!seed) {
                        if (std::getenv("V3_DEBUG_ALIGN")) {
                            std::fprintf(stderr, "edge_topology q=%s ci=%zu %s(%d)->%s(%d) sk:",
                                         q.id.c_str(), ci, q.vars[(size_t)pe.from].name.c_str(), u,
                                         q.vars[(size_t)pe.to].name.c_str(), w);
                            for (size_t z = 0; z < loc.edge.size(); ++z)
                                std::fprintf(stderr, " %zu:%d-%d", z, loc.edge[z].u, loc.edge[z].w);
                            std::fprintf(stderr, "\n");
                        }
                        return "edge_topology";
                    }
                    SkeletonEdge x;
                    x.u = u;
                    x.w = w;
                    x.types = norm(pe.types);
                    x.any_dir = pe.any_dir;
                    x.lo = pe.lo;
                    x.hi = pe.hi;
                    x.clause = (int32_t)ci;
                    x.level = clev[ci];
                    loc.edge.push_back(x);
                    se = (int32_t)loc.edge.size() - 1;
                } else {
                    const SkeletonEdge& x = loc.edge[(size_t)se];
                    if (!((x.u == u && x.w == w) || (x.u == w && x.w == u)))
                        return "edge_direction_conflict";
                    if (!seed && x.level != clev[ci]) return "edge_level_conflict";
                }
                // a second edge between the same positions in one clause is a parallel edge
                for (int32_t e2 : cl.edges)
                    if (e2 != e && pl.emap[(size_t)e2] == se) return "parallel_edges";
                pl.emap[(size_t)e] = se;
                if (!pe.var_name.empty() && q.var_index(pe.var_name) >= 0 &&
                    std::find(pl.vnames.begin(), pl.vnames.end(), pe.var_name) == pl.vnames.end())
                    pl.vnames.push_back(pe.var_name);
                for (const auto& pv : {q.vars[(size_t)pe.from].name, q.vars[(size_t)pe.to].name})
                    if (std::find(pl.vnames.begin(), pl.vnames.end(), pv) == pl.vnames.end()) pl.vnames.push_back(pv);
            }
        } else {
            if (seed) {
                loc.clause_nedges[ci] = 0;
                loc.clause_opt[ci] = 0;
            }
        }
    }
    // every candidate vertex that a pattern edge touches must map to a skeleton position
    for (size_t e = 0; e < q.edges.size(); ++e) {
        if (prefix_end >= 0 && q.edges[e].clause > prefix_end) continue;
        if (pl.emap[e] < 0) return "edge_unplaced";
    }
    // the member's value order: relationship variables first, then the vertices in clause order
    // (including a vertex of a MATCH that writes no edge), then any remaining edge endpoints
    std::vector<std::string> vn;
    auto addn = [&](const std::string& n) {
        if (std::find(vn.begin(), vn.end(), n) == vn.end()) vn.push_back(n);
    };
    for (const auto& e : q.edges)
        if (!e.var_name.empty() && q.var_index(e.var_name) >= 0) addn(e.var_name);
    for (const auto& cl : q.clauses) {
        if (cl.kind != ClauseKind::MATCH && cl.kind != ClauseKind::OPTIONAL_MATCH) continue;
        for (int32_t v : cl.vars) addn(q.vars[(size_t)v].name);
    }
    for (const auto& e : q.edges)
        for (int32_t v : {e.from, e.to}) addn(q.vars[(size_t)v].name);
    pl.vnames = std::move(vn);
    pl.depart_clause = -1;
    *sk = loc;
    *out = pl;
    return "";
}

// A masked evaluation reproduces the candidate's pattern and its trailing WITH / RETURN pipeline.
// A WITH between two MATCH clauses is a stage of its own (its aggregation / LIMIT changes the rows
// the following MATCH joins); those candidates stay with the fallback until that stage is masked.
std::string implementable(const Query& q) {
    if (q.first_match < 0 || q.return_clause < 0) return "no_match";
    if (q.edges.empty()) return "no_edges";   // a single-node pattern: M6 alone
    // Undirected variable-length edges are expanded in both directions by the masked walk
    // (v3_match.cpp vl_step), with the same per-MATCH trail as the reference (v3_vl.cpp).
    // Post atoms (filters before WITH / RETURN) are evaluated by the masked walk when both
    // sides are bound; the walker already defers an atom until it is evaluable, so they do not
    // force M6.  A WITH stage between two MATCH clauses still does.
    bool opaque_with = false;
    for (size_t c = 0; c < q.clauses.size(); ++c) {
        if ((int32_t)c == q.order_by || (int32_t)c == q.skip || (int32_t)c == q.limit) continue;
        const ClauseKind k = q.clauses[c].kind;
        if (k == ClauseKind::WITH) {
            const Clause& w = q.clauses[c];
            bool transparent = !w.distinct && w.order_items.empty() && w.limit_n < 0 && w.skip_n == 0 &&
                               w.where.empty();
            for (int32_t it : w.items) {
                const ProjectionItem& pi = q.items[(size_t)it];
                if (pi.kind != ProjectionItem::KEY || !pi.prop.empty()) transparent = false;
            }
            if (!transparent) opaque_with = true;
            continue;
        }
        if (k == ClauseKind::MATCH || k == ClauseKind::OPTIONAL_MATCH) {
            if (opaque_with) return "with_stage";
        }
    }
    return "";
}

// A WHERE-pattern OR is represented as several top-level pattern edges whose clause has no
// edge-list entry.  The mask sees those edges as a conjunction (company-round-00033: one row
// instead of the 527,683/547,969 reference rows), while the per-query/base path keeps the
// disjunction.  Such candidates are not representable by a single envelope; keep them on M6.
static bool has_word_or(const std::string& s) {
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if ((s[i] != 'O' && s[i] != 'o') || (s[i + 1] != 'R' && s[i + 1] != 'r')) continue;
        const bool left_ok = i == 0 || !std::isalnum((unsigned char)s[i - 1]);
        const bool right_ok = i + 2 == s.size() || !std::isalnum((unsigned char)s[i + 2]);
        if (left_ok && right_ok) return true;
    }
    return false;
}

static bool has_unmaskable_or_pattern(const Query& q) {
    if (!has_word_or(q.text)) return false;
    for (size_t e = 0; e < q.edges.size(); ++e) {
        const int32_t c = q.edges[e].clause;
        if (c < 0 || (size_t)c >= q.clauses.size()) return true;
        const auto& written = q.clauses[(size_t)c].edges;
        if (std::find(written.begin(), written.end(), (int32_t)e) == written.end()) return true;
    }
    return false;
}

// Remove one member from an already-built envelope.  The skeleton may retain positions that
// only this member used; they are harmless (the mask demultiplexes by `present`), while every
// remaining member keeps its exact plan.  This is why the OR gate changes only the affected
// candidate instead of invalidating the whole envelope.
static bool drop_member_from_env(EnvelopeSpec* E, int32_t ci) {
    auto it = std::find(E->members.begin(), E->members.end(), ci);
    if (it == E->members.end()) return false;
    const size_t m = (size_t)(it - E->members.begin());
    auto drop = [&](auto& v) {
        if (m < v.size()) v.erase(v.begin() + (std::ptrdiff_t)m);
    };
    for (auto& L : E->levels) {
        drop(L.present); drop(L.path_dir); drop(L.labels); drop(L.types); drop(L.dir);
        drop(L.any_dir); drop(L.hop_lo); drop(L.hop_hi); drop(L.opt); drop(L.opt_index);
    }
    drop(E->mclause_level); drop(E->vmap); drop(E->emap); drop(E->vlev); drop(E->elev);
    drop(E->depart_clause); drop(E->q); drop(E->members);
    return true;
}

static void fallback_unmaskable_or(const Round& r, RoundPlan* plan) {
    const size_t n = r.queries.size();
    std::vector<char> bad(n, 0), was(n, 0);
    bool any_bad = false;
    for (size_t i = 0; i < n; ++i)
        if (has_unmaskable_or_pattern(r.queries[i])) bad[i] = 1, any_bad = true;
    if (!any_bad) return;
    auto clear_envs = [&](std::vector<EnvelopeSpec>* envs) {
        for (auto it = envs->begin(); it != envs->end();) {
            for (size_t j = 0; j < it->members.size();) {
                const int32_t ci = it->members[j];
                if (ci >= 0 && (size_t)ci < n && bad[(size_t)ci]) {
                    was[(size_t)ci] = 1;
                    drop_member_from_env(&*it, ci);
                } else {
                    ++j;
                }
            }
            if (it->members.empty()) it = envs->erase(it);
            else ++it;
        }
    };
    clear_envs(&plan->envelopes);
    clear_envs(&plan->prefix_envelopes);
    std::fill(plan->env_of.begin(), plan->env_of.end(), -1);
    std::fill(plan->prefix_of.begin(), plan->prefix_of.end(), -1);
    for (size_t e = 0; e < plan->envelopes.size(); ++e)
        for (int32_t ci : plan->envelopes[e].members)
            if (ci >= 0 && (size_t)ci < n) plan->env_of[(size_t)ci] = (int32_t)e;
    for (size_t e = 0; e < plan->prefix_envelopes.size(); ++e)
        for (int32_t ci : plan->prefix_envelopes[e].members)
            if (ci >= 0 && (size_t)ci < n) plan->prefix_of[(size_t)ci] = (int32_t)e;
    for (size_t i = 0; i < n; ++i)
        if (was[i]) {
            plan->why[i] = "or_pattern";
            if (std::find(plan->fallback.begin(), plan->fallback.end(), (int32_t)i) ==
                plan->fallback.end())
                plan->fallback.push_back((int32_t)i);
        }
}

}  // namespace

// ---------------------------------------------------------------- M1 Grouping / Edit Alignment
namespace {

constexpr int kWlambda = 1000;
constexpr double kTheta = 0.7;
constexpr double kLabelOverlap = 0.6;
constexpr int kGroupCap = 64;

std::vector<int32_t> vertex_order(const Query& q) { return pattern_vertex_order(q); }

int32_t degree_of(const Query& q, int32_t v) {
    int32_t d = 0;
    for (const auto& e : q.edges)
        if (e.from == v || e.to == v) ++d;
    return d;
}

bool has_edge(const Query& q, int32_t a, int32_t b) {
    for (const auto& e : q.edges)
        if ((e.from == a && e.to == b) || (e.from == b && e.to == a)) return true;
    return false;
}

std::vector<std::string> label_list(const Query& q, const std::vector<int32_t>& order) {
    std::vector<std::string> out;
    for (int32_t v : order)
        for (const auto& l : q.vars[(size_t)v].labels) out.push_back(l);
    std::sort(out.begin(), out.end());
    return out;
}

bool label_test(const Query& a, const Query& b) {
    // One vertex of a pattern carries a label set (Message = {Post, Comment}); two vertices
    // overlap when their sets intersect, and a greedy one-to-one matching counts the overlap.
    const std::vector<int32_t> oa = vertex_order(a), ob = vertex_order(b);
    std::vector<char> used(ob.size(), 0);
    size_t inter = 0;
    for (int32_t va : oa) {
        for (size_t j = 0; j < ob.size(); ++j) {
            if (used[j]) continue;
            bool hit = false;
            for (const auto& la : a.vars[(size_t)va].labels)
                for (const auto& lb : b.vars[(size_t)ob[j]].labels)
                    if (la == lb) hit = true;
            if (!hit) continue;
            used[j] = 1;
            ++inter;
            break;
        }
    }
    const size_t m = std::max(oa.size(), ob.size());
    return m == 0 || (double)inter >= kLabelOverlap * (double)m;
}

struct AlignMap {
    std::vector<int32_t> q_of_pos;   // seed position -> query vertex
    int paired = 0;
};

// Greedy edit alignment: seed vertices in decreasing degree; each takes the unpaired candidate
// vertex with the largest score (label equality plus the neighbourhood agreement over the pairs
// already fixed).  An inserted / deleted edge lowers the score by one but does not separate its
// endpoints; a relabelled vertex is still paired when its neighbourhood agrees.
AlignMap edit_align(const Query& seed, const Query& q) {
    const std::vector<int32_t> sv = vertex_order(seed), qv = vertex_order(q);
    AlignMap m;
    m.q_of_pos.assign(sv.size(), -1);
    std::vector<int32_t> order(sv.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int32_t)i;
    std::stable_sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
        return degree_of(seed, sv[(size_t)a]) > degree_of(seed, sv[(size_t)b]);
    });
    std::vector<char> used(qv.size(), 0);
    for (int32_t sp : order) {
        const int32_t u = sv[(size_t)sp];
        int best_score = INT32_MIN, best_q = -1;
        for (size_t qi = 0; qi < qv.size(); ++qi) {
            if (used[qi]) continue;
            const int32_t v = qv[qi];
            const bool same_label = norm(seed.vars[(size_t)u].labels) == norm(q.vars[(size_t)v].labels);
            int score = same_label ? kWlambda : 0;
            for (size_t p2 = 0; p2 < sv.size(); ++p2) {
                const int32_t q2 = m.q_of_pos[p2];
                if (q2 < 0) continue;
                const bool es = has_edge(seed, u, sv[p2]);
                const bool eq = has_edge(q, v, q2);
                score += (es == eq) ? 1 : -1;
            }
            if (score > best_score) { best_score = score; best_q = (int32_t)qi; }
        }
        if (best_q >= 0 && best_score > 0) {
            m.q_of_pos[(size_t)sp] = qv[(size_t)best_q];
            used[(size_t)best_q] = 1;
            ++m.paired;
        }
    }
    return m;
}

// Exact-label variant of edit_align for the topological core.  A shared class must carry the
// same label set; a private vertex with a different label is left unpaired and becomes a
// private suffix position instead of stealing a core position from a label-equal vertex.
AlignMap topo_align(const Query& seed, const Query& q) {
    const std::vector<int32_t> sv = vertex_order(seed), qv = vertex_order(q);
    AlignMap m;
    m.q_of_pos.assign(sv.size(), -1);
    std::vector<int32_t> order(sv.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int32_t)i;
    std::stable_sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
        return degree_of(seed, sv[(size_t)a]) > degree_of(seed, sv[(size_t)b]);
    });
    std::vector<char> used(qv.size(), 0);
    for (int32_t sp : order) {
        const int32_t u = sv[(size_t)sp];
        int best_score = INT32_MIN, best_q = -1;
        for (size_t qi = 0; qi < qv.size(); ++qi) {
            if (used[qi]) continue;
            const int32_t v = qv[qi];
            if (norm(seed.vars[(size_t)u].labels) != norm(q.vars[(size_t)v].labels)) continue;
            int score = kWlambda;
            for (size_t p2 = 0; p2 < sv.size(); ++p2) {
                const int32_t q2 = m.q_of_pos[p2];
                if (q2 < 0) continue;
                const bool es = has_edge(seed, u, sv[p2]);
                const bool eq = has_edge(q, v, q2);
                score += (es == eq) ? 1 : -1;
            }
            if (score > best_score) { best_score = score; best_q = (int32_t)qi; }
        }
        if (best_q >= 0) {
            m.q_of_pos[(size_t)sp] = qv[(size_t)best_q];
            used[(size_t)best_q] = 1;
            ++m.paired;
        }
    }
    return m;
}

}  // namespace

namespace {

// Cardinality of a label set (the scan a class performs when it has no anchor).
double label_card(const GraphStore& g, const std::vector<std::string>& labels) {
    if (labels.empty()) return (double)g.num_vertices();
    double n = 0.0;
    for (const auto& l : labels) {
        const int32_t id = g.schema().label_of(l);
        if (id >= 0) n += (double)g.vertices_of((uint32_t)id).size();
    }
    return std::max(1.0, n);
}

// How many roots an anchored class contributes.  An equality on `id` (or any constant property
// map) selects one; the scan still visits the class's label list, which label_card charges.
double anchor_roots(const Query& q, int32_t v) {
    return var_const_anchored(q, v) ? 1.0 : -1.0;   // else the caller uses the label cardinality
}

// Average typed degree per direction, from the loaded graph (one directed copy is stored per
// direction).  The estimate is data-independent only in the type distribution; the pool's
// relationship types are the LDBC ones.
double avg_degree(const GraphStore& g, size_t ntypes) {
    const double nv = std::max(1.0, (double)g.num_vertices());
    const double nt = std::max(1.0, (double)ntypes);
    return std::max(1e-3, (double)g.num_rels() / 2.0 / nt / nv);
}

// Fanout of one pattern edge for the child class: average degree of the parent along the edge
// type set and direction, times the child label selectivity, or one for an anchored child.
double edge_fanout(const GraphStore& g, const Query& q, const PatternEdge& e, int32_t child,
                   size_t ntypes) {
    double f = avg_degree(g, ntypes);
    if (e.any_dir) f *= 2.0;
    const double roots = anchor_roots(q, child);
    if (roots > 0.0) return 1.0;
    const double sel = label_card(g, q.vars[(size_t)child].labels) /
                       std::max(1.0, (double)g.num_vertices());
    f *= std::max(1e-6, sel);
    if (e.lo != 1 || e.hi != 1) {
        const int32_t u = e.hi < 0 ? 4 : std::max(1, e.hi);
        double sum = 0.0, pow = 1.0;
        for (int32_t d = 1; d <= u; ++d) {
            pow *= std::max(1.0, f);
            sum += pow;
        }
        return std::max(1.0, sum);
    }
    return std::max(1.0, f);
}

// Expansion cost after the first (scanned) class: sum over the prefixes of the product of the
// fanouts, formula (1) of the paper with the scan separated out.
double expansion_cost_order(const GraphStore& g, const Query& q,
                            const std::vector<int32_t>& order, size_t ntypes) {
    if (order.empty()) return 0.0;
    double frontier = 1.0, total = 0.0;
    for (size_t j = 1; j < order.size(); ++j) {
        const int32_t v = order[j];
        const PatternEdge* link = nullptr;
        for (const auto& e : q.edges)
            if ((e.from == v || e.to == v)) {
                const int32_t other = e.from == v ? e.to : e.from;
                if (std::find(order.begin(), order.begin() + (long)j, other) !=
                    order.begin() + (long)j) {
                    link = &e;
                    break;
                }
            }
        const double f = link ? edge_fanout(g, q, *link, v, ntypes) : 1.0;
        frontier *= f;
        total += frontier;
        if (frontier > 1e15) { total = 1e15; break; }
    }
    return total;
}

double expansion_cost(const GraphStore& g, const Query& q, size_t ntypes) {
    return expansion_cost_order(g, q, pattern_vertex_order(q), ntypes);
}

}  // namespace

double estimate_query(const GraphStore& g, const Query& q) {
    const std::vector<int32_t> order = pattern_vertex_order(q);
    if (order.empty()) return 1.0;
    const int32_t first = order[0];
    double scan = label_card(g, q.vars[(size_t)first].labels);
    // An anchored first class is scanned once but expands from one root; an unanchored one
    // expands from every vertex the scan accepts.  The expansion term is the same product.
    double exp = expansion_cost(g, q, g.schema().types.size());
    const double roots = anchor_roots(q, first);
    if (roots > 0.0) exp = std::min(exp, exp);   // roots already represented by the product
    double w = scan + exp;
    if (q.clauses.size() > 2) w *= 1.1;
    return std::max(1.0, w);
}

double estimate_query_serial_order(const GraphStore& g, const Query& q) {
    std::vector<int32_t> order = pattern_vertex_order(q);
    if (order.empty()) return 1.0;
    // The per-query executor starts from a constant anchor when the pattern has one; the text
    // order is irrelevant.  Charge that root as one candidate, not as the label class.
    int32_t first = order[0];
    for (int32_t v : order)
        if (var_const_anchored(q, v)) { first = v; break; }
    std::vector<int32_t> rooted;
    rooted.push_back(first);
    for (int32_t v : order)
        if (v != first) rooted.push_back(v);
    const double scan = var_const_anchored(q, first)
                            ? 1.0 : label_card(g, q.vars[(size_t)first].labels);
    double w = scan + expansion_cost_order(g, q, rooted, g.schema().types.size());
    if (q.clauses.size() > 2) w *= 1.1;
    return std::max(1.0, w);
}

// The tiny rule is only trusted on a single fixed hop / single vertex: there the anchored
// estimate is the executor's own order.  A multi-hop / variable-length / multi-clause candidate
// keeps the conservative M2 estimate, otherwise a small anchored scan would hide an expensive
// pattern and the whole workload would bypass planning.
static bool tiny_shape(const Query& q) {
    if (q.clauses.size() > 2) return false;
    if (q.edges.size() > 1) return false;
    if (!q.edges.empty() && (q.edges[0].lo != 1 || q.edges[0].hi != 1)) return false;
    return true;
}

static double tiny_estimate(const GraphStore& g, const Query& q) {
    return tiny_shape(q) ? estimate_query_serial_order(g, q) : estimate_query(g, q);
}

void prepare_tiny_round(const GraphStore& g, Round* r) {
    r->tiny = false;
    r->tiny_uniq.clear();
    r->tiny_replay.clear();
    double raw = 0.0;
    for (const auto& q : r->queries) raw += tiny_estimate(g, q);
    r->tiny_est = raw;
    const double thresh = std::getenv("V3_TINY_PLAN_SEC")
                              ? std::atof(std::getenv("V3_TINY_PLAN_SEC")) : kTinyPlanSec;
    if (kWorkGamma * raw >= thresh) return;
    // Canonical replay is still required on the direct path.  The keys are computed here, before
    // the timed cell, exactly as alignment would compute them.
    std::map<std::string, int32_t> first_of;
    std::vector<int32_t> uniq;
    std::vector<int32_t> replay(r->queries.size(), -1);
    double est = 0.0;
    for (size_t i = 0; i < r->queries.size(); ++i) {
        const std::string key = canonical_key(r->queries[i]);
        auto it = first_of.find(key);
        if (it == first_of.end()) {
            first_of[key] = (int32_t)i;
            uniq.push_back((int32_t)i);
            est += tiny_estimate(g, r->queries[i]);
        } else {
            replay[i] = it->second;
        }
    }
    r->tiny_est = est;
    if (kWorkGamma * est < thresh) {
        r->tiny = true;
        r->tiny_uniq = std::move(uniq);
        r->tiny_replay = std::move(replay);
    }
}

// Fill one envelope (positions, edges, levels) from a skeleton and the candidates placed on it.
static void build_env(const Round& r, const Skeleton& sk, std::vector<Placed> keep,
                      RoundPlan* plan, bool single, int32_t prefix_end = -1);

// Estimated MCS build work of one envelope.  The build does not scan every anchored class: it
// takes one root per connected component of the skeleton (the most selective anchor, else the
// smallest class that fits the build cap), materialises the edge layers reachable from it, and
// refines.  The root scan replaces the walk's own scan, so it is charged only when the MCS root
// differs from the class the masked walk would scan first; charging every anchored label domain
// made the gate refuse every envelope on SF10 (the old estimate was 10-1,000x the real build).
double estimate_mcs_build(const GraphStore& g, const EnvelopeSpec& E) {
    const size_t NP = E.pos.size();
    if (NP == 0) return 0.0;
    const double cap = 100000.0;
    std::vector<char> anch(NP, 0);
    for (size_t m = 0; m < E.members.size() && m < E.q.size(); ++m) {
        const Query& q = *E.q[m];
        for (size_t v = 0; v < q.vars.size(); ++v) {
            if (!var_const_anchored(q, (int32_t)v)) continue;
            const int32_t p = member_var_pos(E, m, q, (int32_t)v);
            if (p >= 0 && (size_t)p < NP) anch[(size_t)p] = 1;
        }
    }
    std::vector<double> card(NP, 0.0);
    for (size_t p = 0; p < NP; ++p) card[p] = label_card(g, E.pos[p].labels);

    // Connected components of the skeleton (positions joined by a skeleton edge).
    std::vector<int32_t> parent(NP);
    for (size_t p = 0; p < NP; ++p) parent[p] = (int32_t)p;
    std::function<int32_t(int32_t)> find = [&](int32_t x) {
        while (parent[(size_t)x] != x) {
            parent[(size_t)x] = parent[(size_t)parent[(size_t)x]];
            x = parent[(size_t)x];
        }
        return x;
    };
    auto join = [&](int32_t a, int32_t b) {
        a = find(a); b = find(b);
        if (a != b) parent[(size_t)a] = b;
    };
    for (const auto& e : E.edges)
        if (e.u >= 0 && e.w >= 0 && (size_t)e.u < NP && (size_t)e.w < NP) join(e.u, e.w);

    // One root per component: an anchor costs one candidate, an unanchored root costs its scan.
    std::vector<int32_t> root(NP, -1);
    std::vector<double> root_cost(NP, 1e300);
    for (size_t p = 0; p < NP; ++p) {
        const int32_t c = find((int32_t)p);
        const double sc = anch[p] ? 1.0 : (card[p] <= cap ? card[p] : 1e300);
        if (sc < root_cost[(size_t)c]) {
            root_cost[(size_t)c] = sc;
            root[(size_t)c] = (int32_t)p;
        }
    }
    std::vector<char> built(NP, 0);
    double cost = 0.0;
    for (size_t p = 0; p < NP; ++p) {
        if (parent[p] != (int32_t)p) continue;
        const int32_t r = root[p];
        if (r < 0) continue;
        built[(size_t)r] = 1;
        // If the root is an anchor its scan is the walk's root scan; charge only the one
        // candidate it materialises.  An unanchored root pays its own class scan.
        cost += anch[(size_t)r] ? 1.0 : std::min(card[(size_t)r], cap);
    }
    const double fan = std::max(1e-3, (double)g.num_rels() / 2.0 /
                                        std::max(1.0, (double)g.schema().types.size()) /
                                        std::max(1.0, (double)g.num_vertices()));
    // Edge layers are built only when their parent class is materialised.  A parent that is
    // anchored has one root candidate; an unanchored parent costs its candidate count.
    for (const auto& e : E.edges) {
        if (e.u < 0 || e.w < 0 || (size_t)e.u >= NP || (size_t)e.w >= NP) continue;
        if (!built[(size_t)e.u] && !built[(size_t)e.w]) continue;
        const size_t up = built[(size_t)e.u] ? (size_t)e.u : (size_t)e.w;
        const double cu = anch[up] ? 1.0 : std::min(card[up], cap);
        const double deg = fan * std::max<size_t>(1, e.types.size()) * (e.any_dir ? 2.0 : 1.0);
        cost += cu * deg;                                   // adjacency scanned per parent
        const size_t other = up == (size_t)e.u ? (size_t)e.w : (size_t)e.u;
        if (!anch[other]) cost += std::min(cu * deg, card[other]) * 0.5;   // discovered children
        cost += cu * 0.25;                                   // refinement probe per parent
    }
    return std::max(1.0, std::min(cost, 1e15));
}

// Two skeletons share an edge (same position pair, same types and direction).
// Adjacency scans of the core's close edges (both endpoints already bound): one scan of the
// bound endpoint's degree per core row.  The average-fanout estimate cannot see a hub, so this
// is a separate gate: a core whose close-edge scans alone exceed the group's own work cannot be
// repaid by sharing.  Cached per label set because the planner calls it for several groups.
double topology_core_close(const GraphStore& g, const Skeleton& sk) {
    const size_t NP = sk.pos.size();
    std::map<std::string, double> maxdeg_cache;
    auto max_degree = [&](const SkeletonPos& sp) -> double {
        std::string key;
        for (const auto& lab : sp.labels) key += lab + ",";
        auto it = maxdeg_cache.find(key);
        if (it != maxdeg_cache.end()) return it->second;
        double best = 1.0;
        const AdjView av = g.all();
        if (sp.labels.empty()) {
            for (uint32_t v = 0; v < g.num_vertices(); ++v)
                best = std::max(best, (double)((*av.off)[v + 1] - (*av.off)[v]));
        } else {
            for (const auto& lab : sp.labels) {
                const int32_t id = g.schema().label_of(lab);
                if (id < 0) continue;
                for (uint32_t v : g.vertices_of((uint32_t)id))
                    best = std::max(best, (double)((*av.off)[v + 1] - (*av.off)[v]));
            }
        }
        maxdeg_cache[key] = best;
        return best;
    };
    double close = 0.0;
    for (const auto& e : sk.edge) {
        if (e.exec_level != 0 || e.tree) continue;
        if (e.u < 0 || (size_t)e.u >= NP) continue;
        const double deg = max_degree(sk.pos[(size_t)e.u]) *
                           (double)std::max<size_t>(1, e.types.size()) * (e.any_dir ? 2.0 : 1.0);
        close += deg;
    }
    return close;
}

// Estimated work of a topological envelope: one core walk (scan + core expansion) plus one
// extension per distinct private step (the suffix trie is charged once per branch, not once per
// member).  The core root is the most selective core position; a private step costs the rows
// reaching its parent times its fanout.  A member that lacks a step pays nothing for it.
double estimate_topology(const GraphStore& g, const Round& r, const Skeleton& sk) {
    const size_t NP = sk.pos.size();
    if (NP == 0 || sk.members.size() < 2) return 0.0;
    int32_t root = -1;
    double best = 1e300;
    for (size_t p = 0; p < NP; ++p) {
        if (sk.pos[p].exec_level != 0) continue;
        const double c = sk.anchored[p] ? 1.0 : label_card(g, sk.pos[p].labels);
        if (root < 0 || c < best) { root = (int32_t)p; best = c; }
    }
    if (root < 0) return 1e18;
    auto fanout = [&](const SkeletonEdge& e, int32_t child) -> double {
        if (sk.anchored[(size_t)child]) return 1.0;
        double f = avg_degree(g, g.schema().types.size());
        if (e.any_dir) f *= 2.0;
        f *= (double)std::max<size_t>(1, e.types.size());
        const double sel = label_card(g, sk.pos[(size_t)child].labels) /
                           std::max(1.0, (double)g.num_vertices());
        f *= std::max(1e-6, sel);
        if (e.lo != 1 || e.hi != 1) {
            const int32_t u = e.hi < 0 ? 4 : std::max(1, e.hi);
            double sum = 0.0, pow = 1.0;
            for (int32_t d = 1; d <= u; ++d) {
                pow *= std::max(1.0, f);
                sum += pow;
            }
            return std::max(1.0, sum);
        }
        return std::max(1.0, f);
    };
    std::vector<double> prod(NP, 0.0);
    std::vector<char> seen(NP, 0);
    prod[(size_t)root] = 1.0;
    seen[(size_t)root] = 1;
    std::vector<int32_t> stack{root};
    double core_exp = 0.0;
    while (!stack.empty()) {
        const int32_t u = stack.back();
        stack.pop_back();
        for (const auto& e : sk.edge) {
            if (e.exec_level != 0 || e.u < 0 || e.w < 0) continue;
            int32_t v = -1;
            if (e.u == u) v = e.w;
            else if (e.w == u) v = e.u;
            if (v < 0 || (size_t)v >= NP || seen[(size_t)v]) continue;
            seen[(size_t)v] = 1;
            prod[(size_t)v] = prod[(size_t)u] * fanout(e, v);
            core_exp += prod[(size_t)v];
            stack.push_back(v);
        }
    }
    const double core_close = topology_core_close(g, sk);
    double private_sum = 0.0;
    for (size_t p = 0; p < NP; ++p) {
        if (sk.pos[p].exec_level <= 0) continue;
        const int32_t par = p < sk.birth_parent.size() ? sk.birth_parent[p] : -1;
        if (par >= 0 && (size_t)par < NP) {
            const int32_t be = p < sk.birth_edge.size() ? sk.birth_edge[p] : -1;
            double f = 1.0;
            if (be >= 0 && (size_t)be < sk.edge.size()) f = fanout(sk.edge[(size_t)be], (int32_t)p);
            prod[p] = std::max(1e-9, prod[(size_t)par]) * f;
            private_sum += prod[p];
        } else {
            // A private component root is scanned on its own.
            prod[p] = 1.0;
            private_sum += sk.anchored[p] ? 1.0 : label_card(g, sk.pos[p].labels);
        }
    }
    // A private close edge (both ends already bound) is a mask test on the parent rows.
    for (const auto& e : sk.edge) {
        if (e.exec_level <= 0 || e.tree) continue;
        const double rows = (e.u >= 0 && (size_t)e.u < NP) ? std::max(1.0, prod[(size_t)e.u]) : 1.0;
        private_sum += 0.05 * rows;
    }
    const double scan = sk.anchored[(size_t)root] ? 1.0 : label_card(g, sk.pos[(size_t)root].labels);
    const double mask = 1.0 + 0.02 * std::ceil((double)sk.members.size() / 64.0);
    if (std::getenv("V3_DEBUG_TOPO_EST"))
        std::fprintf(stderr, "[topoest] members=%zu scan=%.0f core_exp=%.0f core_close=%.0f private=%.0f est=%.0f\n",
                     sk.members.size(), scan, core_exp, core_close, private_sum,
                     mask * (scan + core_exp + core_close + private_sum));
    return std::max(1.0, mask * (scan + core_exp + private_sum));
}


// The estimated work of a masked evaluation of the envelope `sk` over its members: one scan of
// the first position's label union and, per level-0 edge, the union's typed adjacency walked per
// enumerated relationship times the mask test (the same measure as estimate_query, on the
// envelope's walk).
// `same_tree` is set by the small-batch planner, which has already grouped the members by
// their common join tree; the estimate then skips the O(K) shape comparison and charges the
// no-C_S expansion only.
double estimate_envelope(const GraphStore& g, const Round& r, const Skeleton& sk,
                        bool same_tree = false) {
    if (sk.members.empty()) return 0.0;
    if (sk.members.size() == 1) return estimate_query(g, r.queries[(size_t)sk.members[0]]);
    if (sk.topo) return estimate_topology(g, r, sk);
    // Shared scan of the first position: the union of the members' label sets, charged once.
    double scan = label_card(g, sk.pos.empty() ? std::vector<std::string>{} : sk.pos[0].labels);
    // Expansions are shared only when the members have the same edge shape (endpoints, types,
    // direction), so one masked walk visits the same branches for all of them.  Hop ranges may
    // differ (that is the depth mask) and are charged at the largest upper bound.  A member with
    // a different edge shape is a private suffix and is charged in full: merging it would make
    // the envelope walk both branches, which is what the estimate must refuse.
    auto shape = [](const Query& q) {
        std::vector<std::string> s;
        const std::vector<int32_t> order = required_vertex_order(q);
        auto at = [&](int32_t v) {
            auto it = std::find(order.begin(), order.end(), v);
            return it == order.end() ? -1 : (int)(it - order.begin());
        };
        for (const auto& e : q.edges) {
            if (e.clause < 0 || (size_t)e.clause >= q.clauses.size() ||
                q.clauses[(size_t)e.clause].kind != ClauseKind::MATCH)
                continue;
            // Types and direction are per-member tests of the edge layer, not part of the
            // topology.  A reversed edge writes the same undirected class pair, so order the
            // endpoints instead of rejecting the member for writing the pattern the other way.
            int a = at(e.from), b = at(e.to);
            if (a < 0 || b < 0) { s.push_back("?"); continue; }
            if (a > b) std::swap(a, b);
            s.push_back(std::to_string(a) + "-" + std::to_string(b));
        }
        std::sort(s.begin(), s.end());
        return s;
    };
    // Different constant anchors at the first position mean there is no common scan: each
    // member would have its own one-vertex root, while the envelope scans the label union and
    // tests every anchor against every row.  Keep such candidates separate (P3).
    {
        std::set<std::string> anchors;
        bool all_anchored = true;
        for (int32_t ci : sk.members) {
            const Query& q = r.queries[(size_t)ci];
            const std::vector<int32_t> order = pattern_vertex_order(q);
            if (order.empty()) { all_anchored = false; break; }
            const PatternVar& pv = q.vars[(size_t)order[0]];
            std::string a;
            for (const auto& pr : pv.props)
                if (pr.first == "id" || pr.first == "name") a = pr.first + "=" + val_str(pr.second);
            for (int32_t ai : const_atoms_of(q, order[0], q.first_match)) {
                if (!a.empty()) break;
                const Atom& at = q.atoms[(size_t)ai];
                if (at.prop == "id" || at.prop == "name")
                    a = at.prop + "=" + val_str(at.cmp.consts[0]);
            }
            if (a.empty()) { all_anchored = false; break; }
            anchors.insert(a);
        }
        if (all_anchored && anchors.size() > 1) return 1e18;
    }
    const std::vector<std::string> base =
        same_tree ? std::vector<std::string>() : shape(r.queries[(size_t)sk.members[0]]);
    bool same = true;
    double exp_sum = 0.0, exp_max = 0.0;
    for (int32_t ci : sk.members) {
        const Query& q = r.queries[(size_t)ci];
        const double e = expansion_cost(g, q, g.schema().types.size());
        exp_sum += e;
        exp_max = std::max(exp_max, e);
        if (!same_tree && shape(q) != base) same = false;
    }
    // Different edge shapes make the envelope walk both join trees; charging the members'
    // expansions in full is not enough to capture the cross product, so such a set is not
    // merged (P3: it is evaluated per candidate).  Same-shape members share the walk.
    if (!same) return 1e18;
    const double exp = exp_max;
    const double mask = 1.0 + 0.02 * std::ceil((double)sk.members.size() / 64.0);
    return std::max(1.0, mask * (scan + exp));
}


// ---------------------------------------------------------------- seventh edit class
// Topological edits are not a relaxation of a vertex or an edge: a member may add or delete
// vertices and edges inside the group.  The group is laid out on its largest common connected
// subpattern (the core); every remaining member edge becomes a private suffix step.  A step is
// one skeleton edge with the member mask that owns it: equal private edges share the step, and
// the mask splits where the members diverge.  The masked walk passes a member over a step it
// does not own (the step's new positions become NULL for it) and lets it walk only its own
// steps, so its bit reaches the sink exactly after its last private edge.
namespace {

int32_t find_edge_either(const std::vector<SkeletonEdge>& es, int32_t u, int32_t w);

// One MATCH clause only: the relationship ledger and the clause-level scope of v3_match are per
// skeleton edge, and the topological layout keeps every step in that one clause.  A query with
// a later MATCH / OPTIONAL MATCH stays on the existing full/prefix paths.
bool topology_eligible(const Query& q) {
    if (q.first_match < 0 || (size_t)q.first_match >= q.clauses.size()) return false;
    if (q.clauses[(size_t)q.first_match].kind != ClauseKind::MATCH) return false;
    for (size_t c = 0; c < q.clauses.size(); ++c) {
        // ORDER BY / SKIP / LIMIT share the default clause kind; they carry no pattern.
        if ((int32_t)c == q.order_by || (int32_t)c == q.skip || (int32_t)c == q.limit) continue;
        const ClauseKind k = q.clauses[c].kind;
        if ((k == ClauseKind::MATCH || k == ClauseKind::OPTIONAL_MATCH) &&
            (int32_t)c != q.first_match)
            return false;
    }
    return true;
}

bool topo_pair_ok(const Query& seed, const Query& q) {
    if (!label_test(seed, q)) return false;
    const AlignMap am = topo_align(seed, q);
    const int ns = (int)pattern_vertex_order(seed).size();
    const int nq = (int)pattern_vertex_order(q).size();
    return am.paired >= (int)std::ceil(0.6 * (double)std::max(ns, nq));
}

bool label_intersects(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.empty() || b.empty()) return true;
    for (const auto& x : a)
        for (const auto& y : b)
            if (x == y) return true;
    return false;
}

// Lay a group out on its common core plus a private suffix trie.  The seed is the widest
// member; edit_align supplies the vertex correspondence (the same alignment as M1).  Every
// member keeps its own `vmap` / `emap`, so build_env sees exactly the private edges it owns.
std::string build_topo_group(const GraphStore& g, const Round& r,
                             const std::vector<int32_t>& grp, Skeleton* out_sk,
                             std::vector<Placed>* out_keep) {
    if (grp.empty()) return "empty";
    const Query& seed = r.queries[(size_t)grp[0]];
    if (!topology_eligible(seed)) return "clause";
    Skeleton full;
    Placed solo;
    const std::string bad = place_member(seed, &full, &solo);
    if (!bad.empty()) return bad;
    const std::vector<int32_t> sv = pattern_vertex_order(seed);
    const int ns = (int)sv.size();
    const size_t G = grp.size();
    if (G > 64) return "too_many";
    const uint64_t all = (G == 64) ? ~0ULL : ((1ULL << G) - 1ULL);

    // Seed position -> member vertex.  The seed is the identity; every other member is aligned
    // by the M1 edit alignment.
    std::vector<std::vector<int32_t>> qmap(G);
    for (size_t m = 0; m < G; ++m) {
        const Query& q = r.queries[(size_t)grp[m]];
        if (!topology_eligible(q) || q.sem != seed.sem || q.first_match != seed.first_match)
            return "clause";
        qmap[m].assign(q.vars.size(), -1);
        if (m == 0) {
            for (int p = 0; p < ns; ++p) qmap[m][(size_t)sv[(size_t)p]] = p;
            continue;
        }
        if (!topo_pair_ok(seed, q)) return "align";
        const AlignMap am = topo_align(seed, q);
        for (int p = 0; p < ns; ++p)
            if (am.q_of_pos[(size_t)p] >= 0)
                qmap[m][(size_t)am.q_of_pos[(size_t)p]] = p;
    }

    // Core candidate: a seed edge is common when every member writes an edge between the two
    // aligned seed positions (either direction; types, direction and hops are per-member tests).
    std::vector<char> cand(full.edge.size(), 0);
    std::vector<int32_t> uf((size_t)ns);
    for (int p = 0; p < ns; ++p) uf[(size_t)p] = p;
    std::function<int32_t(int32_t)> find = [&](int32_t x) {
        while (uf[(size_t)x] != x) { uf[(size_t)x] = uf[(size_t)uf[(size_t)x]]; x = uf[(size_t)x]; }
        return x;
    };
    for (size_t se = 0; se < full.edge.size(); ++se) {
        const SkeletonEdge& X = full.edge[se];
        bool common = true;
        for (size_t m = 0; m < G && common; ++m) {
            const Query& q = r.queries[(size_t)grp[m]];
            bool hit = false;
            for (const auto& pe : q.edges) {
                if (pe.clause != q.first_match) continue;
                const int32_t a = qmap[m][(size_t)pe.from], b = qmap[m][(size_t)pe.to];
                if ((a == X.u && b == X.w) || (a == X.w && b == X.u)) { hit = true; break; }
            }
            if (!hit) common = false;
        }
        if (!common) continue;
        cand[se] = 1;
        const int32_t a = find(X.u), b = find(X.w);
        if (a != b) uf[(size_t)a] = b;
    }
    // Largest connected component of the common edges.  If there is no common edge, one vertex
    // that every member aligns is still a core (its scan is shared).
    std::map<int32_t, std::vector<int32_t>> comp;
    for (int p = 0; p < ns; ++p) comp[find(p)].push_back(p);
    int best_root = -1;
    size_t best_v = 0, best_e = 0;
    for (const auto& kv : comp) {
        size_t ne = 0;
        for (size_t se = 0; se < full.edge.size(); ++se)
            if (cand[se] && find(full.edge[se].u) == kv.first) ++ne;
        if (ne == 0) continue;
        if (kv.second.size() > best_v || (kv.second.size() == best_v && ne > best_e)) {
            best_v = kv.second.size();
            best_e = ne;
            best_root = kv.first;
        }
    }
    std::vector<int32_t> core_old, core_edge;
    if (best_root >= 0) {
        core_old = comp[best_root];
        for (size_t se = 0; se < full.edge.size(); ++se)
            if (cand[se] && find(full.edge[se].u) == best_root) core_edge.push_back((int32_t)se);
        std::sort(core_old.begin(), core_old.end());
        std::sort(core_edge.begin(), core_edge.end());
    } else {
        int best_p = -1;
        double best_card = 1e300;
        for (int p = 0; p < ns; ++p) {
            bool every = true;
            for (size_t m = 0; m < G && every; ++m) {
                bool hit = false;
                for (int32_t x : qmap[m])
                    if (x == p) { hit = true; break; }
                if (!hit) every = false;
            }
            if (!every) continue;
            const double c = label_card(g, full.pos[(size_t)p].labels);
            if (c < best_card) { best_card = c; best_p = p; }
        }
        if (best_p < 0) return "no_core";
        core_old.push_back(best_p);
    }

    Skeleton sk;
    sk.topo = true;
    sk.seed_q = full.seed_q;
    sk.sem = seed.sem;
    sk.first_match = seed.first_match;
    sk.clause_level = full.clause_level;
    sk.clause_nedges = full.clause_nedges;
    sk.clause_opt = full.clause_opt;
    sk.level_opt = full.level_opt;
    sk.members = grp;
    std::vector<int32_t> old2new((size_t)ns, -1);
    for (size_t i = 0; i < core_old.size(); ++i) old2new[(size_t)core_old[i]] = (int32_t)i;
    sk.pos.reserve(core_old.size() + 8);
    sk.birth_parent.reserve(core_old.size() + 8);
    sk.birth_edge.reserve(core_old.size() + 8);
    sk.users.reserve(core_old.size() + 8);
    sk.anchored.reserve(core_old.size() + 8);
    for (int32_t old : core_old) {
        SkeletonPos sp = full.pos[(size_t)old];
        sp.exec_level = 0;
        sk.pos.push_back(std::move(sp));
        sk.birth_parent.push_back(-1);
        sk.birth_edge.push_back(-1);
        sk.users.push_back(all);
        sk.anchored.push_back(0);
    }
    // A common vertex whose member is constant-anchored has one root candidate.
    for (size_t m = 0; m < G; ++m) {
        const Query& q = r.queries[(size_t)grp[m]];
        for (size_t v = 0; v < q.vars.size(); ++v) {
            if (q.vars[v].kind != PatternVar::VERTEX) continue;
            const int32_t old = qmap[m][v];
            if (old < 0 || old2new[(size_t)old] < 0) continue;
            if (var_const_anchored(q, (int32_t)v)) sk.anchored[(size_t)old2new[(size_t)old]] = 1;
        }
    }
    // Core edges; the walk's tree is a BFS forest over them (the rest are close tests).
    std::vector<char> core_tree(core_edge.size(), 0);
    if (!core_edge.empty()) {
        int root = -1;
        double best = 1e300;
        for (size_t i = 0; i < core_old.size(); ++i) {
            const double c = sk.anchored[i] ? 1.0 : label_card(g, sk.pos[i].labels);
            if (root < 0 || c < best) { root = (int32_t)i; best = c; }
        }
        std::vector<char> seen(sk.pos.size(), 0);
        std::vector<int32_t> q{root};
        seen[(size_t)root] = 1;
        while (!q.empty()) {
            const int32_t u = q.back();
            q.pop_back();
            for (size_t k = 0; k < core_edge.size(); ++k) {
                const SkeletonEdge& X = full.edge[(size_t)core_edge[k]];
                const int32_t a = old2new[(size_t)X.u], b = old2new[(size_t)X.w];
                int32_t v = -1;
                if (a == u) v = b;
                else if (b == u) v = a;
                if (v < 0 || seen[(size_t)v]) continue;
                seen[(size_t)v] = 1;
                core_tree[k] = 1;
                q.push_back(v);
            }
        }
    }
    for (size_t k = 0; k < core_edge.size(); ++k) {
        SkeletonEdge X = full.edge[(size_t)core_edge[k]];
        X.u = old2new[(size_t)X.u];
        X.w = old2new[(size_t)X.w];
        X.clause = seed.first_match;
        X.level = 0;
        X.exec_level = 0;
        X.owners = (uint32_t)all;
        X.core = true;
        X.tree = core_tree[k] != 0;
        sk.edge.push_back(std::move(X));
    }

    int32_t next_level = 0;
    std::vector<std::vector<int32_t>> pos_of(G);
    std::vector<Placed> keep;
    for (size_t m = 0; m < G; ++m) {
        const Query& q = r.queries[(size_t)grp[m]];
        const uint64_t bit = 1ULL << m;
        pos_of[m].assign(q.vars.size(), -1);
        std::vector<char> used(sk.pos.size(), 0);
        for (size_t v = 0; v < q.vars.size(); ++v) {
            const int32_t old = qmap[m][v];
            if (old < 0 || old2new[(size_t)old] < 0) continue;
            const int32_t p = old2new[(size_t)old];
            if (used[(size_t)p]) return "vertex_collision";
            used[(size_t)p] = 1;
            pos_of[m][v] = p;
        }
        auto grow_users = [&](int32_t p) {
            if (p >= (int32_t)sk.users.size()) {
                sk.birth_parent.resize((size_t)p + 1, -1);
                sk.birth_edge.resize((size_t)p + 1, -1);
                sk.users.resize((size_t)p + 1, 0);
                sk.anchored.resize((size_t)p + 1, 0);
                used.resize((size_t)p + 1, 0);
            }
            sk.users[(size_t)p] |= bit;
        };
        auto find_share = [&](int32_t parent, int32_t v) -> int32_t {
            for (size_t p = 0; p < sk.pos.size(); ++p) {
                if (sk.pos[p].exec_level == 0) continue;   // core: every member already uses it
                if ((sk.users[p] & bit) != 0) continue;
                if (sk.birth_parent[p] != parent) continue;
                if (!label_intersects(q.vars[(size_t)v].labels, sk.pos[p].labels)) continue;
                return (int32_t)p;
            }
            return -1;
        };
        auto new_pos = [&](int32_t parent, int32_t v) -> int32_t {
            SkeletonPos sp;
            sp.name = q.vars[(size_t)v].name;
            sp.labels = q.vars[(size_t)v].labels;
            sp.exec_level = -1;
            sk.pos.push_back(std::move(sp));
            sk.birth_parent.push_back(parent);
            sk.birth_edge.push_back(-1);
            sk.users.push_back(0);
            sk.anchored.push_back(0);
            used.push_back(0);
            return (int32_t)sk.pos.size() - 1;
        };
        auto add_edge = [&](int32_t u, int32_t w, const PatternEdge& pe, int32_t introduced) -> int32_t {
            int32_t se = find_edge_either(sk.edge, u, w);
            if (se >= 0) {
                sk.edge[(size_t)se].owners |= (uint32_t)bit;
                sk.edge[(size_t)se].any_dir = sk.edge[(size_t)se].any_dir || pe.any_dir;
                sk.edge[(size_t)se].lo = std::min(sk.edge[(size_t)se].lo, pe.lo);
                sk.edge[(size_t)se].hi = (sk.edge[(size_t)se].hi < 0 || pe.hi < 0)
                                             ? -1 : std::max(sk.edge[(size_t)se].hi, pe.hi);
                for (const auto& t : pe.types) {
                    if (std::find(sk.edge[(size_t)se].types.begin(),
                                  sk.edge[(size_t)se].types.end(), t) == sk.edge[(size_t)se].types.end())
                        sk.edge[(size_t)se].types.push_back(t);
                }
                sk.edge[(size_t)se].types = norm(sk.edge[(size_t)se].types);
                return se;
            }
            SkeletonEdge X;
            X.u = u;
            X.w = w;
            X.types = norm(pe.types);
            X.any_dir = pe.any_dir;
            X.lo = pe.lo;
            X.hi = pe.hi;
            X.clause = pe.clause;
            X.level = 0;
            X.exec_level = ++next_level;
            X.tree = introduced >= 0;
            X.owners = (uint32_t)bit;
            X.core = false;
            sk.edge.push_back(std::move(X));
            se = (int32_t)sk.edge.size() - 1;
            if (introduced >= 0) {
                sk.birth_edge[(size_t)introduced] = se;
                sk.pos[(size_t)introduced].exec_level = sk.edge[(size_t)se].exec_level;
            }
            return se;
        };

        // Vertices written by the clause but not touched by an edge: share a root scan.
        const std::vector<int32_t> vo = pattern_vertex_order(q);
        for (int32_t v : vo) {
            if (pos_of[m][(size_t)v] >= 0) continue;
            bool incident = false;
            for (const auto& pe : q.edges)
                if (pe.from == v || pe.to == v) { incident = true; break; }
            if (incident) continue;
            int32_t p = find_share(-1, v);
            if (p < 0) {
                p = new_pos(-1, v);
                sk.pos[(size_t)p].exec_level = ++next_level;
            }
            grow_users(p);
            pos_of[m][(size_t)v] = p;
            used[(size_t)p] = 1;
            if (var_const_anchored(q, v)) sk.anchored[(size_t)p] = 1;
        }

        std::vector<char> done(q.edges.size(), 0);
        size_t remaining = 0;
        for (size_t e = 0; e < q.edges.size(); ++e)
            if (q.edges[e].clause == q.first_match) ++remaining;
            else return "clause";
        Placed pl;
        pl.cand = grp[m];
        pl.vmap.assign(q.vars.size(), -1);
        pl.emap.assign(q.edges.size(), -1);
        while (remaining > 0) {
            bool progress = false;
            for (size_t e = 0; e < q.edges.size(); ++e) {
                if (done[e]) continue;
                const PatternEdge& pe = q.edges[e];
                int32_t a = pos_of[m][(size_t)pe.from];
                int32_t b = pos_of[m][(size_t)pe.to];
                if (a >= 0 && b >= 0) {
                    const int32_t se = add_edge(a, b, pe, -1);
                    for (size_t e2 = 0; e2 < e; ++e2)
                        if (pl.emap[e2] == se) return "parallel_edges";
                    pl.emap[e] = se;
                    done[e] = 1;
                    --remaining;
                    progress = true;
                    continue;
                }
                if (a < 0 && b < 0) continue;
                const int32_t parent = a >= 0 ? a : b;
                const int32_t v = a >= 0 ? pe.to : pe.from;
                int32_t p = find_share(parent, v);
                bool fresh = false;
                if (p < 0) { p = new_pos(parent, v); fresh = true; }
                if (used[(size_t)p]) return "vertex_collision";
                used[(size_t)p] = 1;
                grow_users(p);
                pos_of[m][(size_t)v] = p;
                if (var_const_anchored(q, v)) sk.anchored[(size_t)p] = 1;
                const int32_t u = a >= 0 ? a : p;
                const int32_t w = a >= 0 ? p : b;
                const int32_t se = add_edge(u, w, pe, fresh ? p : -1);
                for (size_t e2 = 0; e2 < e; ++e2)
                    if (pl.emap[e2] == se) return "parallel_edges";
                pl.emap[e] = se;
                done[e] = 1;
                --remaining;
                progress = true;
            }
            if (!progress) {
                size_t e = 0;
                while (e < q.edges.size() && done[e]) ++e;
                if (e >= q.edges.size()) break;
                int32_t v = q.edges[e].from;
                if (degree_of(q, q.edges[e].to) > degree_of(q, v)) v = q.edges[e].to;
                int32_t p = find_share(-1, v);
                if (p < 0) {
                    p = new_pos(-1, v);
                    sk.pos[(size_t)p].exec_level = ++next_level;
                }
                if (used[(size_t)p]) return "vertex_collision";
                used[(size_t)p] = 1;
                grow_users(p);
                pos_of[m][(size_t)v] = p;
                if (var_const_anchored(q, v)) sk.anchored[(size_t)p] = 1;
            }
        }
        for (size_t v = 0; v < q.vars.size(); ++v) {
            if (q.vars[v].kind != PatternVar::VERTEX) continue;
            if (pos_of[m][v] < 0) return "unmapped";
            pl.vmap[v] = pos_of[m][v];
        }
        pl.vnames = member_value_names(q);
        pl.depart_clause = -2;
        keep.push_back(std::move(pl));
    }
    // One execution level per distinct private owner mask: every member of the mask owns all
    // edges of that step (equal masks keep their edges together), so the step is a trie branch,
    // not a per-edge level.  Shared branches come first (decreasing popcount); a step can only
    // depend on a mask with a larger or equal popcount, because an edge incident to a position
    // is owned by a subset of that position's users.  This also keeps a member from paying a
    // pass row at every private edge of the other members.
    std::vector<uint32_t> pmask;
    for (const auto& e : sk.edge)
        if (!e.core && e.owners) pmask.push_back(e.owners);
    for (size_t p = core_old.size(); p < sk.pos.size(); ++p) {
        if (p < sk.birth_edge.size() && sk.birth_edge[p] >= 0) continue;
        if (sk.users[p]) pmask.push_back(sk.users[p]);
    }
    std::sort(pmask.begin(), pmask.end(), [](uint32_t a, uint32_t b) {
        const int ca = __builtin_popcount(a), cb = __builtin_popcount(b);
        return ca != cb ? ca > cb : a < b;
    });
    pmask.erase(std::unique(pmask.begin(), pmask.end()), pmask.end());
    std::map<uint32_t, int32_t> mask_level;
    int32_t lvl = 0;
    for (uint32_t m : pmask) mask_level[m] = ++lvl;
    for (auto& e : sk.edge) {
        if (e.core) { e.exec_level = 0; continue; }
        auto it = mask_level.find(e.owners);
        e.exec_level = it == mask_level.end() ? lvl : it->second;
    }
    for (size_t p = 0; p < sk.pos.size(); ++p) {
        if (p < core_old.size()) { sk.pos[p].exec_level = 0; continue; }
        const int32_t be = p < sk.birth_edge.size() ? sk.birth_edge[p] : -1;
        if (be >= 0 && (size_t)be < sk.edge.size()) {
            sk.pos[p].exec_level = sk.edge[(size_t)be].exec_level;
        } else {
            auto it = mask_level.find(sk.users[p]);
            sk.pos[p].exec_level = it == mask_level.end() ? lvl : it->second;
        }
    }
    sk.members = grp;
    if (std::getenv("V3_DEBUG_TOPO")) {
        std::fprintf(stderr, "[topo] group=%zu core=%zu np=%zu ne=%zu\n", grp.size(),
                     core_old.size(), sk.pos.size(), sk.edge.size());
        for (size_t p = 0; p < sk.pos.size(); ++p) {
            std::fprintf(stderr, "[topo]  p=%zu lev=%d birth=%d users=%x labels=", p,
                         sk.pos[p].exec_level, sk.birth_parent[p], sk.users[p]);
            for (const auto& x : sk.pos[p].labels) std::fprintf(stderr, "%s,", x.c_str());
            std::fprintf(stderr, "\n");
        }
        for (size_t e = 0; e < sk.edge.size(); ++e)
            std::fprintf(stderr, "[topo]  e=%zu %d->%d lev=%d tree=%d owners=%x core=%d\n", e,
                         sk.edge[e].u, sk.edge[e].w, sk.edge[e].exec_level, (int)sk.edge[e].tree,
                         sk.edge[e].owners, (int)sk.edge[e].core);
    }
    *out_sk = std::move(sk);
    *out_keep = std::move(keep);
    return "";
}

}  // namespace


// ---------------------------------------------------------------- common-prefix envelopes
namespace {

// A copy of the candidate with only the clauses up to `end`.  Used by the prefix estimator; it
// is never evaluated, so the return clause is irrelevant.
Query prefix_query(const Query& q, int32_t end) {
    Query t = q;
    if (end >= 0 && (size_t)end + 1 < t.clauses.size()) t.clauses.resize((size_t)end + 1);
    std::vector<PatternEdge> es;
    for (const auto& e : t.edges)
        if (end < 0 || e.clause <= end) es.push_back(e);
    t.edges.swap(es);
    t.return_clause = -1;
    return t;
}

// Topology and constant anchors of the first MATCH clause.  Labels, relationship types and hop
// ranges are deliberately left out: the mask tests them per member.
std::string first_match_key(const Query& q) {
    const int32_t fm = q.first_match;
    if (fm < 0 || (size_t)fm >= q.clauses.size()) return "";
    if (q.clauses[(size_t)fm].kind != ClauseKind::MATCH) return "";
    const std::vector<int32_t> vo = vertex_order_upto(q, fm);
    if (vo.empty()) return "";
    std::map<int32_t, int32_t> pos;
    for (size_t i = 0; i < vo.size(); ++i) pos[vo[(size_t)i]] = (int32_t)i;
    std::vector<std::string> es;
    for (const auto& e : q.edges) {
        if (e.clause != fm) continue;
        auto a = pos.find(e.from), b = pos.find(e.to);
        if (a == pos.end() || b == pos.end()) return "";
        es.push_back(std::to_string(a->second) + (e.any_dir ? "-" : ">") +
                     std::to_string(b->second));
    }
    std::sort(es.begin(), es.end());
    std::vector<std::string> anchors;
    for (size_t i = 0; i < vo.size(); ++i) {
        const int32_t v = vo[i];
        std::string a;
        for (const auto& pr : q.vars[(size_t)v].props)
            a += pr.first + "=" + val_str(pr.second) + ";";
        for (int32_t ai : q.clauses[(size_t)fm].where) {
            if (ai < 0 || (size_t)ai >= q.atoms.size()) continue;
            const Atom& x = q.atoms[(size_t)ai];
            if (x.var != v || x.cmp.rhs == Comparison::VAR || x.cmp.op != "=" ||
                x.cmp.consts.size() != 1)
                continue;
            a += x.prop + "=" + val_str(x.cmp.consts[0]) + ";";
        }
        std::sort(a.begin(), a.end());
        anchors.push_back(std::to_string(i) + ":" + a);
    }
    return "fm" + std::to_string(fm) + "|n" + std::to_string(vo.size()) + "|" + join(es) + "|" +
           join(anchors, ";");
}

}  // namespace

// Build one common-prefix envelope per group of fallback candidates that share the topology of
// their first MATCH clause.  The envelope evaluates that clause once (including its
// variable-length expansion) and hands each member its prefix rows; the member's remaining
// clauses run per candidate from those rows.  This is the "departure" of Algorithm 1: the shared
// part is the prefix every member has, and the private suffix is completed by the base
// enumerator.  It is what lets IC9/IC5/IC6 share an expensive KNOWS* expansion even though their
// later MATCH clauses differ.
void add_prefix_envelopes(const GraphStore& g, const Round& r, RoundPlan* plan) {
    std::map<std::string, std::vector<int32_t>> groups;
    for (int32_t ci : plan->fallback) {
        if (ci < 0 || (size_t)ci >= r.queries.size()) continue;
        const Query& q = r.queries[(size_t)ci];
        const int32_t fm = q.first_match;
        if (fm < 0 || (size_t)fm >= q.clauses.size()) continue;
        bool has_edge = false, bad = false;
        for (const auto& e : q.edges) {
            if (e.clause != fm) continue;
            has_edge = true;
            // A variable-length edge variable is a list of relationships; the prefix sink only
            // carries one row per skeleton edge, so leave those candidates to Fallback.
            if (!e.var_name.empty() && (e.lo != 1 || e.hi != 1)) bad = true;
        }
        if (!has_edge || bad) continue;
        const std::string key = first_match_key(q);
        if (key.empty()) continue;
        groups[key].push_back(ci);
    }
    std::vector<char> covered(r.queries.size(), 0);
    for (auto& kv : groups) {
        std::vector<int32_t> cand = kv.second;
        if (cand.size() < 2) continue;
        if (cand.size() > 64) cand.resize(64);
        const int32_t seed = cand[0];
        const Query& sq = r.queries[(size_t)seed];
        const int32_t fm = sq.first_match;
        Skeleton sk;
        Placed solo;
        if (!place_member(sq, &sk, &solo, fm).empty()) continue;
        sk.members.assign(1, seed);
        solo.cand = seed;
        std::vector<Placed> keep{solo};
        std::vector<int32_t> S{seed};
        for (size_t k = 1; k < cand.size(); ++k) {
            const int32_t ci = cand[k];
            Skeleton trial = sk;
            Placed pl;
            if (!place_member(r.queries[(size_t)ci], &trial, &pl, fm).empty()) continue;
            trial.members = S;
            trial.members.push_back(ci);
            // The prefix walk is shared when it is cheaper than the members' own first MATCH.
            double own = 0.0, work = 0.0;
            for (int32_t x : trial.members) {
                const Query tq = prefix_query(r.queries[(size_t)x], fm);
                const double e = estimate_query(g, tq);
                own += e;
                work = std::max(work, e);
            }
            if (work >= own * 0.98) continue;
            sk = trial;
            sk.members = S;
            sk.members.push_back(ci);
            S.push_back(ci);
            pl.cand = ci;
            keep.push_back(pl);
        }
        if (keep.size() < 2) continue;
        double own_sum = 0.0, work = 0.0;
        for (const auto& pl : keep) {
            const Query tq = prefix_query(r.queries[(size_t)pl.cand], fm);
            const double e = estimate_query(g, tq);
            own_sum += e;
            work = std::max(work, e);
        }
        // A cheap prefix is not worth a plan of its own: its suffix dominates the round.
        if (own_sum < 5000.0) continue;
        // The budget is the paper's B_S over the members' whole work, not just the shared
        // prefix: the prefix is one operator of each member's plan.
        double full_sum = 0.0;
        for (const auto& pl : keep)
            full_sum += estimate_query(g, r.queries[(size_t)pl.cand]);
        // Below the executor's MCS minimum the prefix would run on the data graph with the
        // per-member mask test; that is not cheaper than the candidates' own scans.
        const double min_budget = std::getenv("V3_MCS_MIN_BUDGET")
                                      ? std::atof(std::getenv("V3_MCS_MIN_BUDGET")) : 0.02;
        if (0.25 * 1e-6 * full_sum < min_budget) continue;
        build_env(r, sk, keep, plan, false, fm);
        EnvelopeSpec& env = plan->prefix_envelopes.back();
        env.q.clear();
        for (int32_t ci : env.members) env.q.push_back(&r.queries[(size_t)ci]);
        env.est_sum = own_sum;
        env.est_work = work;
        env.est_build = estimate_mcs_build(g, env);
        env.est_gain = env.est_sum - env.est_work;
        const double share = std::getenv("V3_MCS_BUILD_SHARE")
                                 ? std::atof(std::getenv("V3_MCS_BUILD_SHARE")) : 0.10;
        env.mcs_worth = env.est_gain > 0.0 && env.est_build <= share * env.est_gain;
        char gb[160];
        std::snprintf(gb, sizeof gb, "%s build=%.0f gain=%.0f share=%.2f",
                      env.mcs_worth ? "ok" : "refuse", env.est_build, env.est_gain, share);
        env.mcs_gate = gb;
        env.budget_sec = 0.25 * 1e-6 * full_sum;
        const int32_t idx = (int32_t)plan->prefix_envelopes.size() - 1;
        for (int32_t ci : env.members) {
            plan->prefix_of[(size_t)ci] = idx;
            covered[(size_t)ci] = 1;
        }
        if (std::getenv("V3_DEBUG_ALIGN"))
            std::fprintf(stderr, "[prefix] key=%s members=%zu est_sum=%.0f est_work=%.0f build=%.0f\n",
                         kv.first.c_str(), env.members.size(), env.est_sum, env.est_work,
                         env.est_build);
    }
    if (!plan->prefix_envelopes.empty()) {
        std::vector<int32_t> fb;
        for (int32_t ci : plan->fallback)
            if (!covered[(size_t)ci]) fb.push_back(ci);
        plan->fallback.swap(fb);
    }
}

// ---------------------------------------------------------------- departure / prefix layout
namespace {

// The alignment test on the first-MATCH classes: the same greedy score as edit_align, but over
// vertex_order_upto so a member with extra clauses is still placed on the shared prefix.
std::vector<int32_t> align_prefix_to_seed(const Query& seed, const Query& q, int32_t end) {
    const std::vector<int32_t> sv = vertex_order_upto(seed, end);
    const std::vector<int32_t> qv = vertex_order_upto(q, end);
    auto deg = [](const Query& x, int32_t v) {
        int32_t d = 0;
        for (const auto& e : x.edges)
            if (e.from == v || e.to == v) ++d;
        return d;
    };
    auto has = [](const Query& x, int32_t a, int32_t b) {
        for (const auto& e : x.edges)
            if ((e.from == a && e.to == b) || (e.from == b && e.to == a)) return true;
        return false;
    };
    std::vector<int32_t> order(sv.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int32_t)i;
    std::stable_sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
        return deg(seed, sv[(size_t)a]) > deg(seed, sv[(size_t)b]);
    });
    std::vector<int32_t> q_of_pos(sv.size(), -1);
    std::vector<char> used(qv.size(), 0);
    for (int32_t sp : order) {
        const int32_t u = sv[(size_t)sp];
        int best = INT32_MIN, best_q = -1;
        for (size_t qi = 0; qi < qv.size(); ++qi) {
            if (used[qi]) continue;
            const int32_t v = qv[qi];
            int score = norm(seed.vars[(size_t)u].labels) == norm(q.vars[(size_t)v].labels)
                            ? kWlambda : 0;
            if (var_const_anchored(seed, u) != var_const_anchored(q, v)) score -= 100000;
            for (size_t p2 = 0; p2 < sv.size(); ++p2) {
                const int32_t q2 = q_of_pos[p2];
                if (q2 < 0) continue;
                score += (has(seed, u, sv[p2]) == has(q, v, q2)) ? 1 : -1;
            }
            if (score > best) { best = score; best_q = (int32_t)qi; }
        }
        if (best_q >= 0 && best > 0) {
            q_of_pos[(size_t)sp] = qv[(size_t)best_q];
            used[(size_t)best_q] = 1;
        }
    }
    return q_of_pos;
}

int32_t find_edge_either(const std::vector<SkeletonEdge>& es, int32_t u, int32_t w) {
    for (size_t k = 0; k < es.size(); ++k)
        if ((es[k].u == u && es[k].w == w) || (es[k].u == w && es[k].w == u))
            return (int32_t)k;
    return -1;
}

// One shared-prefix skeleton over the first MATCH, with the members' extra vertices and edges
// left to their departure.  A seed class is shared when every member aligns a vertex to it; the
// shared classes are connected by edges that every member has with one direction.  Classes that
// cannot be reached through such edges are demoted, because a member without the connecting edge
// cannot share the walk that binds them.  Member-specific edges between shared classes stay as
// close tests; a member whose pattern writes a class or an edge outside the shared prefix
// departs at the first MATCH and the base enumerator completes it from its prefix row.
struct PrefixLayout {
    Skeleton sk;
    std::vector<Placed> placed;
    double own_sum = 0.0;
    double shared_work = 0.0;   // one evaluation of the shared prefix
    double suffix_sum = 0.0;    // per-member completion after departure
    double est_work = 0.0;      // shared_work + suffix_sum
    int32_t shared_pos = 0;
};

bool build_prefix_layout(const GraphStore& g, const Round& r,
                         const std::vector<int32_t>& members, PrefixLayout* out) {
    if (members.size() < 2) return false;
    const int32_t seed_ci = members[0];
    const Query& seed = r.queries[(size_t)seed_ci];
    const int32_t fm = seed.first_match;
    if (fm < 0 || (size_t)fm >= seed.clauses.size() ||
        seed.clauses[(size_t)fm].kind != ClauseKind::MATCH) {
        if (std::getenv("V3_DEBUG_ALIGN")) std::fprintf(stderr, "[prefix] bad fm\n");
        return false;
    }
    for (int32_t ci : members) {
        const Query& q = r.queries[(size_t)ci];
        if (q.first_match != fm || q.sem != seed.sem) {
            if (std::getenv("V3_DEBUG_ALIGN")) std::fprintf(stderr, "[prefix] fm/sem mismatch\n");
            return false;
        }
    }
    Skeleton full;
    Placed solo;
    if (!place_member(seed, &full, &solo, fm).empty()) {
        if (std::getenv("V3_DEBUG_ALIGN")) std::fprintf(stderr, "[prefix] seed unplaced\n");
        return false;
    }
    const std::vector<int32_t> sv = vertex_order_upto(seed, fm);
    std::vector<std::vector<int32_t>> am(members.size());
    for (size_t m = 0; m < members.size(); ++m) {
        const Query& q = r.queries[(size_t)members[m]];
        am[m] = (members[m] == seed_ci) ? std::vector<int32_t>(sv.size(), -1)
                                        : align_prefix_to_seed(seed, q, fm);
        if (members[m] == seed_ci)
            for (size_t p = 0; p < sv.size(); ++p) am[m][p] = sv[p];
    }
    std::vector<char> shared(sv.size(), 1);
    for (size_t p = 0; p < sv.size(); ++p)
        for (size_t m = 0; m < members.size(); ++m)
            if (am[m][p] < 0) shared[p] = 0;

    // Direction of a member's edge between two aligned positions, as +1 (u -> w) or -1.
    // +1 / -1: a directed edge written u -> w / w -> u; +2: undirected (any_dir); 0: absent.
    auto edge_dir = [&](int32_t m, int32_t u, int32_t w) -> int {
        const Query& q = r.queries[(size_t)members[(size_t)m]];
        const int32_t qu = am[(size_t)m][(size_t)u], qw = am[(size_t)m][(size_t)w];
        for (const auto& e : q.edges) {
            if (e.clause != fm) continue;
            if (e.from == qu && e.to == qw) return e.any_dir ? 2 : 1;
            if (e.from == qw && e.to == qu) return e.any_dir ? 2 : -1;
        }
        return 0;
    };
    auto has_edge = [&](int32_t m, int32_t u, int32_t w) -> bool {
        const Query& q = r.queries[(size_t)members[(size_t)m]];
        const int32_t qu = am[(size_t)m][(size_t)u], qw = am[(size_t)m][(size_t)w];
        for (const auto& e : q.edges)
            if (e.clause == fm && ((e.from == qu && e.to == qw) ||
                                   (e.from == qw && e.to == qu)))
                return true;
        return false;
    };
    auto universal = [&](int32_t u, int32_t w, int* orient) {
        int dir = 0;
        for (size_t m = 0; m < members.size(); ++m) {
            const int d = edge_dir((int32_t)m, u, w);
            if (d == 0) return false;   // absent
            if (d == 2) continue;       // undirected: accepts either walk direction
            if (dir == 0) dir = d;
            else if (dir != d) return false;
        }
        *orient = dir == 0 ? 1 : dir;
        return true;
    };

    // Demote classes that no universal-edge path reaches from the root.
    for (;;) {
        std::vector<SkeletonEdge> uni;
        for (const auto& e : full.edge) {
            if (e.clause != fm || !shared[(size_t)e.u] || !shared[(size_t)e.w]) continue;
            int orient = 0;
            if (!universal(e.u, e.w, &orient)) continue;
            SkeletonEdge x = e;
            if (orient < 0) { std::swap(x.u, x.w); }
            uni.push_back(x);
        }
        int32_t root = -1;
        double best = 0.0;
        for (size_t p = 0; p < sv.size(); ++p) {
            if (!shared[p]) continue;
            double card = 0.0;
            for (const auto& lab : full.pos[p].labels) {
                const int32_t id = g.schema().label_of(lab);
                if (id >= 0) card += (double)g.vertices_of((uint32_t)id).size();
            }
            if (card <= 0.0) card = (double)g.num_vertices();
            if (var_const_anchored(seed, sv[p])) card = 1.0;
            if (root < 0 || card < best) { root = (int32_t)p; best = card; }
        }
        if (root < 0) {
            if (std::getenv("V3_DEBUG_ALIGN")) std::fprintf(stderr, "[prefix] no root\n");
            return false;
        }
        std::vector<char> seen(sv.size(), 0);
        std::vector<int32_t> q{root};
        seen[(size_t)root] = 1;
        while (!q.empty()) {
            const int32_t u = q.back();
            q.pop_back();
            for (const auto& e : uni) {
                int32_t v = -1;
                if (e.u == u) v = e.w;
                else if (e.w == u) v = e.u;
                if (v < 0 || seen[(size_t)v]) continue;
                seen[(size_t)v] = 1;
                q.push_back(v);
            }
        }
        int32_t reach = 0, total = 0;
        for (size_t p = 0; p < sv.size(); ++p)
            if (shared[p]) { ++total; if (seen[p]) ++reach; }
        if (reach == total) break;
        for (size_t p = 0; p < sv.size(); ++p)
            if (shared[p] && !seen[p]) shared[p] = 0;
    }
    int32_t nshared = 0;
    for (char x : shared) nshared += x;
    if (nshared < 2) {
        if (std::getenv("V3_DEBUG_ALIGN")) std::fprintf(stderr, "[prefix] nshared=%d\n", nshared);
        return false;
    }

    // Rebuild the skeleton over the shared classes only.
    Skeleton sk = full;
    sk.pos.clear();
    sk.edge.clear();
    std::vector<int32_t> old_to_new(sv.size(), -1);
    for (size_t p = 0; p < sv.size(); ++p) {
        if (!shared[p]) continue;
        old_to_new[p] = (int32_t)sk.pos.size();
        sk.pos.push_back(full.pos[p]);
    }
    auto add_edge = [&](int32_t u, int32_t w, bool tree) {
        if (u < 0 || w < 0 || u == w) return -1;
        int32_t k = find_edge_either(sk.edge, u, w);
        if (k >= 0) {
            if (!tree) sk.edge[(size_t)k].tree = false;
            return k;
        }
        SkeletonEdge x;
        x.u = u;
        x.w = w;
        x.clause = fm;
        x.level = 0;
        x.tree = tree;
        sk.edge.push_back(x);
        return (int32_t)sk.edge.size() - 1;
    };
    // Seed edges first; a universal one is a tree edge, every other seed edge a close test.
    for (const auto& e : full.edge) {
        if (e.clause != fm || !shared[(size_t)e.u] || !shared[(size_t)e.w]) continue;
        int orient = 0;
        const bool uni = universal(e.u, e.w, &orient);
        const int32_t u = old_to_new[(size_t)(orient < 0 ? e.w : e.u)];
        const int32_t w = old_to_new[(size_t)(orient < 0 ? e.u : e.w)];
        add_edge(u, w, uni);
    }
    // Member edges between shared classes that the seed does not write are close tests.
    for (size_t m = 0; m < members.size(); ++m) {
        const Query& q = r.queries[(size_t)members[m]];
        for (const auto& e : q.edges) {
            if (e.clause != fm) continue;
            int32_t pu = -1, pw = -1;
            for (size_t p = 0; p < sv.size(); ++p) {
                if (!shared[p]) continue;
                if (am[m][p] == e.from) pu = (int32_t)p;
                if (am[m][p] == e.to) pw = (int32_t)p;
            }
            if (pu < 0 || pw < 0) continue;
            const int32_t u = old_to_new[(size_t)pu], w = old_to_new[(size_t)pw];
            if (u < 0 || w < 0) continue;
            if (find_edge_either(sk.edge, u, w) < 0) add_edge(u, w, false);
        }
    }

    PrefixLayout L;
    L.sk = sk;
    L.sk.members = members;
    L.shared_pos = nshared;
    for (size_t m = 0; m < members.size(); ++m) {
        const int32_t ci = members[m];
        const Query& q = r.queries[(size_t)ci];
        Placed pl;
        pl.cand = ci;
        pl.vmap.assign(q.vars.size(), -1);
        pl.emap.assign(q.edges.size(), -1);
        // q vertex -> seed class
        std::vector<int32_t> pos_of_q(q.vars.size(), -1);
        for (size_t p = 0; p < sv.size(); ++p)
            if (shared[p] && am[m][p] >= 0)
                pos_of_q[(size_t)am[m][p]] = old_to_new[p];
        for (size_t v = 0; v < q.vars.size(); ++v)
            pl.vmap[v] = pos_of_q[v];
        bool private_suffix = false;
        for (int32_t v : vertex_order_upto(q, fm))
            if (v >= 0 && pl.vmap[(size_t)v] < 0) private_suffix = true;
        for (size_t e = 0; e < q.edges.size(); ++e) {
            if (q.edges[e].clause != fm) { private_suffix = true; continue; }
            const int32_t a = pl.vmap[(size_t)q.edges[e].from];
            const int32_t b = pl.vmap[(size_t)q.edges[e].to];
            if (a < 0 || b < 0) { private_suffix = true; continue; }
            const int32_t se = find_edge_either(sk.edge, a, b);
            if (se < 0) { private_suffix = true; continue; }
            pl.emap[e] = se;
        }
        // A later pattern stage is completed by the base after the same departure.
        for (const auto& c : q.clauses) {
            if (c.kind != ClauseKind::MATCH && c.kind != ClauseKind::OPTIONAL_MATCH) continue;
            if (&c != &q.clauses[(size_t)fm]) private_suffix = true;
        }
        pl.depart_clause = private_suffix ? fm : -2;
        pl.vnames = member_value_names(q);
        L.placed.push_back(std::move(pl));
        const double own = estimate_query(g, q);
        const double pre = estimate_query(g, prefix_query(q, fm));
        L.own_sum += own;
        L.suffix_sum += std::max(0.0, own - pre);
        L.shared_work = std::max(L.shared_work, pre);
    }
    L.est_work = L.shared_work + L.suffix_sum;
    *out = std::move(L);
    return true;
}

}  // namespace

static void build_env(const Round& r, const Skeleton& sk, std::vector<Placed> keep,
                      RoundPlan* plan, bool single, int32_t prefix_end);

static double plan_now() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static RoundPlan align_and_partition_base(const GraphStore& g, const Round& r, uint32_t /*k_limit*/, bool serial_only) {
    constexpr double kGamma = 1e-6;   // seconds per estimated work unit (calibrated on SF1)
    constexpr double kRho = 0.25;
    RoundPlan plan;
    plan.why.assign(r.queries.size(), "");
    plan.replay.assign(r.queries.size(), -1);
    plan.est_own.assign(r.queries.size(), 0.0);
    plan.est_before.assign(r.queries.size(), 0.0);
    plan.est_after.assign(r.queries.size(), 0.0);
    plan.env_of.assign(r.queries.size(), -1);
    plan.prefix_of.assign(r.queries.size(), -1);
    // Phase ledger for the planning profile (V3_PLAN_PROFILE).  A scope prints on destruction,
    // so early returns keep the partial ledger.  Off by default: one getenv per phase.
    const bool plan_prof = std::getenv("V3_PLAN_PROFILE") != nullptr;
    struct PS {
        const char* n;
        double t0;
        bool on;
        ~PS() {
            if (!on) return;
            std::fprintf(stderr, "[plan-phase] %s %.6f\n", n, plan_now() - t0);
            std::fflush(stderr);
        }
    };
    const PS ps_all{"all", plan_now(), plan_prof};
    std::vector<int32_t> uniq;
    {
        const PS pd{"dedup", plan_now(), plan_prof};
        // The canonical key is static text computed when the round is loaded, before any timed
        // cell.  The dedup table views it (no string copy per candidate): rebuilding the key or
        // copying it into a std::map was a pure planning constant on the small text-to-Cypher
        // rounds.
        std::vector<std::string> computed;
        computed.reserve(r.queries.size());
        std::unordered_map<std::string_view, int32_t> first_of;
        first_of.reserve(r.queries.size() * 2 + 1);
        for (size_t i = 0; i < r.queries.size(); ++i) {
            std::string_view key;
            if (!r.queries[i].canon.empty()) {
                key = r.queries[i].canon;
            } else {
                computed.push_back(canonical_key(r.queries[i]));
                key = computed.back();
            }
            auto it = first_of.find(key);
            if (it == first_of.end()) {
                first_of.emplace(key, (int32_t)i);
                uniq.push_back((int32_t)i);
            } else {
                plan.replay[i] = it->second;
            }
        }
    }
    // The 20 ms MCS threshold decides only whether C_S is built.  Below it the round is still
    // aligned and merged, and the masked plan runs directly on the data graph with the label
    // tables and the members' own edge tests.  The full pair matrix is not needed there: the
    // candidates that can merge without a departure have the same ordered join tree, so one
    // canonical key groups them and the existing alignment maps their vertices.  Members with
    // a different tree (whose completion would depart and re-enumerate) stay in Fallback; the
    // no-C_S work estimate would not repay their planning.  Above the threshold the existing
    // alignment and prefix machinery is unchanged.
    const double min_budget = std::getenv("V3_MCS_MIN_BUDGET")
                                  ? std::atof(std::getenv("V3_MCS_MIN_BUDGET")) : 0.02;
    double sum_own = 0.0;
    std::vector<double> exp_cost(r.queries.size(), 0.0);
    {
        const PS pe{"estimate", plan_now(), plan_prof};
        for (int32_t ci : uniq) {
            const double own = estimate_query(g, r.queries[(size_t)ci]);
            plan.est_own[(size_t)ci] = own;
            sum_own += own;
            // The small-batch envelope estimate needs only the member's expansion term; scanning it
            // once here keeps the incremental merge test O(1) per added member.
            exp_cost[(size_t)ci] = expansion_cost(g, r.queries[(size_t)ci], g.schema().types.size());
        }
    }
    // The small-batch planner is also used for a round whose whole work is below 0.1 s
    // (the MASC-home scale): the full pair matrix is overkill there, and the budget gate no
    // longer decides whether the round merges.
    if (!serial_only && !std::getenv("V3_SINGLE_ENVELOPE") &&
        (kRho * kGamma * sum_own < min_budget || kGamma * sum_own < 0.1)) {
        // A round below the topological planning floor (the small-graph MASC batches) has no
        // group that can repay even the union layout; skip the alignment entirely so the
        // per-query / pure-count Fallback keeps the base constant.
        const double min_topo_budget = std::getenv("V3_TOPO_MIN_BUDGET")
                                           ? std::atof(std::getenv("V3_TOPO_MIN_BUDGET"))
                                           : 0.001;
        if (kRho * kGamma * sum_own < min_topo_budget) {
            for (int32_t ci : uniq) {
                if (plan.why[(size_t)ci].empty()) plan.why[(size_t)ci] = "singleton";
                plan.fallback.push_back(ci);
            }
            return plan;
        }
        // Every envelope on this path needs at least five members (a smaller cluster cannot be
        // four times cheaper after the per-branch mask tests), so a round with fewer unique
        // candidates cannot share anything: skip the merge key, the clustering and the layout.
        if (uniq.size() < 5) {
            for (int32_t ci : uniq) {
                if (plan.why[(size_t)ci].empty()) plan.why[(size_t)ci] = "singleton";
                plan.fallback.push_back(ci);
            }
            return plan;
        }
        auto forest_of = [](const Query& q) {
            std::vector<std::pair<int,int>> trees;
            const std::vector<int32_t> vo = pattern_vertex_order(q);
            if (vo.empty()) return trees;
            std::vector<int32_t> par(vo.size());
            for (size_t i = 0; i < par.size(); ++i) par[i] = (int32_t)i;
            std::function<int32_t(int32_t)> find = [&](int32_t x) {
                while (par[(size_t)x] != x) { par[(size_t)x] = par[(size_t)par[(size_t)x]]; x = par[(size_t)x]; }
                return x;
            };
            for (const auto& e : q.edges) {
                if (e.clause < 0 || (size_t)e.clause >= q.clauses.size() ||
                    q.clauses[(size_t)e.clause].kind != ClauseKind::MATCH) continue;
                auto ia = std::find(vo.begin(), vo.end(), e.from);
                auto ib = std::find(vo.begin(), vo.end(), e.to);
                if (ia == vo.end() || ib == vo.end()) continue;
                int a = (int)(ia - vo.begin()), b = (int)(ib - vo.begin());
                if (find(a) != find(b)) { par[(size_t)find(a)] = find(b); if (a > b) std::swap(a, b); trees.push_back({a, b}); }
            }
            std::sort(trees.begin(), trees.end());
            return trees;
        };
        auto first_anchor = [&](const Query& q) {
            const std::vector<int32_t> vo = pattern_vertex_order(q);
            if (vo.empty()) return std::string();
            const PatternVar& pv = q.vars[(size_t)vo[0]];
            std::string a;
            for (const auto& pr : pv.props)
                if (pr.first == "id" || pr.first == "name") a = pr.first + "=" + val_str(pr.second);
            for (int32_t ai : const_atoms_of(q, vo[0], q.first_match)) {
                if (!a.empty()) break;
                const Atom& at = q.atoms[(size_t)ai];
                if (at.prop == "id" || at.prop == "name")
                    a = at.prop + "=" + val_str(at.cmp.consts[0]);
            }
            return a;
        };
        auto merge_key = [&](const Query& q) {
            if (q.first_match < 0) return std::string();
            const std::vector<int32_t> vo = pattern_vertex_order(q);
            if (vo.empty()) return std::string();
            const std::vector<std::pair<int,int>> trees = forest_of(q);
            if (trees.empty()) return std::string();
            std::ostringstream o;
            // The topology edits are the point of this planner: a different spanning tree or
            // vertex count no longer separates two candidates.  Semantics, first MATCH, level
            // sequence and the constant anchor still define the group; build_topo_group then
            // lays the members on their common core and private suffix trie.
            o << "S" << (int)q.sem << "|F" << q.first_match;
            const std::vector<int32_t> lv = clause_levels(q);
            for (int32_t x : lv) o << "|L" << x;
            // Different constant anchors have no common scan; keep them out of the same group
            // instead of letting estimate_envelope reject every pair.
            o << "|A" << first_anchor(q);
            return o.str();
        };
        std::map<std::string, std::vector<int32_t>> sgroups;
        for (int32_t ci : uniq) {
            const Query& q = r.queries[(size_t)ci];
            const std::string why = implementable(q);
            if (!why.empty()) { plan.why[(size_t)ci] = why; plan.fallback.push_back(ci); continue; }
            const std::string key = merge_key(q);
            if (key.empty()) { plan.why[(size_t)ci] = "no_merge_key"; plan.fallback.push_back(ci); continue; }
            sgroups[key].push_back(ci);
        }
        // The merge key only fixes the pipeline shape.  Inside one bucket the candidates are
        // clustered by the M1 label/alignment test, because a batch may contain several
        // unrelated pattern families (MASC's 30 queries form groups of five).  A cluster of one
        // stays in Fallback without any placement work.
        std::vector<std::vector<int32_t>> tgroups;
        for (auto& kv : sgroups) {
            std::vector<int32_t> rest = kv.second;
            std::stable_sort(rest.begin(), rest.end(), [&](int32_t a, int32_t b2) {
                return r.queries[(size_t)a].edges.size() > r.queries[(size_t)b2].edges.size();
            });
            while (!rest.empty()) {
                const int32_t seed = rest.front();
                std::vector<int32_t> grp{seed};
                std::vector<int32_t> left;
                for (size_t k = 1; k < rest.size(); ++k) {
                    if (grp.size() < (size_t)kGroupCap &&
                        topo_pair_ok(r.queries[(size_t)seed], r.queries[(size_t)rest[k]]))
                        grp.push_back(rest[k]);
                    else
                        left.push_back(rest[k]);
                }
                rest.swap(left);
                tgroups.push_back(std::move(grp));
            }
        }
        if (std::getenv("V3_DEBUG_ALIGN")) {
            for (size_t z = 0; z < tgroups.size(); ++z) {
                std::fprintf(stderr, "[tgroup %zu/%zu]", z, tgroups.size());
                for (int32_t ci : tgroups[z]) std::fprintf(stderr, " %d", ci);
                std::fprintf(stderr, "\n");
            }
        }
        bool any_multi = false;
        for (const auto& grp : tgroups)
            if (grp.size() >= 2) { any_multi = true; break; }
        if (!any_multi) {
            for (const auto& grp : tgroups)
                for (int32_t ci : grp) {
                    if (plan.why[(size_t)ci].empty()) plan.why[(size_t)ci] = "singleton";
                    plan.fallback.push_back(ci);
                }
            return plan;
        }
        for (auto& grp : tgroups) {
            // The widest member is the skeleton: every other member then only lacks edges,
            // which the mask accepts, instead of adding close edges the seed does not have.
            std::stable_sort(grp.begin(), grp.end(), [&](int32_t a, int32_t b2) {
                return r.queries[(size_t)a].edges.size() > r.queries[(size_t)b2].edges.size();
            });
            // A cluster below five cannot be four times cheaper than its members after the
            // per-branch mask tests.
            double sum_own = 0.0;
            for (int32_t ci : grp) sum_own += plan.est_own[(size_t)ci];
            if (grp.size() < 5) {
                for (int32_t ci : grp) {
                    if (plan.why[(size_t)ci].empty()) plan.why[(size_t)ci] = "singleton";
                    plan.fallback.push_back(ci);
                }
                continue;
            }
            // Cluster already fixes the members; lay the whole cluster out once (a per-member
            // incremental rebuild repeats the alignment and dominates the small-batch plan).
            Skeleton sk;
            std::vector<Placed> keep;
            const std::string why = build_topo_group(g, r, grp, &sk, &keep);
            if (!why.empty()) {
                for (int32_t ci : grp) {
                    plan.why[(size_t)ci] = why;
                    plan.fallback.push_back(ci);
                }
                continue;
            }
            sk.members = grp;
            const double est = estimate_envelope(g, r, sk);
            // The fixed 3 ms floor was a proxy for the layout cost and was mis-calibrated on
            // WordNet: its groups estimate 1.4-1.6 ms and, once the close edges prune at the
            // position that completes them, execute in 2.3 ms per group against 5 ms per-query.
            // The floor is therefore replaced by the gain the group must return over the measured
            // layout plus estimate cost of one group (0.2 ms), and the existing 4x (rho) budget
            // still decides whether the masked walk is worth it.
            const double min_gain = std::getenv("V3_TOPO_MIN_GAIN")
                                        ? std::atof(std::getenv("V3_TOPO_MIN_GAIN")) : 0.0002;
            const double gain_sec = kGamma * (sum_own - est);
            if (std::getenv("V3_DEBUG_TOPO_EST"))
                std::fprintf(stderr, "[topogate] sum=%.0f est=%.0f gain=%.6f min=%.6f\n",
                             sum_own, est, gain_sec, min_gain);
            if (est >= sum_own || est > kRho * sum_own || gain_sec < min_gain) {
                for (int32_t ci : grp) {
                    plan.why[(size_t)ci] = est >= sum_own ? "not_worth" : "over_budget";
                    plan.fallback.push_back(ci);
                }
                continue;
            }
            build_env(r, sk, keep, &plan, false);
            EnvelopeSpec& env = plan.envelopes.back();
            env.est_sum = sum_own;
            env.est_work = est;
            env.est_build = 0.0;
            env.est_gain = sum_own - est;
            env.mcs_worth = false;
            env.mcs_gate = "topology no_mcs";
            env.budget_sec = kRho * kGamma * env.est_sum;
            for (int32_t ci : env.members)
                if (plan.env_of[(size_t)ci] < 0) plan.env_of[(size_t)ci] = (int32_t)plan.envelopes.size() - 1;
        }
        return plan;
    }
    // the candidates a masked evaluation can carry
    std::vector<int32_t> members;
    for (int32_t ci : uniq) {
        const std::string why = implementable(r.queries[ci]);
        if (!why.empty()) {
            plan.why[ci] = why;
            plan.fallback.push_back(ci);
            continue;
        }
        members.push_back(ci);
    }
    if (members.size() < 2) {
        // No pair can share a whole envelope, but the fallback candidates may still share their
        // first MATCH (IC5's candidates with an aggregating WITH are the common case).
        for (int32_t ci : members) {
            plan.why[(size_t)ci] = implementable(r.queries[(size_t)ci]);
            if (plan.why[(size_t)ci].empty()) plan.why[(size_t)ci] = "singleton";
            plan.fallback.push_back(ci);
        }
        add_prefix_envelopes(g, r, &plan);
        return plan;
    }
    if (serial_only) {
        // P3/E4 serial baseline: the per-candidate executor (M6) evaluates every candidate;
        // this is the base enumerator, not a one-member masked envelope
        for (int32_t ci : members) {
            plan.why[(size_t)ci] = "serial";
            plan.fallback.push_back(ci);
        }
        return plan;
    }
    if (std::getenv("V3_SINGLE_ENVELOPE")) {
        // Test path for the W assertion: every implementable candidate gets its own one-member
        // masked envelope.  Its work must equal the per-candidate path, so the two W counters
        // can be compared on the same query.
        for (int32_t ci : members) {
            const std::string impl = implementable(r.queries[(size_t)ci]);
            if (!impl.empty()) {
                plan.why[(size_t)ci] = impl;
                plan.fallback.push_back(ci);
                continue;
            }
            Skeleton sk;
            Placed pl;
            const std::string why = place_member(r.queries[(size_t)ci], &sk, &pl);
            if (!why.empty()) {
                plan.why[(size_t)ci] = why;
                plan.fallback.push_back(ci);
                continue;
            }
            sk.members.assign(1, ci);
            pl.cand = ci;
            build_env(r, sk, std::vector<Placed>{pl}, &plan, true);
        }
        return plan;
    }
    // M1 Grouping: the candidates of the round are aligned to a seed without a vertex-count
    // bucket.  The label test and the alignment test (tau = ceil(theta * max(|V_s|, |V_q|)))
    // decide whether q joins the seed's group; F = 64 caps a group.  A group is then split by M2
    // into a full envelope (members whose whole pattern fits the skeleton) and a shared-prefix
    // envelope (members whose extra vertices/edges are completed after a departure).
    const size_t n = members.size();
    // The pair matrix is the planning hot spot: every pair runs the label test and the greedy
    // edit alignment.  Both re-derived the vertex order, the degrees and the edge set per call,
    // and the alignment scanned the edge list for every candidate position.  Cache the order
    // (with its degree order), the per-vertex label set and the adjacency of each candidate once
    // and test edges as bits, so one alignment is O(p^3) bit operations instead of O(p^3 E)
    // string/edge scans.
    struct PairCache {
        std::vector<int32_t> vo;
        std::vector<int32_t> ord;                 // vo indices in decreasing degree
        std::vector<int32_t> oi;                  // query vertex -> vo index
        std::vector<uint32_t> adj;                // vo x vo adjacency bits
        std::vector<std::vector<std::string>> lab;  // query vertex -> sorted labels
        std::vector<std::string> lset;            // sorted label-set key per vo position
        int32_t nz = 0;                           // vertices with at least one edge
    };
    std::vector<PairCache> pc(n);
    const PS ps_paircache{"paircache", plan_now(), plan_prof};
    for (size_t i = 0; i < n; ++i) {
        const Query& q = r.queries[(size_t)members[i]];
        PairCache& c = pc[i];
        c.vo = pattern_vertex_order(q);
        c.oi.assign(q.vars.size(), -1);
        for (size_t p = 0; p < c.vo.size(); ++p) c.oi[(size_t)c.vo[p]] = (int32_t)p;
        c.ord.resize(c.vo.size());
        for (size_t p = 0; p < c.ord.size(); ++p) c.ord[p] = (int32_t)p;
        std::stable_sort(c.ord.begin(), c.ord.end(), [&](int32_t a, int32_t b2) {
            return degree_of(q, c.vo[(size_t)a]) > degree_of(q, c.vo[(size_t)b2]);
        });
        c.adj.assign(c.vo.size() * c.vo.size(), 0);
        for (const auto& e : q.edges) {
            const int32_t a = c.oi[(size_t)e.from], b2 = c.oi[(size_t)e.to];
            if (a < 0 || b2 < 0) continue;
            c.adj[(size_t)a * c.vo.size() + (size_t)b2] = 1;
            c.adj[(size_t)b2 * c.vo.size() + (size_t)a] = 1;
        }
        c.lab.resize(q.vars.size());
        for (size_t v = 0; v < q.vars.size(); ++v) {
            c.lab[v] = q.vars[v].labels;
            std::sort(c.lab[v].begin(), c.lab[v].end());
        }
        // The cheap half of the pair test: the multiset of individual vertex labels and the
        // number of non-isolated vertices.  Two candidates can pair at most `label_overlap`
        // vertices by a shared label and, of the rest, only vertices that have an edge in both
        // patterns can still score through neighbourhood agreement.  Both bounds are necessary,
        // so a pair that fails them is rejected without running the O(p^3) alignment.
        // Individual labels (not the per-vertex set) are counted: `label_ok` accepts any
        // intersection, so a set-level key would undercount {Post,Comment} vs {Post}.
        c.lset.reserve(c.vo.size());
        for (int32_t v : c.vo)
            for (const auto& L : c.lab[(size_t)v]) c.lset.push_back(L);
        std::sort(c.lset.begin(), c.lset.end());
        for (int32_t v : c.vo)
            if (degree_of(q, v) > 0) ++c.nz;
    }
    const PS ps_pairs{"pairs", plan_now(), plan_prof};
    auto labels_equal = [&](size_t i, int32_t u, size_t j, int32_t v) {
        return pc[i].lab[(size_t)u] == pc[j].lab[(size_t)v];
    };
    // Exact upper bound on the number of vertices a one-to-one label matching can pair: the
    // multiset intersection of the two label-set signatures.
    auto label_overlap_ub = [&](size_t i, size_t j) {
        const auto& a = pc[i].lset;
        const auto& b = pc[j].lset;
        size_t x = 0, y = 0, inter = 0;
        while (x < a.size() && y < b.size()) {
            if (a[x] == b[y]) { ++inter; ++x; ++y; }
            else if (a[x] < b[y]) ++x;
            else ++y;
        }
        return inter;
    };
    auto label_ok = [&](size_t i, size_t j) {
        const auto& oa = pc[i].vo;
        const auto& ob = pc[j].vo;
        std::vector<char> used(ob.size(), 0);
        size_t inter = 0;
        for (int32_t va : oa) {
            for (size_t k = 0; k < ob.size(); ++k) {
                if (used[k]) continue;
                bool hit = false;
                const auto& la = pc[i].lab[(size_t)va];
                const auto& lb = pc[j].lab[(size_t)ob[k]];
                for (const auto& x : la)
                    for (const auto& y : lb)
                        if (x == y) hit = true;
                if (!hit) continue;
                used[k] = 1;
                ++inter;
                break;
            }
        }
        const size_t m = std::max(oa.size(), ob.size());
        return m == 0 || (double)inter >= kLabelOverlap * (double)m;
    };
    auto align_pair = [&](size_t i, size_t j) {
        const PairCache& A = pc[i];
        const PairCache& B = pc[j];
        const size_t pA = A.vo.size(), pB = B.vo.size();
        std::vector<int32_t> q_of_pos(pA, -1);
        std::vector<char> used(pB, 0);
        int paired = 0;
        for (int32_t sp : A.ord) {
            const int32_t u = A.vo[(size_t)sp];
            int best_score = INT32_MIN, best_q = -1;
            for (size_t qi = 0; qi < pB; ++qi) {
                if (used[qi]) continue;
                const int32_t v = B.vo[qi];
                const int score0 = labels_equal(i, u, j, v) ? kWlambda : 0;
                int score = score0;
                const uint32_t* arow = &A.adj[(size_t)sp * pA];
                const uint32_t* brow = &B.adj[qi * pB];
                for (size_t p2 = 0; p2 < pA; ++p2) {
                    const int32_t q2 = q_of_pos[p2];
                    if (q2 < 0) continue;
                    const int32_t qi2 = B.oi[(size_t)q2];
                    if (qi2 < 0) continue;
                    const bool es = arow[p2] != 0;
                    const bool eq = brow[(size_t)qi2] != 0;
                    score += (es == eq) ? 1 : -1;
                }
                if (score > best_score) { best_score = score; best_q = (int32_t)qi; }
            }
            if (best_q >= 0 && best_score > 0) {
                q_of_pos[(size_t)sp] = B.vo[(size_t)best_q];
                used[(size_t)best_q] = 1;
                ++paired;
            }
        }
        return paired;
    };
    std::vector<char> placed(n, 0);
    std::vector<std::vector<int32_t>> paired(n, std::vector<int32_t>(n, -1));
    const PS pm_matrix{"pairmatrix", plan_now(), plan_prof};
    for (size_t i = 0; i < n; ++i) {
        const size_t ns = pc[i].vo.size();
        for (size_t j = 0; j < n; ++j) {
            if (i == j) continue;
            const size_t nq = pc[j].vo.size();
            const int tau = (int)std::ceil(kTheta * (double)std::max(ns, nq));
            // A pair can bind at most min(|V_s|, |V_q|) vertices; below tau the alignment cannot
            // pass whatever its score.
            if ((int)std::min(ns, nq) < tau) continue;
            const size_t label_ub = label_overlap_ub(i, j);
            if ((double)label_ub < kLabelOverlap * (double)std::max(ns, nq)) continue;
            // Label-unmatched vertices with no edge in either pattern can never score > 0.
            if ((int)(label_ub + (size_t)std::min(pc[i].nz, pc[j].nz)) < tau) continue;
            if (!label_ok(i, j)) continue;
            const int paired_n = align_pair(i, j);
            if (paired_n >= tau) paired[i][j] = paired_n;
        }
    }
    const PS pm_select{"groupselect", plan_now(), plan_prof};
    std::vector<std::array<int, 3>> rich(n);
    for (size_t j = 0; j < n; ++j) {
        const Query& q = r.queries[(size_t)members[j]];
        const std::vector<int32_t> lv = clause_levels(q);
        int maxlev = 0;
        for (int32_t x : lv) maxlev = std::max(maxlev, x);
        rich[j] = {maxlev, (int)q.edges.size(), (int)vertex_order(q).size()};
    }
    auto compatible = [&](size_t si) {
        int fits = 0;
        for (size_t j = 0; j < n; ++j) {
            if (j == si || placed[j]) continue;
            if (paired[si][j] >= 0) ++fits;
        }
        return fits;
    };
    std::vector<std::vector<int32_t>> groups;
    size_t align_pairs = 0;
    for (size_t i = 0; i < n; ++i) {
        if (placed[i]) continue;
        size_t si = i;
        int best_fits = -1;
        for (size_t j = i; j < n; ++j) {
            if (placed[j]) continue;
            const int f = compatible(j);
            if (f > best_fits || (f == best_fits && rich[j] > rich[si])) {
                best_fits = f;
                si = j;
            }
        }
        std::vector<std::pair<int, size_t>> cand;
        for (size_t j = 0; j < n; ++j) {
            if (j == si || placed[j]) continue;
            if (paired[si][j] >= 0) cand.emplace_back(paired[si][j], j);
        }
        std::stable_sort(cand.begin(), cand.end(), [](const auto& a, const auto& b2) {
            return a.first > b2.first;
        });
        std::vector<int32_t> grp{members[si]};
        placed[si] = 1;
        for (size_t k = 0; k < cand.size() && grp.size() < (size_t)kGroupCap; ++k) {
            grp.push_back(members[cand[k].second]);
            placed[cand[k].second] = 1;
            align_pairs += (size_t)cand[k].first;
        }
        groups.push_back(std::move(grp));
    }
    // M2 Partitioning: full envelopes first, then shared-prefix envelopes for the members whose
    // topology does not fit.  Both merges use the estimated work; the prefix estimate charges
    // each member's completion after its departure (suffix_sum), so a private suffix is never
    // treated as free.
    auto set_prefix_est = [&](EnvelopeSpec& env, const PrefixLayout& L) {
        env.q.clear();
        for (int32_t ci : env.members) env.q.push_back(&r.queries[(size_t)ci]);
        env.est_sum = L.own_sum;
        env.est_work = L.est_work;
        env.est_build = estimate_mcs_build(g, env);
        env.est_gain = env.est_sum - env.est_work;
        const double share = std::getenv("V3_MCS_BUILD_SHARE")
                                 ? std::atof(std::getenv("V3_MCS_BUILD_SHARE")) : 0.10;
        env.mcs_worth = env.est_gain > 0.0 && env.est_build <= share * env.est_gain;
        char gb[160];
        std::snprintf(gb, sizeof gb, "%s build=%.0f gain=%.0f share=%.2f",
                      env.mcs_worth ? "ok" : "refuse", env.est_build, env.est_gain, share);
        env.mcs_gate = gb;
        env.budget_sec = kRho * kGamma * env.est_sum;
    };
    auto fallback_ci = [&](int32_t ci, const std::string& why) {
        if (plan.why[(size_t)ci].empty()) plan.why[(size_t)ci] = why;
        if (std::find(plan.fallback.begin(), plan.fallback.end(), ci) == plan.fallback.end())
            plan.fallback.push_back(ci);
    };
    const PS pm_build{"groupbuild", plan_now(), plan_prof};
    for (auto& grp : groups) {
        const int32_t seed = grp[0];
        // Seventh edit class on the high-budget path: try the common-core + private-trie
        // layout before the full/prefix split.  It accepts only when the group is large enough
        // to repay the layout and its estimate fits the same rho budget; otherwise the existing
        // full / prefix / fallback decision runs unchanged.
        if (grp.size() >= 3 && r.family.rfind("ldbc", 0) == 0 &&
            !std::getenv("V3_TOPO_HIGH_OFF")) {
            double sum = 0.0;
            for (int32_t ci : grp) {
                if (plan.est_own[(size_t)ci] <= 0.0)
                    plan.est_own[(size_t)ci] = estimate_query(g, r.queries[(size_t)ci]);
                sum += plan.est_own[(size_t)ci];
            }
            if (kRho * kGamma * sum >= min_budget) {
                Skeleton tsk;
                std::vector<Placed> tkeep;
                const std::string tw = build_topo_group(g, r, grp, &tsk, &tkeep);
                if (tw.empty()) {
                    tsk.members = grp;
                    int32_t ncore = 0;
                    for (const auto& sp : tsk.pos) if (sp.exec_level == 0) ++ncore;
                    double te = estimate_envelope(g, r, tsk);
                    double min_own = 0.0;
                    for (int32_t ci : grp) {
                        const double o = plan.est_own[(size_t)ci];
                        if (min_own <= 0.0 || o < min_own) min_own = o;
                    }
                    // The topological estimate returns a single anchored root as 1; on an LDBC
                    // anchor chain that undercounts the fanout of the rooted expansion.  Floor
                    // it at a quarter of the cheapest member (still far below the group sum).
                    te = std::max(te, 0.25 * min_own);
                    if (ncore >= 3 && te < sum && te <= 0.6 * sum) {
                        build_env(r, tsk, tkeep, &plan, false);
                        EnvelopeSpec& env = plan.envelopes.back();
                        env.est_sum = sum;
                        env.est_work = te;
                        env.est_build = 0.0;
                        env.est_gain = sum - te;
                        env.mcs_worth = false;
                        env.mcs_gate = "topology no_mcs";
                        env.budget_sec = kRho * kGamma * sum;
                        for (int32_t ci : env.members)
                            if (plan.env_of[(size_t)ci] < 0)
                                plan.env_of[(size_t)ci] = (int32_t)plan.envelopes.size() - 1;
                        continue;
                    }
                }
            }
        }
        Skeleton sk;
        Placed solo;
        const std::string why = place_member(r.queries[(size_t)seed], &sk, &solo);
        if (!why.empty()) {
            plan.why[(size_t)seed] = why;
            for (int32_t ci : grp) fallback_ci(ci, why);
            continue;
        }
        sk.members.assign(1, seed);
        solo.cand = seed;
        plan.est_own[(size_t)seed] = estimate_query(g, r.queries[(size_t)seed]);
        std::vector<Placed> keep{solo};
        std::vector<int32_t> S{seed};
        std::vector<int32_t> prefix_cand;
        // The envelope estimate of the current skeleton is the previous iteration's accepted
        // estimate, so it is carried instead of recomputed for every member: the incremental
        // merge test was O(n^2) estimate calls for five to eight candidates.
        double before_est = estimate_envelope(g, r, sk);
        for (size_t k = 1; k < grp.size(); ++k) {
            const int32_t ci = grp[k];
            const double own = estimate_query(g, r.queries[(size_t)ci]);
            plan.est_own[(size_t)ci] = own;
            Skeleton trial = sk;
            Placed pl;
            const std::string w = place_member(r.queries[(size_t)ci], &trial, &pl);
            if (!w.empty()) {
                prefix_cand.push_back(ci);
                continue;
            }
            trial.members = S;
            trial.members.push_back(ci);
            const double after = estimate_envelope(g, r, trial);
            const double before = before_est;
            plan.est_before[(size_t)ci] = before;
            plan.est_after[(size_t)ci] = after;
            if (after >= before + own) {
                prefix_cand.push_back(ci);
                continue;
            }
            sk = trial;
            sk.members = S;
            sk.members.push_back(ci);
            S.push_back(ci);
            before_est = after;
            pl.cand = ci;
            keep.push_back(pl);
        }
        if (keep.size() > 1) {
            build_env(r, sk, keep, &plan, false);
            EnvelopeSpec& env = plan.envelopes.back();
            if (env.members.size() > 1) {
                double sumw = 0.0;
                for (int32_t ci : env.members) sumw += estimate_query(g, r.queries[(size_t)ci]);
                env.budget_sec = kRho * kGamma * sumw;
                env.est_sum = sumw;
                env.q.clear();
                for (int32_t ci : env.members) env.q.push_back(&r.queries[(size_t)ci]);
                Skeleton merged;
                merged.pos = env.pos;
                merged.members = env.members;
                env.est_work = estimate_envelope(g, r, merged);
                env.est_build = estimate_mcs_build(g, env);
                env.est_gain = env.est_sum - env.est_work;
                const double share = std::getenv("V3_MCS_BUILD_SHARE")
                                         ? std::atof(std::getenv("V3_MCS_BUILD_SHARE")) : 0.10;
                env.mcs_worth = env.est_gain > 0.0 && env.est_build <= share * env.est_gain;
                char gb[160];
                std::snprintf(gb, sizeof gb, "%s build=%.0f gain=%.0f share=%.2f",
                              env.mcs_worth ? "ok" : "refuse", env.est_build, env.est_gain, share);
                env.mcs_gate = gb;
                for (int32_t ci : env.members)
                    if (plan.env_of[(size_t)ci] < 0)
                        plan.env_of[(size_t)ci] = (int32_t)plan.envelopes.size() - 1;
            }
        } else {
            // The seed did not fit any other member's full pattern: all group members enter the
            // prefix pool.
            for (int32_t ci : grp)
                if (std::find(prefix_cand.begin(), prefix_cand.end(), ci) == prefix_cand.end())
                    prefix_cand.push_back(ci);
        }
        // Shared-prefix envelope: add the candidates in decreasing aligned-vertex order while
        // the estimated work (shared prefix + per-member completion) is below separate
        // evaluation.
        std::vector<int32_t> rest = prefix_cand;
        if (rest.size() >= 2) {
            std::stable_sort(rest.begin(), rest.end(), [&](int32_t a, int32_t b2) {
                int ia = -1, ib = -1;
                for (size_t z = 0; z < n; ++z) {
                    if (members[z] == a) ia = (int)z;
                    if (members[z] == b2) ib = (int)z;
                }
                const int pa = ia >= 0 ? (int)rich[(size_t)ia][2] : 0;
                const int pb = ib >= 0 ? (int)rich[(size_t)ib][2] : 0;
                return pa > pb;
            });
            while (rest.size() >= 2) {
                std::vector<int32_t> S2{rest[0]};
                PrefixLayout best;
                bool have = false;
                for (size_t k = 1; k < rest.size(); ++k) {
                    std::vector<int32_t> trial = S2;
                    trial.push_back(rest[k]);
                    PrefixLayout L;
                    if (!build_prefix_layout(g, r, trial, &L)) {
                        if (std::getenv("V3_DEBUG_ALIGN"))
                            std::fprintf(stderr, "[prefix] build failed size=%zu seed=%d\n",
                                         trial.size(), trial[0]);
                        continue;
                    }
                    if (L.shared_pos < 2) continue;
                    if (std::getenv("V3_DEBUG_ALIGN"))
                        std::fprintf(stderr, "[prefix] size=%zu shared=%d own=%.1f work=%.1f\n",
                                     trial.size(), L.shared_pos, L.own_sum, L.est_work);
                    // Below the executor's MCS minimum a prefix is walked on the data graph
                    // with per-member tests; on the small MASC blocks that costs more than the
                    // per-candidate plans it replaces.  The same gate as add_prefix_envelopes.
                    const double min_budget = std::getenv("V3_MCS_MIN_BUDGET")
                                                  ? std::atof(std::getenv("V3_MCS_MIN_BUDGET"))
                                                  : 0.02;
                    const double budget = kRho * kGamma * L.own_sum;
                    if (budget >= min_budget && L.est_work < L.own_sum - 1e-9) {
                        S2 = trial;
                        best = std::move(L);
                        have = true;
                    }
                }
                if (have && S2.size() >= 2) {
                    const int32_t fm = r.queries[(size_t)S2[0]].first_match;
                    build_env(r, best.sk, best.placed, &plan, false, fm);
                    EnvelopeSpec& env = plan.prefix_envelopes.back();
                    set_prefix_est(env, best);
                    const int32_t idx = (int32_t)plan.prefix_envelopes.size() - 1;
                    for (int32_t ci : env.members) plan.prefix_of[(size_t)ci] = idx;
                    std::vector<int32_t> left;
                    for (int32_t ci : rest)
                        if (std::find(S2.begin(), S2.end(), ci) == S2.end()) left.push_back(ci);
                    rest.swap(left);
                } else {
                    break;
                }
            }
            for (int32_t ci : rest) fallback_ci(ci, "prefix");
        } else {
            for (int32_t ci : prefix_cand) fallback_ci(ci, "singleton");
        }
    }
    // Common-prefix envelopes for candidates left to Fallback: share the first MATCH (often the
    // expensive variable-length expansion) and complete each member's private suffix alone.
    add_prefix_envelopes(g, r, &plan);
    if (std::getenv("V3_DEBUG_ALIGN")) {
        std::fprintf(stderr, "envs %zu prefix %zu fallback %zu replay %zu groups %zu align_pairs %zu\n",
                     plan.envelopes.size(), plan.prefix_envelopes.size(), plan.fallback.size(),
                     r.queries.size() - uniq.size(), groups.size(), align_pairs);
        for (const auto& w : plan.why)
            if (!w.empty()) std::fprintf(stderr, "why %s\n", w.c_str());
        for (const auto& e : plan.envelopes) {
            std::fprintf(stderr, "pos:");
            for (const auto& p : e.pos) std::fprintf(stderr, " %s", p.name.c_str());
            std::fprintf(stderr, " levels %zu budget %.6f\n", e.levels.size(), e.budget_sec);
        }
    }
    return plan;
}

RoundPlan align_and_partition(const GraphStore& g, const Round& r, uint32_t k_limit, bool serial_only) {
    RoundPlan plan = align_and_partition_base(g, r, k_limit, serial_only);
    // OR-pattern candidates that the mask would evaluate as a conjunction stay on M6.  This is
    // after the base plan so candidates already on Fallback keep their original ledger.
    if (!serial_only) fallback_unmaskable_or(r, &plan);
    // Gate: the normal planner formed no full envelope.  This is deliberately after the whole
    // baseline plan, so every round that already forms one keeps its plan and result bit for bit.
    if (serial_only || !plan.envelopes.empty()) return plan;
    if (std::getenv("V3_DEPTH_CHAIN_OFF")) return plan;
    DepthChainSpec spec;
    if (!detect_depth_chain(g, r, &spec)) return plan;
    plan.prefix_envelopes.clear();
    plan.prefix_of.assign(r.queries.size(), -1);
    plan.fallback.clear();
    plan.env_of.assign(r.queries.size(), -1);
    plan.depth_chain = true;
    plan.dc_members = spec.members;
    plan.dc_lengths = spec.lengths;
    plan.dc_max_len = spec.max_len;
    plan.why.assign(r.queries.size(), "");
    for (size_t m = 0; m < spec.members.size(); ++m) {
        const int32_t ci = spec.members[m];
        plan.why[(size_t)ci] = "depth_chain";
        plan.env_of[(size_t)ci] = 0;
    }
    return plan;
}

static void build_env(const Round& r, const Skeleton& sk, std::vector<Placed> keep,
                      RoundPlan* plan, bool single, int32_t prefix_end);

// Two skeletons share an edge (same position pair, same types and direction).
static void build_env(const Round& r, const Skeleton& sk_in, std::vector<Placed> keep_in,
                      RoundPlan* planp, bool single, int32_t prefix_end) {
    Skeleton sk = sk_in;
    const std::vector<Placed>& keep = keep_in;
    RoundPlan& plan = *planp;
    EnvelopeSpec env;
    env.serial = single;
    env.prefix_end = prefix_end;
    env.topology = sk.topo;
    if (keep.empty()) return;
    auto exlev = [](const SkeletonEdge& e) {
        return e.exec_level >= 0 ? e.exec_level : e.level;
    };
    const size_t NM = keep.size();
    const size_t NP = sk.pos.size();
    const size_t NE = sk.edge.size();
    // label unions per position (the class label set)
    for (auto& sp : sk.pos) sp.labels.clear();
    for (const auto& pl : keep) {
        const Query& q = r.queries[pl.cand];
        for (size_t v = 0; v < q.vars.size(); ++v) {
            if (q.vars[v].kind != PatternVar::VERTEX || pl.vmap[v] < 0) continue;
            auto& L = sk.pos[(size_t)pl.vmap[v]].labels;
            for (const auto& l : q.vars[v].labels)
                if (std::find(L.begin(), L.end(), l) == L.end()) L.push_back(l);
            std::sort(L.begin(), L.end());
        }
    }
    // edge unions (types / direction / hop range) over the members that write the edge
    for (const auto& pl : keep) {
        const Query& q = r.queries[pl.cand];
        for (size_t e = 0; e < q.edges.size(); ++e) {
            if (pl.emap[e] < 0) continue;
            auto& E = sk.edge[(size_t)pl.emap[e]];
            for (const auto& x : q.edges[e].types)
                if (std::find(E.types.begin(), E.types.end(), x) == E.types.end()) E.types.push_back(x);
            E.types = norm(E.types);
            E.any_dir = E.any_dir || q.edges[e].any_dir;
            E.lo = std::min(E.lo, q.edges[e].lo);
            E.hi = (E.hi < 0 || q.edges[e].hi < 0) ? -1 : std::max(E.hi, q.edges[e].hi);
        }
    }
    env.pos = sk.pos;
    env.edges = sk.edge;
    env.clause_level = sk.clause_level;
    env.mclause_level.clear();
    env.pos_lev.assign(NP, INT32_MAX);
    for (size_t p = 0; p < NP; ++p)
        if (sk.pos[p].exec_level >= 0) env.pos_lev[p] = sk.pos[p].exec_level;
    for (const auto& e : sk.edge)
        for (int32_t p : {e.u, e.w})
            if (p >= 0 && (size_t)p < NP && sk.pos[(size_t)p].exec_level < 0)
                env.pos_lev[(size_t)p] = std::min(env.pos_lev[(size_t)p], exlev(e));
    for (const auto& pl : keep) {
        const Query& q = r.queries[pl.cand];
        const std::vector<int32_t> mclev = clause_levels(q);
        env.mclause_level.push_back(mclev);
        for (size_t c = 0; c < q.clauses.size(); ++c) {
            const Clause& cl = q.clauses[c];
            if (cl.kind != ClauseKind::MATCH && cl.kind != ClauseKind::OPTIONAL_MATCH) continue;
            if (c >= mclev.size()) continue;
            const int32_t lv = mclev[c];
            if (lv < 0) continue;
            for (int32_t v : cl.vars) {
                const int32_t p = pl.vmap[(size_t)v];
                if (p >= 0 && (size_t)p < NP && sk.pos[(size_t)p].exec_level < 0)
                    env.pos_lev[(size_t)p] = std::min(env.pos_lev[(size_t)p], lv);
            }
        }
    }
    // a position that only a predicate stage touches is bound at level 0
    for (auto& x : env.pos_lev)
        if (x == INT32_MAX) x = 0;
    int32_t maxl = 0;
    for (int32_t x : env.pos_lev) maxl = std::max(maxl, x);
    for (const auto& e : sk.edge) maxl = std::max(maxl, exlev(e));
    env.core_positions = 0;
    for (const auto& sp : sk.pos)
        if (sk.topo && sp.exec_level == 0) ++env.core_positions;
    env.levels.resize((size_t)maxl + 1);
    for (auto& L : env.levels) {
        L.labels.assign(NM, std::vector<std::string>(NP));
        L.types.assign(NM, std::vector<std::vector<std::string>>(NE));
        L.dir.assign(NM, std::vector<uint8_t>(NE, 1));
        L.any_dir.assign(NM, std::vector<uint8_t>(NE, 0));
        L.hop_lo.assign(NM, std::vector<int32_t>(NE, 1));
        L.hop_hi.assign(NM, std::vector<int32_t>(NE, 1));
        L.opt.assign(NM, 0);
        L.opt_index.assign(NM, -1);
        L.present.assign(NM, std::vector<uint8_t>(NP, 0));
        L.path_dir.assign(NM, std::vector<uint8_t>(NE, 1));
    }
    for (size_t mi = 0; mi < NM; ++mi) {
        const Placed& pl = keep[mi];
        const Query& q = r.queries[pl.cand];
        const int32_t m = (int32_t)mi;
        env.vlev.emplace_back(q.vars.size(), -1);
        env.elev.emplace_back(q.edges.size(), -1);
        const std::vector<int32_t> mclev = clause_levels(q);
        for (size_t c = 0; c < q.clauses.size(); ++c) {
            const Clause& cl = q.clauses[c];
            if (cl.kind != ClauseKind::MATCH && cl.kind != ClauseKind::OPTIONAL_MATCH) continue;
            if (c >= mclev.size()) continue;
            const int32_t lv = mclev[c];
            if (lv < 0) continue;
            for (int32_t v : cl.vars) {
                if (env.vlev.back()[(size_t)v] < 0 || lv < env.vlev.back()[(size_t)v])
                    env.vlev.back()[(size_t)v] = lv;
            }
        }
        for (size_t v = 0; v < q.vars.size(); ++v) {
            if (q.vars[v].kind != PatternVar::VERTEX || pl.vmap[v] < 0) continue;
            int32_t lv = sk.pos[(size_t)pl.vmap[v]].exec_level;
            if (lv < 0) {
                lv = env.vlev.back()[v];
                if (lv < 0) lv = INT32_MAX;
                for (size_t e = 0; e < q.edges.size(); ++e) {
                    if (pl.emap[e] < 0) continue;
                    if (q.edges[e].from == (int32_t)v || q.edges[e].to == (int32_t)v)
                        lv = std::min(lv, exlev(sk.edge[(size_t)pl.emap[e]]));
                }
                if (lv == INT32_MAX) lv = 0;
            }
            env.vlev.back()[v] = lv;
            env.levels[(size_t)lv].present[m][(size_t)pl.vmap[v]] = 1;
        }
        for (size_t e = 0; e < q.edges.size(); ++e) {
            if (pl.emap[e] < 0) continue;
            const PatternEdge& pe = q.edges[e];
            const SkeletonEdge& X = sk.edge[(size_t)pl.emap[e]];
            const int32_t lv = exlev(X);
            env.elev.back()[e] = lv;
            Level& L = env.levels[(size_t)lv];
            const int32_t se = pl.emap[e];
            L.types[m][(size_t)se] = norm(pe.types);
            L.dir[m][(size_t)se] = (pl.vmap[(size_t)pe.from] == X.u) ? 1 : 0;
            L.any_dir[m][(size_t)se] = pe.any_dir ? 1 : 0;
            L.path_dir[m][(size_t)se] = (pl.vmap[(size_t)pe.from] == X.u) ? 1 : 0;
            L.hop_lo[m][(size_t)se] = pe.lo;
            L.hop_hi[m][(size_t)se] = pe.hi;
            if (q.clauses[(size_t)pe.clause].kind == ClauseKind::OPTIONAL_MATCH) {
                L.opt[m] = 1;
                L.opt_index[m] = (int32_t)pe.clause;
            }
        }
    }
    // level edges: the skeleton edges introduced at a level, with the members' union type list
    for (int32_t lv = 0; lv <= maxl; ++lv) {
        Level& L = env.levels[(size_t)lv];
        for (size_t se = 0; se < NE; ++se) {
            if (exlev(sk.edge[se]) != lv) continue;
            LevelEdge le;
            le.u = sk.edge[se].u;
            le.w = sk.edge[se].w;
            std::vector<std::string> all;
            bool any = false;
            int32_t lo = INT32_MAX, hi = 1;
            bool any_dir = false;
            for (size_t m = 0; m < NM; ++m) {
                if (L.types[m][se].empty()) continue;
                any = true;
                for (const auto& t : L.types[m][se])
                    if (std::find(all.begin(), all.end(), t) == all.end()) all.push_back(t);
                lo = std::min(lo, L.hop_lo[m][se]);
                hi = (hi < 0 || L.hop_hi[m][se] < 0) ? -1 : std::max(hi, L.hop_hi[m][se]);
                any_dir = any_dir || L.any_dir[m][se];
            }
            if (!any) continue;
            le.types = norm(all);
            // An edge whose two endpoints were bound by an earlier level is a close test at
            // this level, even when the seed's skeleton marked it as a tree edge.  Walking it
            // as a tree would overwrite an already-bound endpoint: in IS7, the OPTIONAL
            // (cp)-[:KNOWS]->(mp) reused the mandatory MATCH's cp and mp, and the walk
            // rebound cp to every KNOWS neighbour of mp (411 rows for one core row).
            const bool both_bound_before =
                lv > 0 && env.pos_lev[(size_t)le.u] < lv && env.pos_lev[(size_t)le.w] < lv;
            le.tree = sk.edge[se].tree && !both_bound_before;
            le.lo = lo == INT32_MAX ? 1 : lo;
            le.hi = hi;
            le.any_dir = any_dir;
            L.edges.push_back(le);
            L.level_skel.push_back((int32_t)se);
        }
    }
    if (std::getenv("V3_DEBUG_ALIGN")) {
        std::fprintf(stderr, "build_env pos %zu edges %zu levels %zu members:", NP, NE, env.levels.size());
        for (size_t p = 0; p < NP; ++p) std::fprintf(stderr, " %s@%d", sk.pos[p].name.c_str(), env.pos_lev[p]);
        std::fprintf(stderr, "\n");
        for (size_t e = 0; e < NE; ++e)
            std::fprintf(stderr, "  edge %zu %d->%d level %d exec %d core %d types", e, sk.edge[e].u,
                         sk.edge[e].w, sk.edge[e].level, sk.edge[e].exec_level, (int)sk.edge[e].core);
        std::fprintf(stderr, "\n");
        for (size_t m = 0; m < NM; ++m) {
            std::fprintf(stderr, "  member %zu vlev:", m);
            for (size_t v = 0; v < env.vlev[m].size(); ++v)
                std::fprintf(stderr, " %s=%d", r.queries[keep[m].cand].vars[v].name.c_str(), env.vlev[m][v]);
            std::fprintf(stderr, "\n");
        }
        for (size_t l = 0; l < env.levels.size(); ++l) {
            std::fprintf(stderr, "  level %zu edges %zu:", l, env.levels[l].edges.size());
            for (size_t e = 0; e < env.levels[l].edges.size(); ++e)
                std::fprintf(stderr, " se%d", env.levels[l].level_skel[e]);
            std::fprintf(stderr, "\n");
        }
        for (size_t m = 0; m < keep.size(); ++m) {
            std::fprintf(stderr, "  member %zu ci %d emap:", m, keep[m].cand);
            for (int32_t se : keep[m].emap) std::fprintf(stderr, " %d", se);
            std::fprintf(stderr, " types0:");
            for (size_t se = 0; se < env.levels[0].types[m].size(); ++se) {
                std::fprintf(stderr, " [%zu]", se);
                for (const auto& t : env.levels[0].types[m][se]) std::fprintf(stderr, "%s,", t.c_str());
            }
            std::fprintf(stderr, "\n");
        }
    }
    env.members.clear();
    env.members.reserve(NM);
    env.depart_clause.clear();
    env.depart_clause.reserve(NM);
    for (const auto& pl : keep) env.depart_clause.push_back(pl.depart_clause);
    for (const auto& pl : keep) env.members.push_back(pl.cand);
    for (const auto& pl : keep) {
        const Query& q = r.queries[pl.cand];
        std::vector<int32_t> vm;
        vm.push_back((int32_t)pl.vnames.size());
        for (const auto& name : pl.vnames) {
            const int32_t qv = q.var_index(name);
            const int32_t pos = qv >= 0 && q.vars[(size_t)qv].kind == PatternVar::VERTEX ? pl.vmap[(size_t)qv] : -1;
            vm.push_back(pos);
        }
        env.vmap.push_back(vm);
        env.emap.push_back(pl.emap);
    }
    if (prefix_end >= 0)
        plan.prefix_envelopes.push_back(std::move(env));
    else
        plan.envelopes.push_back(std::move(env));
}

}  // namespace v3
