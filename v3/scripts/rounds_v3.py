#!/usr/bin/env python3
"""Rounds of the v3 fixed-pattern / variable-length stages (A, B) of the CQ+ candidate pool.

A pool candidate is parsed from its text alone.  Inside the stages the v3 plan format
(../include/v3_ir.h) accepts it; outside, it is refused with a reason, so the refusal counts are
coverage facts of the executor, never silent approximations.

Stage A (fixed patterns): one MATCH clause, single-hop single-type relationship patterns, no
malformed (undirected) relationships, WHERE atoms able to decide outside a WHERE (constant
comparisons and IN lists; a bare variable against a constant is the relationship's own `id`),
RETURN of variables / the relationship `id`, with DISTINCT / ORDER BY / SKIP / LIMIT.

Stage B adds variable-length edges of a single type (any direction) and atom placement after the
clause that binds the atom's variables.

  --pool    cqplus.jsonl of the v3 CQ+ classifier (families are intent families of the pool)
  --labels  the v2 export manifest (<graph>.labels.json)
  --out     DIRECTORY: round-<family>.json for every family with >= K compiled candidates,
            coverage.json with the per-reason refusal counts
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
  | (?P<op><>|<=|>=|\.\.|[-+*/(),.\[\]{}<>:]|\|)
  | (?P<bad>.)
""", re.X)
CLAUSE_HEADS = ('MATCH', 'OPTIONAL', 'WITH', 'RETURN', 'UNWIND', 'CALL', 'UNION', 'CREATE', 'MERGE',
                'SET', 'DELETE', 'DETACH', 'REMOVE', 'FOREACH', 'LOAD', 'USE')
COMPARE = ('<', '>', '<=', '>=', '=', '<>')
ALIAS = {'Message': ['Post', 'Comment']}


class Refuse(Exception):
    pass


def need(cond, reason):
    if not cond:
        raise Refuse(reason)


def lex(text):
    out, i = [], 0
    while i < len(text):
        m = TOKEN.match(text, i)
        if not m:
            raise Refuse('lex')
        i = m.end()
        if m.lastgroup == 'ws':
            continue
        out.append((m.lastgroup, m.group(m.lastgroup)))
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
            cur = [('word', 'OPTIONAL'), toks[i + 1]]
            depth = 0
            i += 2
            continue
        if kind == 'word' and v in CLAUSE_HEADS and depth == 0:
            if cur:
                out.append(cur)
            cur = [toks[i]]
            i += 1
            continue
        if v in ('(', '[', '{'):
            depth += 1
        elif v in (')', ']', '}'):
            depth -= 1
        cur.append(toks[i])
        i += 1
    if cur:
        out.append(cur)
    return out


def top_index(toks, words):
    """Index of the first top-level word in `words` (-1 when none)."""
    depth = 0
    for j, t in enumerate(toks):
        if t[1] in ('(', '[', '{'):
            depth += 1
        elif t[1] in (')', ']', '}'):
            depth -= 1
        elif depth == 0 and t[0] == 'word' and t[1] in words:
            return j
    return -1


def const(tok, where):
    kind, v = tok
    if kind == 'num':
        need(re.fullmatch(r'-?\d+', v) is not None, 'const:' + where)
        return int(v)
    if kind == 'str':
        body = v[1:-1]
        if '\\' in body:
            body = body.encode('utf-8').decode('unicode_escape')
        return body
    if kind == 'word' and v in ('true', 'false'):
        return v == 'true'
    raise Refuse('const:' + where)


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
            need(v['kind'] == kind, 'varkind:' + name)
            return self.vidx[name]
        self.vidx[name] = len(self.vars)
        self.vars.append({'name': name, 'kind': kind, 'labels': [], 'props': [], 'appear': None})
        return self.vidx[name]


