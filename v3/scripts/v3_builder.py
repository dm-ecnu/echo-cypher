#!/usr/bin/env python3
"""Build ECHO v3 plans (rounds) from the frozen LDBC candidate pool.

A pool candidate is compiled into the v3 plan format (../include/v3_ir.h) from its text alone;
the compiler refuses (with a reason) every text whose parts are not all representable in the
plan, so a refusal is a coverage fact and never a silent approximation.

  --pool    cqplus.jsonl of the v3 CQ+ classifier (record per pool position, text included)
  --labels  the v2 export manifest (<graph>.labels.json): stored labels, relationship types
  --out     directory for plan-<family>.json rounds and coverage.json

Plan-JSON contract of one query (read by src/v3_plan.cpp):
  variables  [{name, kind: vertex|edge, labels[], props{prop: value}}]
             a relationship variable (kind edge) carries the edge's types/direction/range;
             it is bound when that edge's hop is taken (a variable-length edge per path)
  edges      [{from, to, types[], dir: "->"|"both", lo, hi, props{}, clause}]   hi = -1: no bound
  where      [{var, prop, op, consts[] | var_rhs [+ rhs_prop], clause, or, post}]
             op: = <> < > <= >=  IS NULL  IS NOT NULL  IN
             `prop` "" and no consts: the variable against a constant list (`prop` "id" is
             written for a bare entity / variable constant comparison in the pool)
             post: a filter of the MATCHING (e.g. `friend <> p`), applied before RETURN
  items      [{kind: key|agg|const, var, prop, func, distinct, arg_var, arg_prop, label}]
  clauses    [{kind: MATCH|OPTIONAL_MATCH|WITH|RETURN, edges[], where[], items[], distinct,
               carried[], out_vars[], out_items[], pre_where[]}, {kind: ORDER_BY, keys[], desc[]},
              {kind: SKIP, n}, {kind: LIMIT, n}]
             a WITH carries, per item, a projection over the rows of the stage: its carried
             variables (kept, listed in carried/out_vars) and its aggregates (out_items);
             a following MATCH joins the carried variables.  `pre_where` on RETURN filters the
             rows before RETURN.
  order_by / skip / limit are read from those trailing clauses.
"""
import os
ECHO_ROOT = os.environ.get("ECHO_ROOT", ".")
import argparse
import collections
import json
import os
import re
import sys

POOL_DIR = os.environ.get('V3_POOL_DIR', (ECHO_ROOT + '/ldbc'))

TOKEN = re.compile(r"""
    (?P<ws>\s+)
  | (?P<str>'(?:\\.|[^'\\])*'|"(?:\\.|[^"\\])*")
  | (?P<num>-?\d+(?:\.\d+)?)
  | (?P<word>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<op><>|<=|>=|=~|\.\.|[-+*/^=()\[\]{}:.,|<>])
""", re.X)
CLAUSE_HEADS = {'MATCH', 'OPTIONAL', 'WITH', 'RETURN', 'UNWIND', 'CALL', 'UNION', 'CREATE',
                'MERGE', 'SET', 'DELETE', 'DETACH', 'REMOVE', 'FOREACH', 'LOAD', 'USE'}
AGGS = {'count', 'sum', 'min', 'max', 'collect'}
COMPARE = ('=', '<>', '<', '>', '<=', '>=')
EDGE_COLS = {'workFrom', 'joinDate', 'creationDate', 'id'}


class Refuse(Exception):
    pass


def lex(text):
    out, i = [], 0
    while i < len(text):
        m = TOKEN.match(text, i)
        if not m:
            raise Refuse('lex@%d' % i)
        i = m.end()
        if m.lastgroup == 'ws':
            continue
        out.append([m.lastgroup, m.group(m.lastgroup)])
    return out


def close_of(toks, i, o='(', c=')'):
    depth = 0
    for j in range(i, len(toks)):
        if toks[j][1] == o:
            depth += 1
        elif toks[j][1] == c:
            depth -= 1
            if depth == 0:
                return j
    raise Refuse('unbalanced')


def depth_split(toks, sep=','):
    """Split at top-level occurrences of `sep` (brackets of every kind counted)."""
    out, cur, depth = [], [], 0
    for t in toks:
        if t[1] in ('(', '[', '{'):
            depth += 1
            cur.append(t)
            continue
        if t[1] in (')', ']', '}'):
            depth -= 1
            cur.append(t)
            continue
        if depth == 0 and t[1] == sep:
            out.append(cur)
            cur = []
            continue
        cur.append(t)
    if cur:
        out.append(cur)
    return out


def split_clauses(toks):
    out, cur, depth = [], [], 0
    i = 0
    while i < len(toks):
        kind, v = toks[i]
        if kind == 'word' and v == 'OPTIONAL' and i + 1 < len(toks) and toks[i + 1][1] == 'MATCH':
            if cur:
                out.append(cur)
            cur = [['OPTIONAL', 'OPTIONAL'], toks[i + 1]]
            i += 2
            depth = 0
            continue
        if kind == 'word' and v in CLAUSE_HEADS and depth == 0:
            if cur:
                out.append(cur)
            cur = [toks[i]]
            depth = 0
            i += 1
            continue
        if v in ('(', '[', '{'):
            depth += 1
        elif v in (')', ']', '}'):
            depth = max(0, depth - 1)
        cur.append(toks[i])
        i += 1
    if cur:
        out.append(cur)
    return out


def const(tok):
    kind, v = tok
    if kind == 'num':
        return int(v) if re.fullmatch(r'-?\d+', v) else float(v)
    if kind == 'str':
        return bytes(v[1:-1], 'utf-8').decode('unicode_escape') if '\\' in v[1:-1] else v[1:-1]
    if kind == 'word' and v in ('true', 'false'):
        return v == 'true'
    raise Refuse('const:' + v)


class Q:
    def __init__(self):
        self.vars = []
        self.vidx = {}
        self.edges = []
        self.atoms = []
        self.items = []

    def var(self, name, kind):
        if name in self.vidx:
            v = self.vars[self.vidx[name]]
            if v['kind'] != kind:
                raise Refuse('varkind:' + name)
            return self.vidx[name]
        self.vidx[name] = len(self.vars)
        self.vars.append({'name': name, 'kind': kind, 'labels': [], 'props': []})
        return self.vidx[name]


