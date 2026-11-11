// ECHO v3 internal plan format (CQ+), produced by scripts/v3_build_rounds.py from the frozen
// candidate pool and consumed by the executor.
//
//   {id, family, template, text, clauses[], variables[], edges[], where[], items[], order_by,
//    skip, limit}
//
// clauses   MATCH / OPTIONAL_MATCH / WITH (grouping: has items + aggregates) / WITH (plain:
//           carried projection) / RETURN, in text order; every clause lists the pattern edges it
//           writes (`edges`) and the atoms that become evaluable at its end (`where`)
// where     conjunction atoms: var.prop op const | var.prop op var2.prop | IS [NOT] NULL | IN,
//           optionally negated; atoms that cannot be decided outside a WHERE (e.g. the join
//           inequality `friend <> p`) are marked `post: true` and filter the MATCHING before the
//           first WITH / RETURN (see v3_exec).
// items     KEY (var, prop), AGG (func, DISTINCT, arg), or CONST
#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>

namespace v3 {

constexpr uint32_t kNil = 0xFFFFFFFFu;   // absent vertex / relationship
constexpr int64_t kNull = INT64_MIN + 1; // NULL of every property column

enum class PropType : uint8_t { INT = 0, STR = 1 };

// One data node; the enumeration order is (NULL, numeric, string bytes).
struct Value {
    int64_t i = 0;
    std::string s;
    bool is_str = false;
    bool is_null = false;
    bool operator==(const Value& o) const {
        return is_null == o.is_null && is_str == o.is_str && i == o.i && s == o.s;
    }
    static Value str(std::string x) {
        Value v;
        v.s = std::move(x);
        v.is_str = true;
        return v;
    }
    static Value num(int64_t x) {
        Value v;
        v.i = x;
        return v;
    }
    static Value nul() {
        Value v;
        v.is_null = true;
        return v;
    }
    bool operator<(const Value& o) const {
        if (is_null != o.is_null) return is_null;
        if (is_str != o.is_str) return !is_str;
        if (i != o.i) return i < o.i;
        return s < o.s;
    }
    uint64_t hash() const {
        uint64_t h = 1469598103934665603ULL;
        h = (h ^ (uint64_t)((is_str ? 2u : 0u) | (is_null ? 1u : 0u))) * 1099511628211ULL;
        const char* p = reinterpret_cast<const char*>(&i);
        for (size_t k = 0; k < sizeof i; ++k) h = (h ^ (unsigned char)p[k]) * 1099511628211ULL;
        for (unsigned char ch : s) h = (h ^ ch) * 1099511628211ULL;
        return h;
    }
};

// Scalar projection expression: the non-aggregate part of a RETURN / WITH item that is more
// than a variable or a property (CASE / coalesce).  It is evaluated on a row after the match and
// therefore adds no pattern work.
struct Expr {
    enum Kind : uint8_t { CONST = 0, PROP = 1, VAR = 2, CMP = 3, AND = 4, OR = 5, NOT = 6,
                          IS_NULL = 7, LABEL = 8, CASE = 9, COALESCE = 10, FUNC = 11,
                          ARITH = 12, COL = 13 };
    Kind kind = CONST;
    Value c;
    int32_t var = -1;
    int32_t col = -1;        // COL: a WITH column read by the expression
    std::string prop;
    std::string op;          // CMP / FUNC
    std::vector<Expr> args;
    std::vector<std::string> labels;
};

struct Row {
    std::vector<Value> v;
    bool operator==(const Row& o) const {
        if (v.size() != o.v.size()) return false;
        for (size_t k = 0; k < v.size(); ++k)
            if (!(v[k] == o.v[k])) return false;
        return true;
    }
};
struct RowHash {
    size_t operator()(const Row& r) const {
        uint64_t h = 1469598103934665603ULL;
        for (const auto& x : r.v) h = (h ^ x.hash()) * 1099511628211ULL;
        return (size_t)h;
    }
};

// ---------------------------------------------------------------- schema (sidecar meta.json)
struct PropDef {
    std::string name;
    PropType type = PropType::INT;
    uint32_t col = 0;
};

struct LabelDef {
    std::string name;
    uint32_t nprops = 0;
    std::unordered_map<std::string, uint32_t> prop_col;
};

struct TypeDef {
    std::string name;
    uint32_t nprops = 0;
    std::unordered_map<std::string, uint32_t> prop_col;
};

struct Schema {
    std::vector<LabelDef> labels;
    std::vector<TypeDef> types;
    std::map<std::string, uint32_t> label_id, type_id;
    int32_t label_of(const std::string& s) const {
        auto it = label_id.find(s);
        return it == label_id.end() ? -1 : (int32_t)it->second;
    }
    int32_t type_of(const std::string& s) const {
        auto it = type_id.find(s);
        return it == type_id.end() ? -1 : (int32_t)it->second;
    }
};

// ---------------------------------------------------------------- plan IR
enum class ClauseKind : uint8_t { MATCH, OPTIONAL_MATCH, WITH, RETURN };

// Matching semantics of one candidate.  They differ in which duplicates a MATCH may produce:
//   CYPHER: relationships are distinct inside one MATCH, vertices may repeat (Cypher);
//   ISO:    vertices are injective inside one MATCH (subgraph isomorphism, the MASC home);
//   SIMPLE: the vertices of a variable-length path are distinct (BatchEnum's simple path);
//   WALK:   relationships and vertices may repeat (VLPQ's walk).
// Relationship identity is the logical relationship, not the directed adjacency row: the store
// has one row per direction, and the two rows of one undirected relationship are the same edge.
enum class Semantics : uint8_t { CYPHER = 0, ISO = 1, SIMPLE = 2, WALK = 3 };

enum class AggFunc : uint8_t { COUNT_STAR, COUNT_VAR, COUNT, SUM, MIN, MAX, COLLECT };

struct ProjectionItem {
    enum Kind : uint8_t { KEY = 0, AGG = 1, CONST = 2, COL = 3, EXPR = 4 } kind = KEY;
    int32_t var = -1;
    std::string prop;
    AggFunc func = AggFunc::COUNT_STAR;
    bool distinct = false;
    int32_t arg_var = -1;
    std::string arg_prop;
    bool arg_is_prop = false;
    // A WITH projects its non-aggregated expressions (a property of a carried variable) and its
    // aggregates into columns; a later WITH / RETURN reads them as `COL` items, and an aggregate
    // may take a column as its argument (`count(cnt)` after `WITH f, count(f) AS cnt`).
    int32_t col = -1;       // COL: the WITH column read; AGG: the column argument
    int32_t arg_col = -1;
    int32_t expr = -1;      // EXPR: index into Query::exprs
    std::string label;   // result column name (an alias or the projection text)
};

struct Clause {
    ClauseKind kind = ClauseKind::MATCH;
    std::vector<int32_t> edges;     // pattern edges written by this clause
    std::vector<int32_t> vars;      // vertex variables written by this clause (even without an edge)
    std::vector<int32_t> where;     // atoms evaluated at the end of this clause
    std::vector<int32_t> pre_where; // post atoms: filter the matching before WITH / RETURN
    std::vector<int32_t> items;     // projection items (WITH / RETURN)
    bool distinct = false;
    std::vector<int32_t> carried;   // WITH: pattern variables carried forward
    std::vector<int32_t> out_vars;  // WITH: columns of the carried (non-aggregated) result
    std::vector<int32_t> out_items; // WITH: aggregate columns of the carried result
    std::vector<int32_t> order_items;   // ORDER BY: sort keys as result columns
    std::vector<int> order_desc;        // ORDER BY: 1 = descending, per key
    int64_t limit_n = -1, skip_n = 0;   // SKIP / LIMIT values
};

struct PatternVar {
    enum Kind : uint8_t { VERTEX = 0, EDGE = 1 } kind = VERTEX;
    std::string name;
    std::vector<std::string> labels;
    std::vector<std::pair<std::string, Value>> props;
};

struct Comparison {
    enum Kind : uint8_t { CONST = 0, VAR = 1 } rhs = CONST;
    std::string op;
    int32_t var = -1;
    std::vector<Value> consts;
};

struct Atom {
    int32_t var = -1;
    int32_t col = -1;   // a comparison over a WITH column (var is -1 then)
    std::string prop;
    std::string rhs_prop;   // for a var.prop op var2.prop comparison
    Comparison cmp;
    bool negated = false;  // the atom is a term of a NOT over this comparison
    int32_t clause = -1;
    bool post = false;     // filter the matching (not a step of the join)
    int32_t or_group = -1; // atoms of one OR are one disjunct group (decided together)
    // A comparison whose two sides bind at different steps: `checks[0]` is the operator applied
    // to the left side's value against a constant, `checks[1]` the test of the right side against
    // the kept left value ("" = no test at that step).  Empty: both sides known at one step.
    std::vector<std::string> checks;
    std::string keep;      // value kept for the second step (empty: none)
};

struct ScopedAtom {          // `x IN relationships(p)` / `x IN nodes(p)`: the pattern edges of p
    bool nodes = false;      // true: the vertices of the path (joined by its edges); false: edges
    int32_t var = -1;
};

struct PatternEdge {
    int32_t from = -1, to = -1;
    std::vector<std::string> types;
    bool any_dir = false;
    int32_t lo = 1, hi = 1;     // -1: unbounded
    std::string var_name;
    std::vector<std::pair<std::string, Value>> props;
    int32_t clause = -1;
    std::string path;           // enclosing named path ("" when the edge is not in one)
    bool path_nodes = false;    // `p = (a)-[*]->(b)`: nodes(p) = the vertices, edges(p) = the edges
    bool simple = false;        // SIMPLE semantics: no repeated vertex on this variable-length edge
    bool walk = false;          // WALK semantics: relationships may repeat
    std::vector<ScopedAtom> scoped;   // constraints on an edge / vertex that the path attaches
};

struct Query {
    std::string id, family, template_name, text;
    // Canonical form computed once when the round is compiled (outside every timed cell).
    // It is query text normalisation, not data-dependent planning; the point executor uses it
    // to replay identical candidates without rebuilding the general canonical string.
    std::string canon;
    std::vector<Clause> clauses;
    std::vector<PatternVar> vars;
    std::vector<PatternEdge> edges;
    std::vector<Atom> atoms;
    std::vector<ProjectionItem> items;
    std::vector<Expr> exprs;
    Semantics sem = Semantics::CYPHER;
    int32_t order_by = -1;        // clause index carrying ORDER BY (results only)
    int32_t skip = -1, limit = -1;
    int32_t return_clause = -1;
    int32_t first_match = -1;
    int32_t var_index(const std::string& n) const;
    bool is_plain() const;
};

// Static shape of one point candidate: a constant-anchored single vertex or one fixed typed
// hop.  It is compiled from the query IR before the timed cells, like the parser output, and
// then reused by the shared point executor for every repeat.
struct PointQuery {
    bool supported = false;
    bool edge = false;
    int root = -1;
    int other = -1;
    int edge_var = -1;
    int type = -1;
    bool any_dir = false;
    bool root_from = true;
    std::vector<int> root_labels;
    std::vector<int> other_labels;
    int64_t root_id = 0;
    bool other_has_id = false;
    int64_t other_id = 0;
    bool direct_project = false;
};

struct Round {
    std::string family, template_name;
    std::vector<Query> queries;
    std::vector<std::string> labels;     // result column names (from the round file)
    // Filled by prepare_tiny_round() outside the timed cell.  A round whose whole estimated
    // work is below the measured planning + C_S cost runs one candidate at a time and skips
    // alignment / partitioning / C_S; identical candidates still replay canonically.
    bool tiny = false;
    double tiny_est = 0.0;
    std::vector<int32_t> tiny_uniq;
    std::vector<int32_t> tiny_replay;
    // Compiled point plan (outside the timed cells): per-candidate shape, deduplicated
    // candidates, and the canonical replay map.  Empty/unsupported rounds use the general path.
    bool point_ready = false;
    std::vector<PointQuery> point;
    std::vector<int32_t> point_uniq;
    std::vector<int32_t> point_replay;
};

// Query -> round file JSON (the inverse of parse_query; used by tests and the reference rows).
std::string query_to_json(const Query& q);

}  // namespace v3