class Builder:
    def __init__(self, manifest, stage):
        self.stored = [l for l in manifest['vertex_labels'] if not l.startswith(('OUT_', 'IN_'))]
        self.types = list(manifest['edge_labels'])
        self.stage = stage          # 'B' or 'A'
        self.q = Q()
        self.names = {}             # bare names that RETURN may reference (relationship ids)
        self.rel_seq = 0

    # ------------------------------------------------------------------ patterns
    def labels_of(self, names):
        out = set()
        for n in names:
            for x in ALIAS.get(n, [n]):
                need(x in self.stored, 'label:' + x)
                out.add(x)
        return sorted(out)

    def types_of(self, names):
        out = set()
        for n in names:
            for x in ALIAS.get(n, [n]):
                need(x in self.types, 'type:' + x)
                out.add(x)
        return sorted(out)

    def anonymous_node(self):
        k = sum(1 for v in self.q.vars if v['name'].startswith('_an'))
        name = '_an%d' % k
        while name in self.q.vidx:
            k += 1
            name = '_an%d' % k
        return name

    def node(self, toks, i):
        need(toks[i][1] == '(', 'node_start')
        j = close_of(toks, i)
        inner = toks[i + 1:j]
        k = 0
        name, labels, props = None, [], []
        if k < len(inner) and inner[k][0] == 'word':
            name = inner[k][1]
            k += 1
        while k + 1 < len(inner) and inner[k][1] == ':' and inner[k + 1][0] == 'word':
            labels.append(inner[k + 1][1])
            k += 2
        if k < len(inner) and inner[k][1] == '{':
            e = close_of(inner, k, '{', '}')
            props = self.map_list(inner[k + 1:e])
            k = e + 1
        need(k == len(inner), 'node_tail')
        if name is None:
            name = self.anonymous_node()
        vid = self.q.var(name, 'vertex')
        v = self.q.vars[vid]
        for x in self.labels_of(labels):
            if x not in v['labels']:
                v['labels'].append(x)
        v['props'].extend(props)
        return j + 1, vid

    @staticmethod
    def map_list(inner):
        out = []
        for part in depth_split(inner):
            if not part:
                continue
            need(len(part) == 3 and part[1][1] == ':', 'map_key')
            out.append([part[0][1], const(part[2], 'map')])
        return out

    def rel(self, toks, i):
        back = toks[i][1] == '<'
        i += 2 if back else 1
        var, types, lo, hi, props, many = None, [], 1, 1, [], False
        if i < len(toks) and toks[i][1] == '[':
            j = close_of(toks, i, '[', ']')
            inner = toks[i + 1:j]
            k = 0
            if k < len(inner) and inner[k][0] == 'word' and not (k + 1 < len(inner) and inner[k + 1][1] == '.'):
                var = inner[k][1]
                k += 1
            while k < len(inner) and inner[k][1] == ':':
                k += 1
                need(k < len(inner) and inner[k][0] == 'word', 'rel_type')
                types.append(inner[k][1])
                k += 1
            if k < len(inner) and inner[k][1] == '*':
                many = True
                need(self.stage != 'A', 'variable_length')
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
                elif k < len(inner) and inner[k][1] == '*':
                    raise Refuse('rel_multi_star')
            if k < len(inner) and inner[k][1] == '{':
                e = close_of(inner, k, '{', '}')
                props = self.map_list(inner[k + 1:e])
                k = e + 1
            need(k == len(inner), 'rel_tail')
            i = j + 1
        need(i < len(toks), 'rel_end')
        if toks[i][1] == '-' and i + 1 < len(toks) and toks[i + 1][1] == '>':
            i += 2
            need(not back, 'rel_both_arrows')
            any_dir = False
        elif toks[i][1] == '-':
            # CygPerf writes every (a)-[:T]-(b) as an undirected relationship
            any_dir = True
            raise Refuse('rel_undirected')
        else:
            raise Refuse('rel_end')
        if len(types) > 1:
            raise Refuse('rel_type_alt')
        return i, {'types': self.types_of(types), 'any_dir': any_dir, 'lo': lo, 'hi': hi,
                   'var': var or '', 'props': props}

    def pattern(self, toks, clause):
        added = []
        for part in depth_split(toks):
            if not part:
                continue
            need(part[0][0] == 'word' or part[0][1] == '(', 'pattern_lead')
            i = 0
            if part[0][0] == 'word' and i + 1 < len(part) and part[i + 1][1] == '=':
                raise Refuse('named_path')
            if part[0][0] == 'word' and part[1][1] != '(':
                raise Refuse('bare_label_pattern')
            i, prev = self.node(part, i)
            while i < len(part) and part[i][1] in ('-', '<'):
                i, e = self.rel(part, i)
                i, nxt = self.node(part, i)
                if not e['var']:
                    name = ('_r_' + e['types'][0]) if len(e['types']) == 1 and not many_hops(e) else '_rel'
                    while name in self.q.vidx:
                        name = '%s_%d' % ('_rel' if name.startswith('_rel') else name[:2], self.rel_seq)
                        self.rel_seq += 1
                    e['var'] = name
                e.update({'from': prev, 'to': nxt, 'clause': clause, 'path': ''})
                self.q.edges.append(e)
                added.append(len(self.q.edges) - 1)
                prev = nxt
            need(i >= len(part), 'pattern_tail')
        return added

    # ------------------------------------------------------------------ WHERE atoms
    def where(self, toks, clause, post=False):
        atoms = []
        for g in depth_split(toks, 'AND'):
            need(g, 'where_empty')
            need(len(depth_split(g, 'OR')) == 1, 'where_or')
            atoms.append(self.term(g, clause, post))
        return atoms

    def term(self, t, clause, post):
        neg = False
        if t and t[0][1] == 'NOT':
            raise Refuse('where_not')
        need(t[0][0] == 'word', 'where_lead')
        var = t[0][1]
        need(var in self.q.vidx, 'where_name:' + var)
        vid = self.q.vidx[var]
        k = 1
        prop = ''
        if k + 1 < len(t) and t[k][1] == '.':
            prop = t[k + 1][1]
            k += 2
        if k >= len(t):
            raise Refuse('where_bare_var')
        op = t[k][1]
        if op == 'IS':
            need(prop != '', 'where_is_entity')
            if k + 2 < len(t) and t[k + 1][1] == 'NOT' and t[k + 2][1] == 'NULL':
                return {'var': vid, 'prop': prop, 'op': 'IS NOT NULL', 'consts': [], 'clause': clause,
                        'or': -1, 'negated': neg, 'post': post}
            need(k + 1 < len(t) and t[k + 1][1] == 'NULL', 'where_is')
            return {'var': vid, 'prop': prop, 'op': 'IS NULL', 'consts': [], 'clause': clause,
                    'or': -1, 'negated': neg, 'post': post}
        if k + 1 < len(t) and t[k + 1][0] == 'word':
            # `other <> p` (an entity comparison) or `creator = friend` (a relationship id on
            # the right): outside the stage
            raise Refuse('where_var_rhs')
        if op == '<>':
            raise Refuse('where_neq')
        if op == 'IN':
            need(k + 1 < len(t) and t[k + 1][1] == '[', 'where_in')
            c = close_of(t, k + 1, '[', ']')
            need(c == len(t) - 1, 'where_in_tail')
            vals = [const(x[0], 'in') for x in depth_split(t[k + 2:c]) if x]
            need(vals, 'where_in_empty')
            return {'var': vid, 'prop': prop, 'op': 'IN', 'consts': vals, 'clause': clause,
                    'or': -1, 'negated': neg, 'post': post}
        need(op in COMPARE, 'where_op:' + op)
        rhs = t[k + 1:]
        need(len(rhs) == 1 and rhs[0][0] in ('num', 'str', 'word'), 'where_expr')
        if rhs[0][0] == 'word' and rhs[0][1] not in ('true', 'false'):
            raise Refuse('where_var_rhs')   # unreachable for words: refused above
        need(prop != '', 'where_entity_rhs')
        return {'var': vid, 'prop': prop, 'op': op, 'consts': [const(rhs[0], 'op')], 'clause': clause,
                'or': -1, 'negated': neg, 'post': post}

    # ------------------------------------------------------------------ projection
    def item(self, toks, i):
        n = len(toks)
        kind, v = toks[i]
        if v == '(':
            raise Refuse('expr_item')
        if kind != 'word':
            raise Refuse('expr_item')
        if self.item_end(toks, i) > i + 1 and toks[i + 1][1] not in ('.', ','):
            raise Refuse('expr_item')
        if v in self.q.vidx:
            j = i + 1
            prop = ''
            if j + 1 < n and toks[j][1] == '.' and toks[j + 1][0] == 'word' and toks[j + 1][1] not in CLAUSE_HEADS:
                prop = toks[j + 1][1]
                j += 2
            else:
                need(j >= n or toks[j][1] in (',', 'ORDER', 'SKIP', 'LIMIT', 'AS'), 'expr_item')
            need(not self.q.vars[self.q.vidx[v]]['kind'] == 'edge' or prop in ('id',), 'rel_item')
            return {'kind': 'key', 'var': v, 'prop': prop, 'label': v + ('.' + prop if prop else '')}, j
        if v in self.names:
            return {'kind': 'key', 'var': self.names[v], 'prop': 'id', 'label': v + '.id'}, i + 1
        return {'kind': 'key', 'var': None, 'prop': v, 'label': v}, i + 1

    @staticmethod
    def item_end(toks, i):
        depth = 0
        for j in range(i, len(toks)):
            if toks[j][1] in ('(', '['):
                depth += 1
            elif toks[j][1] in (')', ']'):
                depth -= 1
            elif depth == 0 and toks[j][1] == ',':
                return j
        return len(toks)

    def items_list(self, toks):
        items = []
        distinct = bool(toks and toks[0][1] == 'DISTINCT')
        k = 1 if distinct else 0
        n = len(toks)
        while k < n:
            if toks[k][0] == 'word' and toks[k][1] in ('ORDER', 'LIMIT', 'SKIP', 'WHERE') and \
                    not (k + 1 < n and toks[k + 1][1] == '.'):
                break
            if toks[k][1] == ',':
                k += 1
                continue
            start = k
            it, k = self.item(toks, k)
            if k < n and toks[k][1] == 'AS':
                it['label'] = toks[k + 1][1]
                k += 2
            if it.get('var') is not None and it['var'] in self.q.vidx and self.q.vars[self.q.vidx[it['var']]]['kind'] == 'edge':
                need(it['prop'] == 'id', 'rel_alias')
            items.append(it)
        return items, distinct, k