class Builder:
    def __init__(self, manifest):
        self.stored = [l for l in manifest['vertex_labels'] if not l.startswith(('OUT_', 'IN_'))]
        self.types = list(manifest['edge_labels'])
        # LDBC splits Message into Post/Comment.  A new benchmark carries its own labels and
        # must not inherit that alias, so a manifest can replace the table.
        self.alias = dict(manifest.get('aliases', {'Message': ['Post', 'Comment'],
                                                   'MessageHasTags': ['HAS_TAG']}))
        self.prop_types = manifest.get('prop_types') or {}
        self.names = {}
        self.rel_seq = 0   # synthetic relationship variables are unique per occurrence
        self.pattern_atoms = {}   # clause -> atoms produced by a pattern property comparison

    def labels_of(self, names):
        out = set()
        for n in names:
            if n in self.alias:
                out.update(self.alias[n])
            elif n in self.stored:
                out.add(n)
            else:
                raise Refuse('label:' + n)
        return sorted(out)

    def types_of(self, names):
        out = set()
        for n in names:
            x = self.alias.get(n, [n])
            for t in x:
                if t not in self.types:
                    raise Refuse('type:' + t)
                out.add(t)
        return sorted(out)

    def declare_rel_vars(self, toks, q):
        # every `[name:...]` of a pattern is a relationship variable regardless of the clause's
        # parse, so a WHERE / RETURN may reference it before the edge is read
        for k in range(len(toks) - 3):
            if toks[k][1] == '[' and toks[k + 1][0] == 'word' and toks[k + 2][1] == ':':
                q.var(toks[k + 1][1], 'edge')

    def pattern(self, toks, q, clause, paths, opt, clause_vars=None):
        added = []
        self.declare_rel_vars(toks, q)
        for part in depth_split(toks):
            if not part:
                continue
            i, n = 0, len(part)
            path = None
            if part[0][0] == 'word' and i + 1 < n and part[i + 1][1] == '=':
                path = part[0][1]
                i += 2
            i, head = self.node(part, i, q, clause_vars)
            prev = head
            sedges = []
            while i < n and part[i][1] in ('-', '<'):
                i, e = self.rel(part, i)
                i, nxt = self.node(part, i, q, clause_vars)
                # a back arrow is the reverse of the text order: (a)<-[:T]-(b) is b -> a
                frm, to = (nxt, prev) if e.get('back') else (prev, nxt)
                e.update({'from': frm, 'to': to, 'clause': clause, 'path': path or '',
                          'path_nodes': path is not None, 'scoped': []})
                for prop, alias in e.get('prop_bind', []):
                    if alias in q.vidx and q.vars[q.vidx[alias]]['kind'] != 'edge':
                        raise Refuse('rel_prop_alias:' + alias)
                    if not e.get('var'):
                        raise Refuse('rel_prop_bind_without_var')
                    vid = q.var(e['var'], 'edge')
                    self.names[alias] = ('var', vid, prop)
                for name, op, val in e.get('prop_cmp', []):
                    if not e.get('var'):
                        raise Refuse('rel_prop_cmp_without_var')
                    at = {'var': q.var(e['var'], 'edge'), 'prop': name, 'op': op, 'consts': [val],
                          'clause': clause, 'or': -1, 'negated': False, 'post': False}
                    self.pattern_atoms.setdefault(clause, []).append(at)
                q.edges.append(e)
                sedges.append(len(q.edges) - 1)
                added.append(len(q.edges) - 1)
                prev = nxt
            if i != n:
                raise Refuse('pattern_tail:' + part[i][1])
            if path:
                if path in paths:
                    raise Refuse('path_reused:' + path)
                paths[path] = sedges
        return added

    def node(self, toks, i, q, clause_vars=None):
        if toks[i][1] != '(':
            raise Refuse('node_start')
        j = close_of(toks, i)
        inner = toks[i + 1:j]
        k = 0
        name, labels, props = None, [], []
        if k < len(inner) and inner[k][0] == 'word':
            name = inner[k][1]
            k += 1
        while k + 1 < len(inner) and inner[k][1] in (':', '|') and inner[k + 1][0] == 'word':
            labels.append(inner[k + 1][1])
            k += 2
        if k < len(inner) and inner[k][1] == '{':
            e = close_of(inner, k, '{', '}')
            props = self.props(inner[k + 1:e])
            k = e + 1
        if k != len(inner):
            raise Refuse('node_tail')
        if name is None:
            k = sum(1 for v in q.vars if v['name'].startswith('_a'))
            name = '_a%d' % k
            while name in q.vidx:
                k += 1
                name = '_a%d' % k
        vid = q.var(name, 'vertex')
        if clause_vars is not None and vid not in clause_vars:
            clause_vars.append(vid)
        v = q.vars[vid]
        for x in self.labels_of(labels):
            if x not in v['labels']:
                v['labels'].append(x)
        v['props'].extend(props)
        return j + 1, vid

    @staticmethod
    def props(inner):
        out = []
        for part in depth_split(inner):
            if not part:
                continue
            if len(part) != 3 or part[1][1] != ':':
                raise Refuse('map_key')
            out.append([part[0][1], const(part[2])])
        return out

    @staticmethod
    def props2(inner):
        """Pattern property entries: `{p: v}` (equality), `{p < v}` (an atom on the edge), and
        `{p: name}` with an unbound name: bind `name` to that relationship property (the generated
        LDBC candidates use this form to project a relationship column, e.g.
        `[:KNOWS {creationDate: cd}] ... RETURN cd`)."""
        eq, cmp, bind = [], [], []
        for part in depth_split(inner):
            if not part:
                continue
            if len(part) != 3 or part[0][0] != 'word':
                raise Refuse('map_key')
            if part[1][1] == ':':
                if part[2][0] == 'word' and part[2][1] not in ('true', 'false'):
                    bind.append([part[0][1], part[2][1]])
                else:
                    eq.append([part[0][1], const(part[2])])
            elif part[1][1] in COMPARE:
                cmp.append([part[0][1], part[1][1], const(part[2])])
            else:
                raise Refuse('map_key')
        return eq, cmp, bind

    def rel(self, toks, i):
        back = toks[i][1] == '<'
        i += 2 if back else 1
        var, types, lo, hi, props = None, [], 1, 1, []
        rel_var = None
        bracket = i < len(toks) and toks[i][1] == '['
        if bracket:
            j = close_of(toks, i, '[', ']')
            inner = toks[i + 1:j]
            k = 0
            if k < len(inner) and inner[k][0] == 'word' and inner[k][1] not in AGGS:
                var = inner[k][1]
                k += 1
            while k < len(inner) and inner[k][1] in (':', '|'):
                k += 1
                if k < len(inner) and inner[k][1] == ':':
                    k += 1
                if k < len(inner) and inner[k][0] == 'word':
                    types.append(inner[k][1])
                    k += 1
                else:
                    raise Refuse('rel_type')
            many = False
            if k < len(inner) and inner[k][1] == '*':
                many = True
                k += 1
                lo = 1
                if k < len(inner) and inner[k][0] == 'num':
                    lo = int(inner[k][1])
                    k += 1
                hi = lo
                if k < len(inner) and inner[k][1] == '..':
                    k += 1
                    hi = -1
                    if k < len(inner) and inner[k][0] == 'num':
                        hi = int(inner[k][1])
                        k += 1
            prop_cmp, prop_bind = [], []
            if k < len(inner) and inner[k][1] == '{':
                e = close_of(inner, k, '{', '}')
                props, prop_cmp, prop_bind = self.props2(inner[k + 1:e])
                k = e + 1
            if k != len(inner):
                raise Refuse('rel_tail')
            if var is None and len(types) == 1 and not many:
                # `-[KNOWS]-`: the relationship variable is named after its type and occurrence
                # (an occurrence suffix: a pattern may repeat a type, and two anonymous
                # relationships are two variables, not one)
                self.rel_seq += 1
                rel_var = '_r_%s_%d' % (types[0], self.rel_seq)
            i = j + 1
        # `<-[:T]-` ends with the left arrow's own `-`; it is not an undirected edge marker
        if back:
            if i >= len(toks) or toks[i][1] != '-':
                raise Refuse('rel_end')
            i += 1
            undirected = False
        else:
            undirected = i < len(toks) and toks[i][1] == '-' and not (i + 1 < len(toks) and toks[i + 1][1] == '>')
            if i < len(toks) and toks[i][1] == '-' and i + 1 < len(toks) and toks[i + 1][1] == '>':
                i += 2
            elif undirected:
                i += 1
            else:
                raise Refuse('rel_end')
        any_dir = undirected
        if not bracket and back:
            raise Refuse('rel_no_bracket')
        # a `-[T]-` edge is written in one direction: an undirected KNOWS of LDBC reads as two
        # directed edges in the encoding, and the pool's `-[:T]-` is the directed form; the same
        # for `-[T]->` (a back arrow before the label with a forward arrow after it)
        return i, {'types': self.types_of(types) if types else [], 'any_dir': any_dir, 'back': back,
                   'lo': lo, 'hi': hi, 'var': var or (rel_var or ''), 'props': props,
                   'prop_cmp': prop_cmp, 'prop_bind': prop_bind}

    def where(self, toks, q, clause, paths, opt):
        """WHERE: a conjunction of OR-groups of comparisons.  Returns atoms (with `or` ids)."""
        groups = [g for g in depth_split(toks, 'AND') if g]
        atoms = []
        gid = 0
        for g in groups:
            terms = [t for t in depth_split(g, 'OR') if t]
            if len(terms) > 1:
                for t in terms:
                    atoms.extend(self.term(t, q, clause, paths, gid))
                gid += 1
            else:
                atoms.extend(self.term(g, q, clause, paths, -1))
        return atoms

    def term(self, t, q, clause, paths, gid):
        neg = False
        while t and t[0][1] == 'NOT':
            neg = not neg
            if len(t) >= 3 and t[1][1] == '(' and close_of(t, 1) == len(t) - 1:
                t = t[2:-1]
            else:
                t = t[1:]
            if not t:
                raise Refuse('where_empty')
        # a negated pattern predicate is an anti-join; keep it for the fallback rather than
        # hoisting it as a positive pattern
        if neg and any(x[1] == '[' for x in t) and any(x[1] == '-' for x in t):
            raise Refuse('where_not_pattern')
        if not t:
            raise Refuse('where_empty')
        if t[0][1] == '(':
            # A parenthesised term.  A pattern predicate is hoisted into the clause's pattern
            # (a positive existential pattern is a join in Cypher's MATCH semantics).  A group
            # that encloses the whole term is unwrapped; a label-only disjunction becomes one
            # LABEL_ANY atom; the remaining nested boolean groups stay with the fallback.
            has_edge = any(x[1] == '[' for x in t) and any(x[1] == '-' for x in t)
            if has_edge:
                self.pattern(t, q, clause, paths, False)
                return []
            if close_of(t, 0) == len(t) - 1:
                inner = t[1:-1]
                if any(x[0] == 'word' and x[1] in ('AND', 'OR') for x in inner):
                    labels = []
                    if all((x[0] == 'word' and x[1] in ('AND', 'OR', 'NOT')) or x[1] in ('(', ')', ':')
                           or x[0] == 'word' for x in inner):
                        # `(a:Post OR a:Comment)` (and the redundant `OR (a:Comment)`): a single
                        # runtime label test over the union.
                        names = [x[1] for x in inner if x[0] == 'word' and x[1] not in ('AND', 'OR', 'NOT')]
                        if names and all(n in q.vidx for n in names[:1]):
                            var = names[0]
                            labels = [x[1] for x in inner if x[0] == 'word' and x[1] not in
                                      ('AND', 'OR', 'NOT', var)]
                            if labels:
                                return [dict({'var': q.vidx[var], 'prop': '', 'op': 'LABEL_ANY',
                                              'consts': labels, 'clause': clause, 'or': gid,
                                              'negated': neg, 'post': False})]
                    return self.where(inner, q, clause, paths, False)
                return self.term(inner, q, clause, paths, gid)
            raise Refuse('where_group')
        var = t[0][1]
        if var in paths:
            raise Refuse('path_predicate')
        vid, col = -1, -1
        if var in q.vidx:
            vid = q.vidx[var]
        elif var in self.names:
            ref = self.names[var]
            if ref[0] == 'col':
                col = ref[1]
            else:
                vid = ref[1]
        else:
            raise Refuse('where_name:' + str(var))
        # a label test `x:Post` / `x:Post:Comment` (a runtime label of the bound vertex)
        if len(t) >= 3 and t[1][1] == ':':
            labels, k = [], 1
            while k + 1 < len(t) and t[k][1] == ':' and t[k + 1][0] == 'word':
                labels.append(t[k + 1][1])
                k += 2
            if k == len(t) and labels:
                return [dict({'var': vid, 'prop': '', 'op': 'LABEL', 'consts': labels,
                              'clause': clause, 'or': gid, 'negated': neg, 'post': False})]
            raise Refuse('where_label')
        prop = ''
        k = 1
        if len(t) > k + 1 and t[k][1] == '.':
            prop = t[k + 1][1]
            k += 2
        base = {'var': vid, 'prop': prop, 'clause': clause, 'or': gid, 'negated': neg}
        if col >= 0:
            if prop:
                raise Refuse('where_col_prop')
            op = t[k][1]
            if op == 'IS':
                if k + 1 < len(t) and t[k + 1][1] == 'NULL':
                    return [dict(base, col=col, op='IS NULL', consts=[])]
                if k + 2 < len(t) and t[k + 1][1] == 'NOT' and t[k + 2][1] == 'NULL':
                    return [dict(base, col=col, op='IS NOT NULL', consts=[])]
                raise Refuse('where_is')
            if op == 'IN':
                if k + 1 >= len(t) or t[k + 1][1] != '[':
                    raise Refuse('where_in')
                c = close_of(t, k + 1, '[', ']')
                if c != len(t) - 1:
                    raise Refuse('where_in_tail')
                vals = [const(x[0]) for x in depth_split(t[k + 2:c]) if x and len(x) == 1]
                return [dict(base, col=col, op='IN', consts=vals)]
            if op not in COMPARE:
                raise Refuse('where_op:' + op)
            rhs = t[k + 1:]
            if len(rhs) != 1 or rhs[0][0] not in ('num', 'str'):
                raise Refuse('where_col_rhs')
            return [dict(base, col=col, op=op, consts=[const(rhs[0])])]
        if q.vars[vid]['kind'] == 'edge':
            if len(t) == 3 and t[1][1] in COMPARE and (t[2][0] in ('num', 'str')):
                return [dict(base, op=t[1][1], consts=[const(t[2])])]
            if len(t) == 5 and t[1][1] == '.' and t[2][0] == 'word' and t[3][1] in COMPARE and \
                    t[4][0] in ('num', 'str'):
                # w.workFrom < 2009: a property comparison on the relationship variable
                return [dict(base, prop=t[2][1], op=t[3][1], consts=[const(t[4])])]
            raise Refuse('where_edge_expr')
        if len(t) == 4 and t[1][1] == '.' and t[2][0] == 'word' and t[3][1] in COMPARE:
            # var.prop op <operand>: the operand follows; handled by the generic path
            pass
        op = t[k][1]
        if op == 'IS':
            if k + 1 < len(t) and t[k + 1][1] == 'NULL':
                return [dict(base, op='IS NULL', consts=[])]
            if k + 2 < len(t) and t[k + 1][1] == 'NOT' and t[k + 2][1] == 'NULL':
                return [dict(base, op='IS NOT NULL', consts=[])]
            raise Refuse('where_is')
        if op == 'IN':
            if k + 1 >= len(t) or t[k + 1][1] != '[':
                raise Refuse('where_in')
            c = close_of(t, k + 1, '[', ']')
            if c != len(t) - 1:
                raise Refuse('where_in_tail')
            vals = [const(x[0]) for x in depth_split(t[k + 2:c]) if x and len(x) == 1]
            return [dict(base, op='IN', consts=vals)]
        if op not in COMPARE:
            if op == '.':
                raise Refuse('where_left_expr')
            raise Refuse('where_op:' + op)
        rhs = t[k + 1:]
        if len(rhs) == 1:
            if rhs[0][0] not in ('num', 'str', 'word'):
                raise Refuse('where_expr')
            if rhs[0][0] == 'word' and rhs[0][1] not in ('true', 'false'):
                if rhs[0][1] not in q.vidx:
                    raise Refuse('where_name:' + str(rhs[0][1]))
                return [dict(base, op=op, var_rhs=q.vidx[rhs[0][1]])]   # a bare variable: entity / id
            return [dict(base, op=op, consts=[const(rhs[0])])]
        if len(rhs) == 3 and rhs[0][0] == 'word' and rhs[1][1] == '.' and rhs[0][1] in q.vidx:
            rv = q.vidx[rhs[0][1]]
            if q.vars[rv]['kind'] != 'vertex':
                raise Refuse('compare_rel_var')
            return [dict(base, op=op, var_rhs=rv, rhs_prop=rhs[2][1])]
        raise Refuse('where_expr')

    # ------------------------------------------------------------- projection lists
    def items_list(self, toks, q):
        """Items of a RETURN / WITH list plus the index where its tail (ORDER / LIMIT / WHERE) starts.

        An alias is registered by `register_items` after the items are appended to q.items, because
        a WITH aggregate / projected property is a column and its identity is that item's index.
        """
        items = []
        distinct = bool(toks and toks[0][1] == 'DISTINCT')
        k = 1 if distinct else 0
        n = len(toks)
        base = len(q.items)
        while k < n:
            if toks[k][0] == 'word' and toks[k][1] in ('ORDER', 'LIMIT', 'SKIP', 'WHERE') and \
                    not (k + 1 < n and toks[k + 1][1] == '.'):
                break
            if toks[k][1] == ',':
                k += 1
                continue
            start = k
            it, k = self.item(toks, k, q)
            if k < n and toks[k][1] == 'AS':
                it['alias'] = toks[k + 1][1]
                it['label'] = toks[k + 1][1]
                k += 2
            else:
                it['label'] = ident_text(toks[start:k])
            items.append(it)
            # A WITH / RETURN list may refer to an alias earlier in the same list
            # (`count(x) AS a, count(y) AS b, a + b AS total`): register it with the position it
            # will occupy once the list is appended.  `register_items` repeats this after append.
            idx = base + len(items) - 1
            if it['kind'] == 'agg' or it['kind'] in ('col', 'expr', 'const'):
                if it.get('alias'):
                    self.names[it['alias']] = ('col', idx)
            elif it['kind'] == 'key' and it.get('var', -1) >= 0:
                if it.get('alias'):
                    self.names[it['alias']] = ('var', it['var'], it.get('prop', ''))
                elif it.get('prop'):
                    self.names.setdefault(it['prop'], ('var', it['var'], it['prop']))
        return items, distinct, k

    def register_items(self, items, indices):
        """Register the aliases of a WITH / RETURN projection.  A plain carried variable keeps its
        identity; a projected property or an aggregate becomes a column of the plan."""
        for it, idx in zip(items, indices):
            if it['kind'] == 'key' and it.get('prop', '') == '' and it.get('var', -1) >= 0:
                self.names[it.get('alias') or it['label']] = ('var', it['var'], '')
            elif it['kind'] == 'key' and it.get('var', -1) >= 0 and it.get('prop', ''):
                # A projected property may be referenced by its short name in a later clause
                # (`WITH liker.firstName ... RETURN firstName`); the pool uses both spellings.
                self.names[it.get('alias') or it['label']] = ('var', it['var'], it['prop'])
                if not it.get('alias'):
                    self.names.setdefault(it['prop'], ('var', it['var'], it['prop']))
            elif it['kind'] in ('col', 'expr'):
                if it.get('alias'):
                    self.names[it['alias']] = ('col', idx)
            else:
                if it.get('alias'):
                    self.names[it['alias']] = ('col', idx)

    # ------------------------------------------------------------- scalar expressions
    # CASE / coalesce projections.  The expression is evaluated after the match; it adds no
    # pattern work and no atom to the join.  Only the forms the pool uses are accepted
    # (comparisons, IS [NOT] NULL, label tests, AND/OR/NOT, property / constant leaves); an
    # arithmetic or pattern leaf is refused with a reason, which is counted as a fallback.
    def expr(self, toks, i, q, stops=()):
        e, i = self.expr_or(toks, i, q, stops)
        return e, i

    def expr_or(self, toks, i, q, stops):
        left, i = self.expr_and(toks, i, q, stops)
        args = [left]
        while i < len(toks) and toks[i][0] == 'word' and toks[i][1] == 'OR':
            r, i = self.expr_and(toks, i + 1, q, stops)
            args.append(r)
        return (args[0] if len(args) == 1 else {'k': 'or', 'a': args}), i

    def expr_and(self, toks, i, q, stops):
        left, i = self.expr_not(toks, i, q, stops)
        args = [left]
        while i < len(toks) and toks[i][0] == 'word' and toks[i][1] == 'AND':
            r, i = self.expr_not(toks, i + 1, q, stops)
            args.append(r)
        return (args[0] if len(args) == 1 else {'k': 'and', 'a': args}), i

    def expr_not(self, toks, i, q, stops):
        if i < len(toks) and toks[i][0] == 'word' and toks[i][1] == 'NOT':
            e, i = self.expr_not(toks, i + 1, q, stops)
            return {'k': 'not', 'a': [e]}, i
        return self.expr_cmp(toks, i, q, stops)

    def expr_cmp(self, toks, i, q, stops):
        left, i = self.expr_add(toks, i, q, stops)
        if i < len(toks):
            op = toks[i][1]
            if op == 'IS':
                neg = i + 1 < len(toks) and toks[i + 1][1] == 'NOT'
                j = i + 2 if neg else i + 1
                if j < len(toks) and toks[j][1] == 'NULL':
                    e = {'k': 'isnull', 'a': [left]}
                    return ({'k': 'not', 'a': [e]} if neg else e), j + 1
                raise Refuse('expr_is')
            if op in COMPARE:
                right, j = self.expr_primary(toks, i + 1, q, stops)
                return {'k': 'cmp', 'op': op, 'a': [left, right]}, j
        return left, i

    def expr_add(self, toks, i, q, stops):
        left, i = self.expr_mul(toks, i, q, stops)
        while i < len(toks) and toks[i][1] in ('+', '-'):
            op = toks[i][1]
            right, i = self.expr_mul(toks, i + 1, q, stops)
            left = {'k': 'arith', 'op': op, 'a': [left, right]}
        return left, i

    def expr_mul(self, toks, i, q, stops):
        left, i = self.expr_primary(toks, i, q, stops)
        while i < len(toks) and toks[i][1] in ('*', '/'):
            op = toks[i][1]
            right, i = self.expr_primary(toks, i + 1, q, stops)
            left = {'k': 'arith', 'op': op, 'a': [left, right]}
        return left, i

    def vars_name(self, vid):
        return self.cur_q.vars[vid]['name']

    def expr_primary(self, toks, i, q, stops):
        self.cur_q = q
        if i >= len(toks):
            raise Refuse('expr_empty')
        kind, v = toks[i]
        if v == '(':
            e, j = self.expr(toks, i + 1, q, stops)
            if j >= len(toks) or toks[j][1] != ')':
                raise Refuse('expr_paren')
            return e, j + 1
        if kind == 'word' and v == 'CASE':
            args, j = [], i + 1
            operand = None
            if j < len(toks) and not (toks[j][0] == 'word' and toks[j][1] == 'WHEN'):
                operand, j = self.expr_or(toks, j, q, stops)
            while j < len(toks) and toks[j][0] == 'word' and toks[j][1] == 'WHEN':
                cond, j = self.expr_or(toks, j + 1, q, stops)
                if j >= len(toks) or toks[j][1] != 'THEN':
                    raise Refuse('expr_then')
                val, j = self.expr_or(toks, j + 1, q, stops)
                if operand is not None:
                    cond = {'k': 'cmp', 'op': '=', 'a': [operand, cond]}
                args.extend([cond, val])
            if j < len(toks) and toks[j][0] == 'word' and toks[j][1] == 'ELSE':
                val, j = self.expr_or(toks, j + 1, q, stops)
                args.append(val)
            if j >= len(toks) or toks[j][1] != 'END':
                raise Refuse('expr_end')
            return {'k': 'case', 'a': args}, j + 1
        if kind == 'word' and v.lower() in ('coalesce',) and i + 1 < len(toks) and toks[i + 1][1] == '(':
            c = close_of(toks, i + 1)
            args = []
            for part in depth_split(toks[i + 2:c]):
                if part:
                    e, j = self.expr(part, 0, q, stops)
                    if j != len(part):
                        raise Refuse('expr_arg')
                    args.append(e)
            return {'k': 'coalesce', 'a': args}, c + 1
        if kind == 'word' and v.lower() in AGGS and i + 1 < len(toks) and toks[i + 1][1] == '(':
            raise Refuse('expr_agg')
        if kind == 'num' or kind == 'str':
            return {'k': 'const', 'v': const(toks[i])}, i + 1
        if kind == 'op' and v == '-' and i + 1 < len(toks) and toks[i + 1][0] == 'num':
            return {'k': 'const', 'v': -const(toks[i + 1])}, i + 2
        if kind == 'word' and v.lower() == 'null':
            return {'k': 'const', 'v': None}, i + 1
        if kind == 'word' and v.lower() in ('true', 'false'):
            return {'k': 'const', 'v': v.lower() == 'true'}, i + 1
        if kind == 'word':
            name, j, prop = v, i + 1, ''
            labels = []
            while j + 1 < len(toks) and toks[j][1] == ':' and toks[j + 1][0] == 'word':
                labels.append(toks[j + 1][1])
                j += 2
            if labels:
                return {'k': 'label', 'var': name, 'labels': labels}, j
            if j + 1 < len(toks) and toks[j][1] == '.' and toks[j + 1][0] == 'word':
                prop = toks[j + 1][1]
                j += 2
            if name in q.vidx:
                if prop:
                    return {'k': 'prop', 'var': name, 'prop': prop}, j
                return {'k': 'var', 'var': name}, j
            if name in self.names:
                ref = self.names[name]
                if ref[0] == 'col':
                    return {'k': 'col', 'col': ref[1]}, j
                if prop:
                    return {'k': 'prop', 'var': self.vars_name(ref[1]), 'prop': prop}, j
                if len(ref) > 2 and ref[2]:
                    return {'k': 'prop', 'var': self.vars_name(ref[1]), 'prop': ref[2]}, j
                return {'k': 'var', 'var': self.vars_name(ref[1])}, j
            raise Refuse('expr_name:' + name)
        raise Refuse('expr_leaf:' + str(v))

    def item(self, toks, i, q, order=False):
        kind, v = toks[i]
        n = len(toks)
        # a scalar arithmetic projection (`lao_count + scot_count AS sum`) is one expression item
        depth, arith = 0, False
        for x in toks[i:]:
            if x[1] in ('(', '[', '{'):
                depth += 1
            elif x[1] in (')', ']', '}'):
                depth -= 1
            elif depth == 0 and x[1] == ',':
                break
            elif depth == 0 and x[0] == 'word' and x[1] == 'AS':
                break
            elif depth == 0 and x[1] in ('+', '-', '*', '/'):
                arith = True
        if arith and not (kind == 'word' and v.upper() == 'CASE'):
            try:
                e, j = self.expr(toks, i, q)
                return {'kind': 'expr', 'expr': e}, j
            except Refuse:
                pass
        if kind == 'word' and v == 'CASE':
            e, j = self.expr(toks, i, q)
            return {'kind': 'expr', 'expr': e}, j
        if kind == 'word' and v.lower() == 'coalesce' and i + 1 < n and toks[i + 1][1] == '(':
            e, j = self.expr(toks, i, q)
            return {'kind': 'expr', 'expr': e}, j
        if v == '(':
            # `(CASE ... END)` / `(coalesce(...))`: the parentheses around one scalar item
            try:
                j = close_of(toks, i)
                e, k = self.expr(toks, i + 1, q)
                if k == j:
                    return {'kind': 'expr', 'expr': e}, j + 1
            except Refuse:
                pass
        if kind == 'word' and not (i + 1 < n and toks[i + 1][1] == '.') and v in self.names:
            # a name produced by an earlier WITH (a column or a carried variable)
            ref = self.names[v]
            if ref[0] == 'col':
                return {'kind': 'col', 'col': ref[1]}, i + 1
            return {'kind': 'key', 'var': ref[1], 'prop': ref[2]}, i + 1
        if kind == 'word' and v.lower() == 'null':
            return {'kind': 'const', 'value': None, 'label': 'null'}, i + 1
        if kind == 'word' and v.lower() in AGGS and i + 1 < n and toks[i + 1][1] == '(':
            fn = v.lower()
            c = close_of(toks, i + 1)
            inner = toks[i + 2:c]
            distinct = bool(inner and inner[0][1] == 'DISTINCT')
            if distinct:
                inner = inner[1:]
            if not inner:
                return {'kind': 'agg', 'func': 'count_star'}, c + 1
            if len(inner) == 1 and inner[0][0] == 'num':
                if fn != 'count':
                    raise Refuse('agg_arg')
                return {'kind': 'agg', 'func': 'count_star'}, c + 1
            if len(inner) == 1 and inner[0][0] == 'word' and inner[0][1] in self.names and \
                    self.names[inner[0][1]][0] == 'col':
                # count(cnt): an aggregate over a WITH column
                if len(inner) > 1:
                    raise Refuse('agg_arg')
                fname = {'count': 'count', 'sum': 'sum', 'min': 'min', 'max': 'max', 'collect': 'collect'}[fn]
                return {'kind': 'agg', 'func': fname, 'distinct': distinct,
                        'arg_col': self.names[inner[0][1]][1]}, c + 1
            if len(inner) == 1 and inner[0][0] == 'word':
                var, attr = inner[0][1], ''
            elif len(inner) == 3 and inner[0][0] == 'word' and inner[1][1] == '.' and inner[2][0] == 'word':
                var, attr = inner[0][1], inner[2][1]
            else:
                raise Refuse('agg_arg')
            if var not in q.vidx:
                raise Refuse('agg_var:' + var)
            vid = q.vidx[var]
            if q.vars[vid]['kind'] != 'vertex':
                raise Refuse('agg_edge_var')
            fname = {'count': 'count', 'sum': 'sum', 'min': 'min', 'max': 'max', 'collect': 'collect'}[fn]
            return {'kind': 'agg', 'func': fname, 'distinct': distinct, 'arg_var': vid, 'arg_prop': attr}, c + 1
        if kind == 'word' and v in q.vidx:
            j = i + 1
            prop = ''
            if j + 1 < n and toks[j][1] == '.':
                prop = toks[j + 1][1]
                j += 2
            if q.vars[q.vidx[v]]['kind'] == 'edge':
                # a relationship column: the projection carries it as an edge property item
                if prop not in EDGE_COLS:
                    raise Refuse('edge_item:' + (prop or 'id'))
                return {'kind': 'edge_prop', 'var': q.vidx[v], 'prop': prop}, j
            return {'kind': 'key', 'var': q.vidx[v], 'prop': prop}, j
        raise Refuse('item_expr')


