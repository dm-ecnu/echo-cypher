#include "v3_plan.h"

#include <fstream>
#include <sstream>

namespace v3 {

Value parse_value(const json::J& j) {
    Value v;
    if (j.t == json::J::NUM) {
        v.i = j.is_int ? j.i : (long long)j.num;
    } else if (j.t == json::J::STR) {
        v.s = j.s;
        v.is_str = true;
    } else if (j.t == json::J::NUL) {
        v.is_null = true;
    } else if (j.t == json::J::BOOL) {
        v.i = j.b ? 1 : 0;
    }
    return v;
}

// Property maps appear as an object {name: value} (the plan writer) or as a list of [name,
// value] pairs (the builder); both carry the pattern's constant map.
static void parse_props(const json::J* ps, std::vector<std::pair<std::string, Value>>* out) {
    if (!ps) return;
    if (ps->t == json::J::OBJ) {
        for (const auto& p : ps->o) out->emplace_back(p.first, parse_value(p.second));
    } else if (ps->t == json::J::ARR) {
        for (const auto& p : ps->a) {
            if (p.t != json::J::ARR || p.a.size() < 2 || p.a[0].t != json::J::STR) continue;
            out->emplace_back(p.a[0].s, parse_value(p.a[1]));
        }
    }
}

namespace {
int32_t var_by_name(const std::vector<std::string>& names, const std::string& n);

Expr parse_expr(const json::J& j, const std::vector<std::string>& names, int depth = 0) {
    Expr e;
    if (depth > 32) return e;
    const std::string k = j.str("k", "const");
    if (k == "const") {
        e.kind = Expr::CONST;
        if (const json::J* v = j.get("v")) e.c = parse_value(*v);
        else e.c = Value::nul();
        return e;
    }
    if (k == "prop") {
        e.kind = Expr::PROP;
        e.var = var_by_name(names, j.str("var"));
        e.prop = j.str("prop");
        return e;
    }
    if (k == "col") {
        e.kind = Expr::COL;
        e.col = (int32_t)j.intv("col", -1);
        return e;
    }
    if (k == "var") {
        e.kind = Expr::VAR;
        e.var = var_by_name(names, j.str("var"));
        return e;
    }
    if (k == "cmp") {
        e.kind = Expr::CMP;
        e.op = j.str("op", "=");
        if (const json::J* a = j.get("a")) e.args.push_back(parse_expr(*a, names, depth + 1));
        if (const json::J* b = j.get("b")) e.args.push_back(parse_expr(*b, names, depth + 1));
        return e;
    }
    if (k == "and" || k == "or" || k == "coalesce") {
        e.kind = k == "and" ? Expr::AND : k == "or" ? Expr::OR : Expr::COALESCE;
        if (const json::J* a = j.get("a"))
            for (const auto& x : a->a) e.args.push_back(parse_expr(x, names, depth + 1));
        return e;
    }
    if (k == "not" || k == "isnull") {
        e.kind = k == "not" ? Expr::NOT : Expr::IS_NULL;
        if (const json::J* a = j.get("a")) e.args.push_back(parse_expr(*a, names, depth + 1));
        return e;
    }
    if (k == "label") {
        e.kind = Expr::LABEL;
        e.var = var_by_name(names, j.str("var"));
        if (const json::J* a = j.get("labels"))
            for (const auto& x : a->a) e.labels.push_back(x.s);
        return e;
    }
    if (k == "case") {
        e.kind = Expr::CASE;
        if (const json::J* a = j.get("a"))
            for (const auto& x : a->a) e.args.push_back(parse_expr(x, names, depth + 1));
        return e;
    }
    if (k == "arith") {
        e.kind = Expr::ARITH;
        e.op = j.str("op", "+");
        if (const json::J* a = j.get("a"))
            for (const auto& x : a->a) e.args.push_back(parse_expr(x, names, depth + 1));
        return e;
    }
    if (k == "func") {
        e.kind = Expr::FUNC;
        e.op = j.str("op");
        if (const json::J* a = j.get("a"))
            for (const auto& x : a->a) e.args.push_back(parse_expr(x, names, depth + 1));
        return e;
    }
    return e;
}
}  // namespace

int32_t Query::var_index(const std::string& n) const {
    for (size_t k = 0; k < vars.size(); ++k)
        if (vars[k].name == n) return (int32_t)k;
    return -1;
}

bool Query::is_plain() const {
    // Fixed patterns only: one MATCH clause, no OPTIONAL / WITH, every edge a single typed hop.
    for (const auto& c : clauses)
        if (c.kind != ClauseKind::MATCH && c.kind != ClauseKind::RETURN) return false;
    for (const auto& e : edges)
        if (e.lo != 1 || e.hi != 1) return false;
    return true;
}

namespace {
int32_t var_by_name(const std::vector<std::string>& names, const std::string& n) {
    for (size_t k = 0; k < names.size(); ++k)
        if (names[k] == n) return (int32_t)k;
    return -1;
}
AggFunc agg_of(const std::string& s) {
    if (s == "count_star") return AggFunc::COUNT_STAR;
    if (s == "count_var") return AggFunc::COUNT_VAR;
    if (s == "count") return AggFunc::COUNT;
    if (s == "sum") return AggFunc::SUM;
    if (s == "min") return AggFunc::MIN;
    if (s == "max") return AggFunc::MAX;
    return AggFunc::COLLECT;
}
}  // namespace

bool parse_query(const json::J& j, Query* q, std::string* err) {
    auto bad = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    q->id = j.str("id");
    q->family = j.str("family");
    q->template_name = j.str("template");
    q->text = j.str("text");
    // variables: name + clause-level index order (the envelope relies on a stable order)
    std::vector<std::string> names;
    if (const json::J* vs = j.get("variables")) {
        for (const auto& v : vs->a) {
            PatternVar pv;
            pv.name = v.str("name");
            const std::string kind = v.str("kind", "vertex");
            pv.kind = kind == "edge" ? PatternVar::EDGE : PatternVar::VERTEX;
            if (const json::J* ls = v.get("labels"))
                for (const auto& l : ls->a) pv.labels.push_back(l.s);
            parse_props(v.get("props"), &pv.props);
            names.push_back(pv.name);
            q->vars.push_back(std::move(pv));
        }
    }
    // edges
    if (const json::J* es = j.get("edges")) {
        for (const auto& e : es->a) {
            PatternEdge pe;
            pe.from = var_by_name(names, e.str("from"));
            pe.to = var_by_name(names, e.str("to"));
            if (const json::J* ts = e.get("types"))
                for (const auto& t : ts->a) pe.types.push_back(t.s);
            const std::string d = e.str("dir", "->");
            pe.any_dir = (d == "both" || d == "-");
            pe.lo = (int32_t)e.intv("lo", 1);
            pe.hi = (int32_t)e.intv("hi", 1);
            pe.var_name = e.str("var");
            parse_props(e.get("props"), &pe.props);
            pe.clause = (int32_t)e.intv("clause", -1);
            pe.path = e.str("path");
            pe.path_nodes = e.boolv("path_nodes", false);
            pe.simple = e.boolv("simple", false);
            pe.walk = e.boolv("walk", false);
            if (const json::J* sc = e.get("scoped")) {
                for (const auto& sa : sc->a) {
                    ScopedAtom x;
                    x.nodes = sa.boolv("nodes", false);
                    x.var = var_by_name(names, sa.str("var"));
                    pe.scoped.push_back(x);
                }
            }
            if (pe.from < 0 || pe.to < 0) return bad("edge endpoint not declared: " + q->id);
            q->edges.push_back(std::move(pe));
        }
    }
    // Per-candidate matching semantics.  The old opponent rounds only carried per-edge simple /
    // walk flags; infer the query semantics from them when no explicit field is present.  The
    // default is Cypher, which is what the LDBC rounds and the serial reference use.
    {
        const std::string s = j.str("semantics", "");
        bool any_simple = false, any_walk = false;
        for (const auto& pe : q->edges) {
            if (pe.simple) any_simple = true;
            if (pe.walk) any_walk = true;
        }
        if (s == "iso" || s == "injective") q->sem = Semantics::ISO;
        else if (s == "simple") q->sem = Semantics::SIMPLE;
        else if (s == "walk") q->sem = Semantics::WALK;
        else if (s == "cypher" || s.empty()) {
            q->sem = any_walk ? Semantics::WALK
                   : any_simple ? Semantics::SIMPLE : Semantics::CYPHER;
        } else {
            return bad("unknown semantics: " + s + " in " + q->id);
        }
        // Normalise the per-edge flags so the two evaluation paths read the same switches.
        for (auto& pe : q->edges) {
            pe.simple = (q->sem == Semantics::SIMPLE) &&
                        (pe.lo != 1 || pe.hi != 1);
            pe.walk = (q->sem == Semantics::WALK);
        }
    }
    // atoms (WHERE conjunctions, placed at the clause where they become evaluable)
    if (const json::J* as = j.get("where")) {
        for (const auto& a : as->a) {
            Atom at;
            at.var = var_by_name(names, a.str("var"));
            at.col = (int32_t)a.intv("col", -1);
            at.prop = a.str("prop");
            const std::string op = a.str("op");
            at.cmp.op = op;
            at.negated = a.boolv("negated", false);
            {
                const json::J* rp = a.get("rhs_prop");
                if (rp) at.rhs_prop = rp->s;
            }
            if (const json::J* rhs = a.get("var_rhs")) {
                at.cmp.rhs = Comparison::VAR;
                at.cmp.var = var_by_name(names, rhs->s);
                if (at.cmp.var < 0) return bad("atom rhs var not declared: " + q->id);
            } else if (const json::J* cs = a.get("consts")) {
                for (const auto& c : cs->a) at.cmp.consts.push_back(parse_value(c));
            } else if (const json::J* c = a.get("const")) {
                at.cmp.consts.push_back(parse_value(*c));
            }
            at.clause = (int32_t)a.intv("clause", -1);
            at.post = a.boolv("post", false);
            at.or_group = (int32_t)a.intv("or", -1);
            if (const json::J* ch = a.get("checks"))
                for (const auto& x : ch->a) at.checks.push_back(x.t == json::J::STR ? x.s : std::string());
            at.keep = a.str("keep");
            if (at.var < 0 && at.col < 0) return bad("atom var not declared: " + q->id);
            q->atoms.push_back(std::move(at));
        }
    }
    // projection items
    if (const json::J* is = j.get("items")) {
        for (const auto& it : is->a) {
            ProjectionItem pi;
            const std::string k = it.str("kind", "key");
            pi.kind = k == "agg" ? ProjectionItem::AGG
                    : k == "const" ? ProjectionItem::CONST
                    : k == "col" ? ProjectionItem::COL
                    : k == "expr" ? ProjectionItem::EXPR : ProjectionItem::KEY;
            pi.var = var_by_name(names, it.str("var"));
            pi.prop = it.str("prop");
            pi.label = it.str("label");
            pi.col = (int32_t)it.intv("col", -1);
            if (pi.kind == ProjectionItem::EXPR) {
                pi.expr = (int32_t)q->exprs.size();
                if (const json::J* e = it.get("expr")) q->exprs.push_back(parse_expr(*e, names));
                else q->exprs.push_back(Expr{});
            }
            if (pi.kind == ProjectionItem::AGG) {
                pi.func = agg_of(it.str("func"));
                pi.distinct = it.boolv("distinct", false);
                pi.arg_is_prop = it.has("arg_prop");
                pi.arg_prop = it.str("arg_prop");
                pi.arg_var = var_by_name(names, it.str("arg_var"));
                pi.arg_col = (int32_t)it.intv("arg_col", -1);
            }
            q->items.push_back(std::move(pi));
        }
    }
    // clauses
    if (const json::J* cs = j.get("clauses")) {
        for (const auto& c : cs->a) {
            Clause cl;
            const std::string k = c.str("kind");
            cl.kind = k == "OPTIONAL_MATCH" ? ClauseKind::OPTIONAL_MATCH
                     : k == "WITH" ? ClauseKind::WITH
                     : k == "RETURN" ? ClauseKind::RETURN : ClauseKind::MATCH;
            if (const json::J* e = c.get("edges"))
                for (const auto& x : e->a) cl.edges.push_back((int32_t)x.i);
            if (const json::J* v = c.get("vars"))
                for (const auto& x : v->a) cl.vars.push_back(var_by_name(names, x.s));
            if (const json::J* it = c.get("items"))
                for (const auto& x : it->a) cl.items.push_back((int32_t)x.i);
            cl.distinct = c.boolv("distinct", false);
            if (const json::J* v = c.get("carried"))
                for (const auto& x : v->a) cl.carried.push_back(var_by_name(names, x.s));
            if (const json::J* v = c.get("out_vars"))
                for (const auto& x : v->a) cl.out_vars.push_back(var_by_name(names, x.s));
            if (const json::J* v = c.get("out_items"))
                for (const auto& x : v->a) cl.out_items.push_back((int32_t)x.i);
            if (const json::J* v = c.get("where"))
                for (const auto& x : v->a) cl.where.push_back((int32_t)x.i);
            if (const json::J* v = c.get("pre_where"))
                for (const auto& x : v->a) cl.pre_where.push_back((int32_t)x.i);
            if (k == "ORDER_BY") {
                if (const json::J* v = c.get("keys"))
                    for (const auto& x : v->a) cl.order_items.push_back((int32_t)x.i);
                if (const json::J* v = c.get("desc"))
                    for (const auto& x : v->a) cl.order_desc.push_back((int)(x.t == json::J::BOOL ? x.b : x.i != 0));
            }
            if (k == "SKIP") {
                q->skip = (int32_t)q->clauses.size();
                cl.skip_n = c.intv("n", 0);
            }
            if (k == "LIMIT") {
                q->limit = (int32_t)q->clauses.size();
                cl.limit_n = c.intv("n", -1);
            }
            if (cl.kind == ClauseKind::MATCH && q->first_match < 0) q->first_match = (int32_t)q->clauses.size();
            if (cl.kind == ClauseKind::RETURN) q->return_clause = (int32_t)q->clauses.size();
            if (k == "ORDER_BY") q->order_by = (int32_t)q->clauses.size();
            q->clauses.push_back(std::move(cl));
        }
    }
    return true;
}

bool load_query_file(const std::string& path, Query* out, std::string* err) {
    std::ifstream in(path);
    if (!in) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    json::J j;
    if (!json::parse(ss.str(), &j, err)) return false;
    return parse_query(j, out, err);
}

std::string query_to_json(const Query& q) {
    using W = json::Writer;
    std::string o = "{\"id\":" + W::quote(q.id) + ",\"family\":" + W::quote(q.family) +
                    ",\"template\":" + W::quote(q.template_name) + ",\"text\":" + W::quote(q.text) +
                    ",\"semantics\":" + W::quote(q.sem == Semantics::ISO ? "iso"
                                            : q.sem == Semantics::SIMPLE ? "simple"
                                            : q.sem == Semantics::WALK ? "walk" : "cypher") +
                    ",\"clauses\":[";
    auto name_of = [&](int32_t v) { return v >= 0 && v < (int32_t)q.vars.size() ? q.vars[v].name : std::string(); };
    for (size_t c = 0; c < q.clauses.size(); ++c) {
        const Clause& cl = q.clauses[c];
        if (c) o += ",";
        const char* kind = cl.kind == ClauseKind::MATCH ? "MATCH"
                         : cl.kind == ClauseKind::OPTIONAL_MATCH ? "OPTIONAL_MATCH"
                         : cl.kind == ClauseKind::WITH ? "WITH" : "RETURN";
        o += std::string("{\"kind\":\"") + kind + "\",\"edges\":[";
        for (size_t e = 0; e < cl.edges.size(); ++e) o += (e ? "," : "") + std::to_string(cl.edges[e]);
        o += "],\"vars\":[";
        for (size_t k = 0; k < cl.vars.size(); ++k) o += (k ? "," : "") + W::quote(name_of(cl.vars[k]));
        o += "],\"where\":[";
        for (size_t a = 0; a < cl.where.size(); ++a) o += (a ? "," : "") + std::to_string(cl.where[a]);
        o += "],\"items\":[";
        for (size_t it = 0; it < cl.items.size(); ++it) o += (it ? "," : "") + std::to_string(cl.items[it]);
        o += "],\"distinct\":" + std::string(cl.distinct ? "true" : "false");
        if (cl.kind == ClauseKind::WITH) {
            o += ",\"carried\":[";
            for (size_t k = 0; k < cl.carried.size(); ++k) o += (k ? "," : "") + W::quote(name_of(cl.carried[k]));
            o += "],\"out_items\":[";
            for (size_t k = 0; k < cl.out_items.size(); ++k) o += (k ? "," : "") + std::to_string(cl.out_items[k]);
            o += "],\"out_vars\":[";
            for (size_t k = 0; k < cl.out_vars.size(); ++k) o += (k ? "," : "") + W::quote(name_of(cl.out_vars[k]));
            o += "]";
        }
        if (!cl.order_items.empty()) {
            o += ",\"keys\":[";
            for (size_t k = 0; k < cl.order_items.size(); ++k) o += (k ? "," : "") + std::to_string(cl.order_items[k]);
            o += "],\"desc\":[";
            for (size_t k = 0; k < cl.order_desc.size(); ++k) o += (k ? "," : "") + std::string(cl.order_desc[k] ? "true" : "false");
            o += "]";
        }
        if (cl.skip_n != 0 || q.skip == (int32_t)c) o += ",\"n\":" + std::to_string(cl.skip_n);
        if (cl.limit_n != -1) o += ",\"n\":" + std::to_string(cl.limit_n);
        o += "}";
    }
    o += "],\"variables\":[";
    for (size_t v = 0; v < q.vars.size(); ++v) {
        const PatternVar& pv = q.vars[v];
        if (v) o += ",";
        o += "{\"name\":" + W::quote(pv.name) + ",\"kind\":" + W::quote(pv.kind == PatternVar::EDGE ? "edge" : "vertex") +
             ",\"labels\":[";
        for (size_t l = 0; l < pv.labels.size(); ++l) o += (l ? "," : "") + W::quote(pv.labels[l]);
        o += "],\"props\":{";
        for (size_t p2 = 0; p2 < pv.props.size(); ++p2) {
            const Value& x = pv.props[p2].second;
            o += (p2 ? "," : "") + W::quote(pv.props[p2].first) + ":";
            if (x.is_null) o += "null";
            else if (x.is_str) o += W::quote(x.s);
            else o += std::to_string(x.i);
        }
        o += "}}";
    }
    o += "],\"edges\":[";
    for (size_t e = 0; e < q.edges.size(); ++e) {
        const PatternEdge& pe = q.edges[e];
        if (e) o += ",";
        o += "{\"from\":" + W::quote(name_of(pe.from)) + ",\"to\":" + W::quote(name_of(pe.to)) + ",\"types\":[";
        for (size_t t = 0; t < pe.types.size(); ++t) o += (t ? "," : "") + W::quote(pe.types[t]);
        o += "],\"dir\":" + W::quote(pe.any_dir ? "both" : "->") + ",\"lo\":" + std::to_string(pe.lo) +
             ",\"hi\":" + std::to_string(pe.hi) + ",\"var\":" + W::quote(pe.var_name) +
             ",\"simple\":" + (pe.simple ? "true" : "false") +
             ",\"walk\":" + (pe.walk ? "true" : "false") + ",\"props\":{";
        for (size_t p2 = 0; p2 < pe.props.size(); ++p2) {
            const Value& x = pe.props[p2].second;
            o += (p2 ? "," : "") + W::quote(pe.props[p2].first) + ":";
            if (x.is_str) o += W::quote(x.s);
            else o += std::to_string(x.i);
        }
        o += "},\"clause\":" + std::to_string(pe.clause) + "}";
    }
    o += "],\"where\":[";
    for (size_t a = 0; a < q.atoms.size(); ++a) {
        const Atom& at = q.atoms[a];
        if (a) o += ",";
        o += "{\"var\":" + W::quote(name_of(at.var)) + ",\"prop\":" + W::quote(at.prop) + ",\"op\":" + W::quote(at.cmp.op) + ",\"consts\":[";
        for (size_t k = 0; k < at.cmp.consts.size(); ++k) {
            const Value& x = at.cmp.consts[k];
            if (k) o += ",";
            if (x.is_str) o += W::quote(x.s);
            else if (x.is_null) o += "null";
            else o += std::to_string(x.i);
        }
        o += "]";
        if (at.col >= 0) o += ",\"col\":" + std::to_string(at.col);
        if (at.cmp.rhs == Comparison::VAR) o += ",\"var_rhs\":" + W::quote(name_of(at.cmp.var));
        if (!at.rhs_prop.empty()) o += ",\"rhs_prop\":" + W::quote(at.rhs_prop);
        o += ",\"clause\":" + std::to_string(at.clause) + ",\"or\":" + std::to_string(at.or_group);
        if (at.negated) o += ",\"negated\":true";
        if (at.post) o += ",\"post\":true";
        o += "}";
    }
    o += "],\"items\":[";
    static const char* fn[] = {"count_star", "count_var", "count", "sum", "min", "max", "collect"};
    for (size_t it = 0; it < q.items.size(); ++it) {
        const ProjectionItem& pi = q.items[it];
        if (it) o += ",";
        const char* kind = pi.kind == ProjectionItem::AGG ? "agg"
                         : pi.kind == ProjectionItem::CONST ? "const"
                         : pi.kind == ProjectionItem::COL ? "col" : "key";
        o += std::string("{\"kind\":\"") + kind + "\",\"var\":" + W::quote(name_of(pi.var)) + ",\"prop\":" + W::quote(pi.prop) +
             ",\"label\":" + W::quote(pi.label);
        if (pi.col >= 0) o += ",\"col\":" + std::to_string(pi.col);
        if (pi.kind == ProjectionItem::AGG) {
            o += std::string(",\"func\":\"") + fn[(int)pi.func] + "\",\"distinct\":" + (pi.distinct ? "true" : "false") +
                 ",\"arg_var\":" + W::quote(name_of(pi.arg_var)) + ",\"arg_prop\":" + W::quote(pi.arg_prop);
            if (pi.arg_col >= 0) o += ",\"arg_col\":" + std::to_string(pi.arg_col);
        }
        o += "}";
    }
    o += "]}";
    return o;
}

bool load_round(const std::string& path, Round* out, std::string* err) {
    std::ifstream in(path);
    if (!in) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    json::J j;
    if (!json::parse(ss.str(), &j, err)) return false;
    out->family = j.str("family");
    out->template_name = j.str("template");
    if (const json::J* ls = j.get("labels"))
        for (const auto& x : ls->a) out->labels.push_back(x.s);
    const json::J* qs = j.get("queries");
    if (!qs) {
        if (err) *err = "round without queries";
        return false;
    }
    for (const auto& q : qs->a) {
        Query qq;
        if (!parse_query(q, &qq, err)) return false;
        out->queries.push_back(std::move(qq));
    }
    return true;
}

}  // namespace v3