def many_hops(e):
    return e['lo'] != 1 or e['hi'] != 1


def build(text, qid, family, template, manifest, stage):
    b = Builder(manifest, stage)
    toks = lex(text)
    clauses = split_clauses(toks)
    out_clauses = []
    order_items, order_desc = [], []
    skip_n, limit_n = None, None
    for ci, c in enumerate(clauses):
        head = c[0][1]
        if head in ('MATCH', 'OPTIONAL'):
            body = c[1:]
            if head == 'OPTIONAL':
                need(body and body[0][1] == 'MATCH', 'optional_head')
                body = body[1:]
                raise Refuse('optional_match')
            wpos = top_index(body, ('WHERE',))
            edges = b.pattern(body if wpos < 0 else body[:wpos], ci)
            need(edges, 'empty_pattern')
            where = b.where(body[wpos + 1:], ci) if wpos >= 0 else []
            out_clauses.append({'kind': 'MATCH', 'edges': edges, 'where': where})
        elif head == 'WITH':
            raise Refuse('with')
        elif head == 'RETURN':
            need(ci == len(clauses) - 1, 'after_return')
            body = c[1:]
            end = top_index(body, ('ORDER', 'LIMIT', 'SKIP'))
            if end < 0:
                end = len(body)
            items, distinct, _ = b.items_list(body[:end])
            need(items, 'empty_return')
            rest = body[end:]
            base = len(b.q.items)
            for it in items:
                b.q.items.append(it)
            out_clauses.append({'kind': 'RETURN', 'items': list(range(base, base + len(items))),
                                'distinct': distinct})
            r = 0
            while r < len(rest):
                if rest[r][1] == 'ORDER':
                    need(r + 1 < len(rest) and rest[r + 1][1] == 'BY', 'order')
                    r += 2
                    while r < len(rest) and rest[r][1] not in ('SKIP', 'LIMIT'):
                        if rest[r][1] == ',':
                            r += 1
                            continue
                        need(rest[r][0] == 'word', 'order_expr')
                        ref = self_resolve(b, rest[r][1])
                        r += 1
                        if not (r >= len(rest) or rest[r][1] in ('ASC', 'DESC', 'ASCENDING', 'DESCENDING', ',')):
                            raise Refuse('order_expr:' + rest[r][1])
                        desc = False
                        if r < len(rest) and rest[r][1] in ('ASC', 'DESC', 'ASCENDING', 'DESCENDING'):
                            desc = rest[r][1].startswith('DESC')
                            r += 1
                        order_items.append(ref)
                        order_desc.append(desc)
                    continue
                if rest[r][1] == 'SKIP':
                    skip_n = const(rest[r + 1], 'skip')
                    r += 2
                    continue
                if rest[r][1] == 'LIMIT':
                    limit_n = const(rest[r + 1], 'limit')
                    r += 2
                    continue
                raise Refuse('tail:' + rest[r][1])
        elif head == 'OPTIONAL':
            raise Refuse('optional_match')
        else:
            raise Refuse('clause:' + head)
    need(out_clauses and out_clauses[-1]['kind'] == 'RETURN', 'no_return')
    ret = out_clauses[-1]
    tail = []
    for it, desc in zip(order_items, order_desc):
        idx = None
        for j in ret['items']:
            x = b.q.items[j]
            if x['kind'] == 'key' and x['var'] is not None and x['var'] == it['var'] and x['prop'] == it['prop']:
                idx = j
                break
        if idx is None:
            raise Refuse('order_not_returned')
        tail.append(idx)
    if tail:
        out_clauses.append({'kind': 'ORDER_BY', 'keys': tail, 'desc': order_desc})
    if skip_n is not None:
        out_clauses.append({'kind': 'SKIP', 'n': skip_n})
    if limit_n is not None:
        out_clauses.append({'kind': 'LIMIT', 'n': limit_n})
    return {'id': qid, 'family': family, 'template': template, 'text': text,
            'clauses': out_clauses,
            'variables': [{'name': v['name'], 'kind': v['kind'], 'labels': v['labels'],
                           'props': [[p, x] for p, x in v['props']]} for v in b.q.vars],
            'edges': [{'from': b.q.vars[e['from']]['name'], 'to': b.q.vars[e['to']]['name'],
                       'types': e['types'], 'dir': '->' if not e['any_dir'] else '-', 'lo': e['lo'],
                       'hi': e['hi'], 'var': e['var'], 'props': [[p, x] for p, x in e['props']],
                       'clause': e['clause'], 'path': ''} for e in b.q.edges],
            'where': [{'var': b.q.vars[a['var']]['name'], 'prop': a['prop'], 'op': a['op'],
                       'consts': a['consts'], 'clause': a['clause'], 'or': a['or'],
                       'negated': a['negated'], 'post': a['post']} for a in b.q.atoms],
            'items': [{'kind': 'key', 'func': '', 'distinct': False,
                       'var': (it['var'] if it.get('var') is not None else ''),
                       'prop': it['prop'], 'arg_var': '', 'arg_prop': '', 'label': it['label']}
                      for it in b.q.items]}