def ident_text(toks):
    out = ''
    for k, v in toks:
        out += v
    return out


def ref_kind(it):
    """The identity of a reference in ORDER BY (kind, var, prop[, func, arg])."""
    if it['kind'] in ('key', 'edge_prop'):
        return ('key', it['var'], it.get('prop', ''))
    if it['kind'] == 'agg':
        return ('agg', it.get('func', ''), it.get('arg_var', -1), it.get('arg_prop', ''),
                it.get('distinct', False), it.get('arg_col', -1))
    if it['kind'] == 'col':
        return ('col', it.get('col', -1))
    if it['kind'] == 'expr':
        return ('expr', json.dumps(it.get('expr'), sort_keys=True))
    return ('const', it.get('value'))


def schedule(q, clauses):
    """Place every atom at the MATCH-level clause after which all of its variables are bound, then
    turn the atoms that land after the last clause into filters of the RETURN (`pre_where`)."""
    start = {}
    for ci, c in enumerate(clauses):
        if c['kind'] not in ('MATCH', 'OPTIONAL_MATCH'):
            continue
        # a vertex written by a clause is bound there, even when the clause has no edge
        for v in c.get('vars', []):
            start.setdefault(v, ci)
        for e in c['edges']:
            for v in (q.edges[e]['from'], q.edges[e]['to']):
                start.setdefault(v, ci)
    for ci, c in enumerate(clauses):
        if c['kind'] not in ('MATCH', 'OPTIONAL_MATCH', 'WITH'):
            continue
        kept = []
        for a in c.get('where', []):
            c1 = start.get(a['var'], ci)
            c2 = start.get(a['var_rhs'], -1) if 'var_rhs' in a else -1
            if 'var_rhs' in a:
                # a join predicate is decided at the later of the two bindings; a bare variable
                # against a variable / entity is a filter of the matching (`post`)
                a['clause'] = max(c1, c2)
                a['post'] = a['prop'] == '' and 'rhs_prop' not in a
                if a['post']:
                    a['prop'] = 'id'
            else:
                a['clause'] = c1
                a['post'] = False
            q.atoms.append(a)
            kept.append(len(q.atoms) - 1)
        c['where'] = kept
    # A post atom (an entity / id inequality) is applied at the stage where its later variable
    # is bound.  Keeping it in that clause's where list lets the masked walk prune the stage
    # before expanding the next one; moving it to RETURN would expand rows that the filter can
    # already reject (and makes a one-member envelope do more work than the per-candidate path).
    for c in clauses:
        if c['kind'] == 'RETURN':
            c['pre_where'] = list(c.get('pre_where', []))
    return


def _is_float(x):
    return isinstance(x, float) and not x.is_integer()


def check_types(plan, manifest):
    """Refuse numeric predicates on FLOAT columns and non-integral constants.

    The v3 Value is int64/string (v3_ir.h), and the sidecar stores FLOAT as its exact decimal
    text.  Comparing such a column numerically in the int path would be a silent approximation,
    so a candidate that needs it is a coverage fact (`float_prop` / `float_const`) and goes to
    the Fallback row instead of being evaluated with a truncated value.
    """
    pt = manifest.get('prop_types') or {}
    if not pt:
        return
    kinds = {}
    for v in plan['variables']:
        if v['kind'] == 'vertex':
            kinds[v['name']] = [pt.get(l, {}) for l in v['labels']]
        else:
            kinds[v['name']] = []
    for e in plan.get('edges', []):
        if e.get('var'):
            kinds[e['var']] = [pt.get('E:' + t, {}) for t in e.get('types', [])]
    def kind_of(var, prop):
        out = set()
        for d in kinds.get(var, []):
            if prop in d:
                out.add(d[prop])
        return out
    for a in plan['where']:
        if a.get('op') in ('IS NULL', 'IS NOT NULL'):
            continue
        ks = kind_of(a.get('var', ''), a.get('prop', ''))
        if not ks:
            raise Refuse('missing_prop:' + a.get('prop', ''))
        if 'list' in ks:
            raise Refuse('list_prop:' + a.get('prop', ''))
        if 'float' in ks:
            raise Refuse('float_prop:' + a.get('prop', ''))
        for c in a.get('consts', []):
            if _is_float(c):
                raise Refuse('float_const')
            if isinstance(c, float):
                a['consts'][a['consts'].index(c)] = int(c)
    for it in plan['items']:
        if it.get('kind') == 'agg' and it.get('func') in ('sum', 'avg', 'min', 'max', 'stdev', 'stdevp'):
            ks = kind_of(it.get('var', ''), it.get('prop', '')) | kind_of(it.get('arg_var', ''), it.get('arg_prop', ''))
            if 'float' in ks:
                raise Refuse('float_agg:' + (it.get('prop') or it.get('arg_prop') or ''))
            if 'list' in ks:
                raise Refuse('list_agg')
        if it.get('kind') == 'const' and _is_float(it.get('value')):
            raise Refuse('float_const')
    for e in plan.get('edges', []):
        for p, x in e.get('props', []):
            ks = kind_of(e.get('var', ''), p) if e.get('var') else set()
            if not ks:
                raise Refuse('missing_prop:' + p)
            if 'float' in ks or _is_float(x):
                raise Refuse('float_prop:' + p)
    for v in plan['variables']:
        if v['kind'] != 'vertex':
            continue
        for p, x in v.get('props', []):
            ks = kind_of(v['name'], p)
            if not ks:
                raise Refuse('missing_prop:' + p)
            if 'float' in ks or _is_float(x):
                raise Refuse('float_prop:' + p)