def self_resolve(b, name):
    if name in b.q.vidx:
        return {'kind': 'key', 'var': name, 'prop': ''}
    if name in b.names:
        return {'kind': 'key', 'var': b.names[name], 'prop': 'id'}
    raise Refuse('order_unknown:' + name)


def shape_of(q):
    """The edit dimension of a compiled candidate: what its round varies."""
    layers = [c for c in q['clauses'] if c['kind'] == 'MATCH']
    sh = []
    for e in q['edges']:
        if e['lo'] != 1 or e['hi'] != 1:
            return 'vl'
    if any(a['op'] in ('IN', 'IS NULL', 'IS NOT NULL') for a in q['where']):
        return 'pred'
    edge_types = set(t for e in q['edges'] for t in e['types'])
    if 'HAS_TAG' in edge_types or 'HAS_TYPE' in edge_types:
        return 'tag'
    if len(layers) > 1:
        return 'multi'
    if any(a['op'] in ('IN', 'IS NULL', 'IS NOT NULL') for a in q['where']):
        return 'pred'
    return 'pattern'


def families_by_intent(plans):
    """(template, shape) -> [candidate keys] in pool order, split by the pool's anchor when
    the template's texts use more than one."""
    fam = collections.OrderedDict()
    for key in plans:
        pl = plans[key]
        sh = shape_of(pl)
        if sh in ('vl',):
            continue
        fam.setdefault((pl['template'], sh), set()).add(pl['text'])
    return fam


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pool', default=os.path.join(POOL_DIR, 'cqplus-v3', 'cqplus.jsonl'))
    ap.add_argument('--labels', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--stage', default='B', choices=('A', 'B'))
    ap.add_argument('--k', type=int, default=8)
    ap.add_argument('--families', default='', help='only these families (comma separated)')
    ap.add_argument('--log-refusals', action='store_true', help='write refused.jsonl in --out')
    ap.add_argument('--intent', action='store_true',
                    help='rounds = (template, edit shape) of the compiled candidates, pool order')
    args = ap.parse_args()
    manifest = json.load(open(args.labels))
    recs = [json.loads(l) for l in open(args.pool)]
    only = set(x for x in args.families.split(',') if x)
    plans, why, shapes, all_plans = {}, collections.Counter(), {}, {}
    attempts = {}
    texts = 0
    for r in recs:
        if not r['in_cqplus']:
            continue
        if r.get('exec_ok') is False:
            why['not_executed'] += 1
            continue
        if only and r['family'] not in only:
            continue
        if any(a['text'] == r['text'] for a in attempts.get(r['family'], [])):
            continue
        attempts.setdefault(r['family'], []).append(r)
        texts += 1
        try:
            pl = build(r['text'], '%s#%d' % (r['family'], r['candidate']), r['family'], r['template'], manifest,
                       args.stage)
            plans[(r['family'], r['candidate'])] = pl
            shapes[pl['id']] = shape_of(pl)
            all_plans[(r['family'], r['candidate'])] = pl
        except Refuse as ex:
            if str(ex) == 'variable_length':
                why['variable_length'] += 1
            why[str(ex).split(':')[0] + (':' + str(ex).split(':', 1)[1] if ':' in str(ex) else '')] += 1
            if args.log_refusals:
                with open(os.path.join(args.out, 'refused.jsonl'), 'a') as f:
                    f.write(json.dumps({'id': '%s#%d' % (r['family'], r['candidate']), 'reason': str(ex),
                                        'text': r['text']}) + '\n')
        except Exception as ex:   # a compiler bug on an unusual text: counted, never dropped
            why['internal:' + type(ex).__name__] += 1
    os.makedirs(args.out, exist_ok=True)
    rounds = []
    if args.intent:
        groups = collections.OrderedDict()
        for key in plans:
            pl = plans[key]
            sh = shape_of(pl)
            if sh == 'vl':
                continue
            groups.setdefault((pl['template'], sh), []).append(pl)
        for (tpl, sh), pls in groups.items():
            if len(pls) < args.k:
                continue
            rounds.append({'family': 'ldbc_sf1_%s_%s' % (tpl, sh), 'template': tpl,
                           'queries': pls[:args.k]})
            for pl in pls:
                plans.pop((pl['family'], int(pl['id'].split('#')[1])), None)
        os.makedirs(args.out, exist_ok=True)
        for rd in rounds:
            with open(os.path.join(args.out, 'round-%s.json' % rd['family']), 'w') as f:
                json.dump(rd, f)
        by_tpl = collections.Counter(pl["template"] for k2, pl in all_plans.items())
        summary = {'stage': args.stage, 'texts': texts, 'compiled': sum(len(r['queries']) for r in rounds),
                   'compiled_by_template': dict(by_tpl.most_common()), 'refused': dict(why.most_common()),
                   'groups': {t + '/' + s: len(p) for (t, s), p in groups.items()},
                   'rounds': [{'family': rd['family'], 'template': rd['template'],
                               'members': [q['id'] for q in rd['queries']]} for rd in rounds]}
        with open(os.path.join(args.out, 'coverage.json'), 'w') as f:
            json.dump(summary, f, indent=1)
        print(json.dumps({'texts': texts, 'groups': summary['groups'],
                          'rounds': [r['family'] for r in rounds], 'refused': dict(why.most_common(30))}, indent=1))
        return 0
    for family in sorted({k[0] for k in plans}):
        cands = sorted(k[1] for k in plans if k[0] == family)
        if len(cands) >= args.k:
            rounds.append({'family': family, 'template': plans[(family, cands[0])]['template'],
                           'queries': [plans[(family, c)] for c in cands[:args.k]]})
            for c in cands[args.k:]:
                del plans[(family, c)]
    for rd in rounds:
        with open(os.path.join(args.out, 'round-%s.json' % rd['family']), 'w') as f:
            json.dump(rd, f)
    by_tpl = collections.Counter(pl['template'] for pl in plans.values())
    summary = {'stage': args.stage, 'texts': texts, 'compiled': len(plans),
               'compiled_by_template': dict(by_tpl.most_common()),
               'refused': dict(why.most_common()),
               'rounds': [{'family': rd['family'], 'template': rd['template'],
                           'members': [q['id'] for q in rd['queries']]} for rd in rounds]}
    with open(os.path.join(args.out, 'coverage.json'), 'w') as f:
        json.dump(summary, f, indent=1)
    print(json.dumps({'texts': texts, 'compiled': len(plans), 'rounds': len(rounds),
                      'refused': dict(why.most_common(30))}, indent=1))


if __name__ == '__main__':
    sys.exit(main())