def build(text, qid, family, template, manifest):
    b = Builder(manifest)
    toks = lex(text)
    clauses = split_clauses(toks)
    q = Q()
    paths = {}
    out_clauses = []
    order_items, order_desc = [], []
    skip_n, limit_n = None, None
    for ci, c in enumerate(clauses):
        head = c[0][1]
        if head in ('MATCH', 'OPTIONAL'):
            body = c[1:]
            if head == 'OPTIONAL':
                if not body or body[0][1] != 'MATCH':
                    raise Refuse('optional_head')
                body = body[1:]
            depth, wpos = 0, None
            for j, t in enumerate(body):
                if t[1] == 'WHERE' and depth == 0:
                    wpos = j
                    break
                if t[1] in ('(', '[', '{'):
                    depth += 1
                elif t[1] in (')', ']', '}'):
                    depth -= 1
            cvs = []
            edges = b.pattern(body if wpos is None else body[:wpos], q, ci, paths, head == 'OPTIONAL', cvs)
            atoms = b.where(body[wpos + 1:], q, ci, paths, head == 'OPTIONAL') if wpos is not None else []
            atoms = b.pattern_atoms.pop(ci, []) + atoms
            out_clauses.append({'kind': 'OPTIONAL_MATCH' if head == 'OPTIONAL' else 'MATCH',
                                'edges': edges, 'vars': cvs, 'where': atoms})
        elif head == 'RETURN':
            if ci != len(clauses) - 1:
                raise Refuse('after_return')
            body = c[1:]
            depth = 0
            end = len(body)
            for j, t in enumerate(body):
                if t[1] in ('(', '[', '{'):
                    depth += 1
                elif t[1] in (')', ']', '}'):
                    depth -= 1
                elif depth == 0 and t[0] == 'word' and t[1] in ('ORDER', 'LIMIT', 'SKIP'):
                    end = j
                    break
            items, distinct, _ = b.items_list(body[:end], q)
            rest = body[end:]
            base = len(q.items)
            for it in items:
                it.setdefault('label', '')
                q.items.append(it)
            ret_items = list(range(base, base + len(items)))
            b.register_items(items, ret_items)
            ret = {'kind': 'RETURN', 'items': ret_items, 'distinct': distinct}
            out_clauses.append(ret)
            r = 0
            while r < len(rest):
                if rest[r][1] == 'ORDER':
                    if r + 1 >= len(rest) or rest[r + 1][1] != 'BY':
                        raise Refuse('order')
                    r += 2
                    while r < len(rest) and rest[r][1] not in ('SKIP', 'LIMIT'):
                        if rest[r][1] == ',':
                            r += 1
                            continue
                        it, r2 = b.item(rest, r, q, order=True)
                        desc = False
                        if r2 < len(rest) and rest[r2][1] in ('ASC', 'DESC', 'DESCENDING', 'ASCENDING'):
                            desc = rest[r2][1].startswith('DESC')
                            r2 += 1
                        order_items.append(it)
                        order_desc.append(desc)
                        r = r2
                    continue
                if rest[r][1] == 'SKIP':
                    skip_n = const(rest[r + 1])
                    r += 2
                    continue
                if rest[r][1] == 'LIMIT':
                    limit_n = const(rest[r + 1])
                    r += 2
                    continue
                raise Refuse('tail:' + rest[r][1])
        elif head == 'WITH':
            body = c[1:]
            k = 0
            # a WITH tail (WHERE / ORDER BY / SKIP / LIMIT) is part of the WITH clause
            depth = 0
            for j, t in enumerate(body):
                if t[1] in ('(', '[', '{'):
                    depth += 1
                elif t[1] in (')', ']', '}'):
                    depth -= 1
                elif depth == 0 and t[0] == 'word' and t[1] in ('WHERE', 'ORDER', 'SKIP', 'LIMIT'):
                    k = j
                    break
            else:
                k = len(body)
            items, distinct, _ = b.items_list(body[:k], q)
            base = len(q.items)
            for it in items:
                it.setdefault('label', '')
                q.items.append(it)
            indices = list(range(base, base + len(items)))
            b.register_items(items, indices)
            carried, out_items = [], []
            for it, idx in zip(items, indices):
                if it['kind'] == 'key' and it.get('prop', '') == '' and it.get('var', -1) >= 0:
                    carried.append(it['var'])
                else:
                    out_items.append(idx)
            cwh = {'kind': 'WITH', 'items': indices, 'distinct': distinct,
                   'carried': sorted(set(carried)), 'out_items': out_items,
                   'out_vars': sorted(set(carried)), 'where': []}
            # the tail: WHERE (HAVING) then ORDER BY / SKIP / LIMIT
            r = k
            if r < len(body) and body[r][1] == 'WHERE':
                r += 1
                wstart = r
                depth = 0
                while r < len(body):
                    if body[r][1] in ('(', '[', '{'):
                        depth += 1
                    elif body[r][1] in (')', ']', '}'):
                        depth -= 1
                    elif depth == 0 and body[r][0] == 'word' and body[r][1] in ('ORDER', 'SKIP', 'LIMIT'):
                        break
                    r += 1
                cwh['where'] = b.where(body[wstart:r], q, ci, paths, False)
            while r < len(body):
                if body[r][1] == 'ORDER':
                    if r + 1 >= len(body) or body[r + 1][1] != 'BY':
                        raise Refuse('order')
                    r += 2
                    while r < len(body) and body[r][1] not in ('SKIP', 'LIMIT'):
                        if body[r][1] == ',':
                            r += 1
                            continue
                        it, r2 = b.item(body, r, q, order=True)
                        # the key is a WITH output column: reuse an existing item or append one
                        idx = None
                        for j in indices:
                            if ref_kind(q.items[j]) == ref_kind(it):
                                idx = j
                                break
                        if idx is None:
                            it.setdefault('label', '')
                            q.items.append(it)
                            idx = len(q.items) - 1
                            cwh['items'].append(idx)
                            cwh['out_items'].append(idx)
                        desc = False
                        if r2 < len(body) and body[r2][1] in ('ASC', 'DESC', 'DESCENDING', 'ASCENDING'):
                            desc = body[r2][1].startswith('DESC')
                            r2 += 1
                        cwh.setdefault('order_items', []).append(idx)
                        cwh.setdefault('order_desc', []).append(desc)
                        r = r2
                    continue
                if body[r][1] == 'SKIP':
                    cwh['skip_n'] = const(body[r + 1])
                    r += 2
                    continue
                if body[r][1] == 'LIMIT':
                    cwh['limit_n'] = const(body[r + 1])
                    r += 2
                    continue
                raise Refuse('with_tail:' + body[r][1])
            out_clauses.append(cwh)
        else:
            raise Refuse('clause:' + head)
    # relationship variables of an edge that no clause adds (not reachable: every parsed edge
    # belongs to a clause) and relationship variables referenced before their edge: declared now
    for e in q.edges:
        if e['var']:
            q.var(e['var'], 'edge')
    schedule(q, out_clauses)
    ret = out_clauses[-1]
    # ORDER BY keys are result-column positions: an expression already projected uses its
    # position, any other (non-aggregate) expression is appended as a trailing column
    tail = []
    for it in order_items:
        idx = None
        for pos, j in enumerate(ret['items']):
            if ref_kind(q.items[j]) == ref_kind(it):
                idx = pos
                break
        if idx is None:
            if it['kind'] == 'agg':
                raise Refuse('order_agg_not_returned')
            it['label'] = ''
            q.items.append(it)
            ret['items'].append(len(q.items) - 1)
            idx = len(ret['items']) - 1
        tail.append(idx)
    plan = {'id': qid, 'family': family, 'template': template, 'text': text,
            'clauses': out_clauses,
            'variables': [{'name': v['name'], 'kind': v['kind'], 'labels': v['labels'],
                           'props': [[p, x] for p, x in v['props']]} for v in q.vars],
            'edges': [{'from': e['from'], 'to': e['to'], 'types': e['types'], 'dir': '-' if e['any_dir'] else '->',
                       'lo': e['lo'], 'hi': e['hi'], 'var': e['var'], 'props': [[p, x] for p, x in e['props']],
                       'clause': e['clause'], 'path': e['path']} for e in q.edges],
            'where': [{'var': a['var'], 'prop': a['prop'], 'op': a['op'],
                       'consts': a.get('consts', []), **({'var_rhs': a['var_rhs']} if 'var_rhs' in a else {}),
                       **({'rhs_prop': a['rhs_prop']} if 'rhs_prop' in a else {}),
                       **({'col': a['col']} if a.get('col', -1) >= 0 else {}),
                       'clause': a['clause'], 'or': a['or'], 'negated': a.get('negated', False),
                       'post': a.get('post', False)} for a in q.atoms],
            'items': [{'kind': it['kind'], 'func': it.get('func', ''), 'distinct': it.get('distinct', False),
                       'var': it.get('var', -1), 'prop': it.get('prop', ''),
                       'arg_var': it.get('arg_var', -1), 'arg_prop': it.get('arg_prop', ''),
                       **({'col': it['col']} if it.get('col', -1) >= 0 else {}),
                       **({'arg_col': it['arg_col']} if it.get('arg_col', -1) >= 0 else {}),
                       **({'expr': it['expr']} if it.get('expr') else {}),
                       'label': it.get('label', '')} for it in q.items]}
    for a in plan['where']:
        if a['var'] >= 0:
            a['var'] = q.vars[a['var']]['name']
        if 'var_rhs' in a:
            a['var_rhs'] = q.vars[a['var_rhs']]['name']
    for e in plan['edges']:
        e['from'] = q.vars[e['from']]['name']
        e['to'] = q.vars[e['to']]['name']
    for it in plan['items']:
        if it['var'] >= 0:
            it['var'] = q.vars[it['var']]['name']
        if it['arg_var'] >= 0:
            it['arg_var'] = q.vars[it['arg_var']]['name']
    for cl in plan['clauses']:
        if 'vars' in cl:
            cl['vars'] = [q.vars[v]['name'] for v in cl['vars']]
        if cl['kind'] in ('WITH', 'RETURN'):
            cl['carried'] = [q.vars[v]['name'] for v in cl.get('carried', [])]
            cl['out_vars'] = [q.vars[v]['name'] for v in cl.get('out_vars', [])]
        if cl['kind'] == 'RETURN':
            cl['pre_where'] = cl.get('pre_where', [])
    if tail:
        plan['clauses'].append({'kind': 'ORDER_BY', 'keys': tail, 'desc': order_desc})
    if skip_n is not None:
        plan['clauses'].append({'kind': 'SKIP', 'n': skip_n})
    if limit_n is not None:
        plan['clauses'].append({'kind': 'LIMIT', 'n': limit_n})
    check_types(plan, manifest)
    return plan


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pool', default=os.path.join(POOL_DIR, 'cqplus-v3', 'cqplus.jsonl'))
    ap.add_argument('--labels', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--k', type=int, default=8)
    ap.add_argument('--all', action='store_true', help='write one file per distinct text, not rounds')
    ap.add_argument('--families', default='', help='comma separated family filter (with --all)')
    args = ap.parse_args()
    manifest = json.load(open(args.labels))
    recs = [json.loads(l) for l in open(args.pool)]
    ok, why = collections.Counter(), collections.Counter()
    cnt_internal = 0
    plans = {}
    first = {}
    for r in recs:
        if not r.get('in_cqplus_v3', r['in_cqplus']):
            continue
        if r['text'] in first:
            continue
        first[r['text']] = (r['family'], r['candidate'])
        key = (r['family'], r['candidate'])
        try:
            plans[key] = build(r['text'], '%s#%d' % (r['family'], r['candidate']), r['family'], r['template'], manifest)
            ok['ok'] += 1
        except Refuse as ex:
            why[str(ex)] += 1
            if os.environ.get('V3_DUMP_REFUSED'):
                with open(os.environ['V3_DUMP_REFUSED'], 'a') as df:
                    df.write(json.dumps({'reason': str(ex), 'text': r['text']}) + '\n')
        except Exception as ex:   # a compiler bug on an unusual text: count it
            why['internal:' + type(ex).__name__ + ':' + str(ex)[:40]] += 1
            import traceback
            if cnt_internal < 5:
                cnt_internal += 1
                traceback.print_exc()
    os.makedirs(args.out, exist_ok=True)
    if args.all:
        only = set(x for x in args.families.split(',') if x)
        for n, (text, key) in enumerate(sorted(first.items())):
            if key in plans and (not only or plans[key]['template'] in only):
                with open(os.path.join(args.out, 'q%05d.json' % n), 'w') as f:
                    json.dump(plans[key], f)
    rounds = []
    for family in sorted({k[0] for k in plans}):
        cands = sorted(k[1] for k in plans if k[0] == family)
        if len(cands) >= args.k:
            rounds.append({'family': family, 'template': plans[(family, cands[0])]['template'],
                           'queries': [plans[(family, c)] for c in cands[:args.k]]})
    for k, rd in enumerate(rounds):
        with open(os.path.join(args.out, 'round-%04d.json' % k), 'w') as f:
            json.dump(rd, f)
    summary = {'compiled': sum(ok.values()), 'refused': why.most_common(),
               'rounds': [r['family'] for r in rounds]}
    with open(os.path.join(args.out, 'coverage.json'), 'w') as f:
        json.dump(summary, f, indent=1)
    print(json.dumps({'compiled': sum(ok.values()), 'rounds': len(rounds),
                      'refused': dict(collections.Counter({k.split(':')[0]: 0 for k in why})),
                      'refused_top': why.most_common(25)}, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
