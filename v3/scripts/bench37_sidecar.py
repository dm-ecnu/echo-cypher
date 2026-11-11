#!/usr/bin/env python3
"""main37: build v3 sidecars for the two LLM-era benchmarks from their raw exports.

Two encodings, both read by v3/src/v3_graph.cpp:

  simplekg  CypherBench released `*_simplekg.json` (entities/relations).
  t2gq      Text2GraphQuery-Bench node/relationship CSVs exported by the official driver.

The sidecar format is the LDBC one (tools/v3_sidecar.cpp):
  vertices.bin  <u32 label><u32 row> per vertex
  edges.bin     <u32 src><u32 dst><u32 type><u32 row><u32 dir> per direction
  props.bin     per (label, col) and (L + type, col): rows * width then a one-bit-per-row null map
  meta.json / props.json

Type convention (same as the LDBC loader): INTEGER/INT -> int64, BOOLEAN -> 0/1,
STRING/STR and DATE/DATETIME -> string cells, FLOAT/DOUBLE -> string cells (the v3 Value is
int/string only; the plan compiler refuses numeric predicates on them, so they are a coverage
fact, not an approximation), LIST/array properties are not stored and are likewise refused.

The converter writes `counts.json` with per-label and per-type counts of the source and of the
sidecar, plus the SHA256 of every input file.
"""
import argparse
import collections
import csv
import hashlib
import io
import json
import os
import struct
import sys

U32 = struct.Struct('<I')
I64 = struct.Struct('<q')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def norm_type(t):
    t = str(t).upper()
    if t in ('INTEGER', 'INT', 'LONG'):
        return 'int'
    if t in ('BOOLEAN', 'BOOL'):
        return 'bool'
    if t in ('DOUBLE', 'FLOAT', 'REAL'):
        return 'float'
    if t in ('DATE', 'DATETIME', 'LOCALDATETIME', 'LOCALDATE', 'TIME'):
        return 'date'
    if t.startswith('LIST') or t.endswith('[]') or t.startswith('ARRAY'):
        return 'list'
    return 'str'


def cell(value, kind):
    """Return (bytes, is_null). `kind` is int/str/bool/float/date/list."""
    if value is None:
        return b'', True
    if kind == 'list':
        return b'', True
    if kind in ('int', 'bool'):
        if isinstance(value, bool):
            v = 1 if value else 0
        elif str(value).strip().lower() in ('true', 'false'):
            v = 1 if str(value).strip().lower() == 'true' else 0
        else:
            try:
                v = int(str(value).strip())
            except Exception:
                return b'', True
        return I64.pack(v), False
    s = str(value)
    if s == '':
        return b'', True
    b = s.encode('utf-8')
    return b'\x01' + U32.pack(len(b)) + b, False


class SidecarWriter:
    def __init__(self, out):
        self.out = out
        os.makedirs(out, exist_ok=True)
        self.vf = open(os.path.join(out, 'vertices.bin.part'), 'wb')
        self.ef = open(os.path.join(out, 'edges.bin.part'), 'wb')
        self.pf = open(os.path.join(out, 'props.bin.part'), 'wb')
        self.segments = []
        self.off = 0
        self.labels = []          # (name, rows, [(prop, col, kind)])
        self.types = []           # (name, [(prop, col, kind)])
        self.label_ids = {}       # name -> id
        self.type_ids = {}

    def add_label(self, name, rows, props):
        """props: list of (prop_name, kind), column order is the list order."""
        self.label_ids[name] = len(self.labels)
        self.labels.append((name, rows, [(p, i, k) for i, (p, k) in enumerate(props)]))

    def add_type(self, name, props):
        self.type_ids[name] = len(self.types)
        self.types.append((name, [(p, i, k) for i, (p, k) in enumerate(props)]))

    def write_vertices(self, rows):
        for label, row in rows:
            self.vf.write(U32.pack(label) + U32.pack(row))

    def write_edges(self, rows):
        # rows: (src, dst, type, row, dir), already sorted
        for r in rows:
            self.ef.write(struct.pack('<IIIII', *r))

    def write_block(self, key, col, values, kind):
        """values: list indexed by row; writes records + null bitmap."""
        n = len(values)
        cells = []
        width = 17
        for v in values:
            b, null = cell(v, kind)
            cells.append((b, null))
            if kind in ('str', 'float', 'date', 'list'):
                width = max(width, 8 + len(b))
        width = max(width, 8)
        bm = bytearray(n // 8 + 1)
        buf = io.BytesIO()
        for i, (b, null) in enumerate(cells):
            rec = I64.pack(i) + b
            buf.write(rec + b'\x00' * (width - len(rec)))
            if null:
                bm[i >> 3] |= (1 << (i & 7))
        buf.write(bm)
        data = buf.getvalue()
        self.pf.write(data)
        self.segments.append({'key': key, 'col': col, 'rows': n, 'width': width, 'off': self.off})
        self.off += len(data)

    def close(self, nverts, ndirs):
        self.vf.close()
        self.ef.close()
        self.pf.close()
        for a, b in (('vertices.bin', 'vertices.bin'), ('edges.bin', 'edges.bin'),
                     ('props.bin', 'props.bin')):
            os.replace(os.path.join(self.out, a + '.part'), os.path.join(self.out, b))
        meta = {
            'vertices': nverts,
            'rels': ndirs,
            'props_bytes': self.off,
            'labels': [{'name': n, 'rows': r,
                        'props': [{'name': p, 'col': c, 'type': k} for p, c, k in ps]}
                       for n, r, ps in self.labels],
            'types': [{'id': i, 'name': n,
                       'props': [{'name': p, 'col': c, 'type': k} for p, c, k in ps]}
                      for i, (n, ps) in enumerate(self.types)],
            'label_ids': [{'id': i, 'name': n} for i, (n, _, _) in enumerate(self.labels)],
            'type_ids': [{'id': i, 'name': n} for i, (n, _) in enumerate(self.types)],
        }
        with open(os.path.join(self.out, 'meta.json'), 'w') as f:
            json.dump(meta, f, indent=1)
        with open(os.path.join(self.out, 'props.json'), 'w') as f:
            json.dump({'segments': self.segments}, f)
        return meta


def write_simplekg(graph, out):
    src = os.path.abspath(graph)
    d = json.load(open(src))
    ent_schema = {e['label']: e['properties'] for e in d['schema']['entities']}
    rel_schema = {(r['label'], r['subj_label'], r['obj_label']): r['properties']
                  for r in d['schema']['relations']}
    labels = sorted({e['label'] for e in d['entities']})
    types = sorted({r['label'] for r in d['relations']})
    label_rows = collections.defaultdict(list)
    for e in d['entities']:
        label_rows[e['label']].append(e)
    by_eid = {}
    vindex = {}
    for li, lab in enumerate(labels):
        for row, e in enumerate(label_rows[lab]):
            by_eid[e['eid']] = (li, row)
            vindex[(li, row)] = e
    # vertex property columns: eid, name, then the schema properties (stable order)
    w = SidecarWriter(out)
    for li, lab in enumerate(labels):
        cols = [('eid', 'str'), ('name', 'str')]
        for p in sorted(ent_schema.get(lab, {})):
            k = norm_type(ent_schema[lab][p])
            if k == 'list':
                continue
            cols.append((p, k))
        w.add_label(lab, len(label_rows[lab]), cols)
    for ti, typ in enumerate(types):
        props = set()
        for (lab, subj, obj), ps in rel_schema.items():
            if lab != typ:
                continue
            for p in ps:
                k = norm_type(ps[p])
                if k != 'list':
                    props.add((p, k))
        w.add_type(typ, sorted(props))
    vertices = []
    for li, lab in enumerate(labels):
        for row, e in enumerate(label_rows[lab]):
            vertices.append((li, row))
    w.write_vertices(vertices)
    # property blocks
    for li, lab in enumerate(labels):
        rows = label_rows[lab]
        for ci, (p, c, k) in enumerate(w.labels[li][2]):
            vals = []
            for e in rows:
                if p == 'eid':
                    v = e.get('eid')
                elif p == 'name':
                    v = e.get('name')
                else:
                    v = e.get('properties', {}).get(p)
                vals.append(v)
            w.write_block(li, ci, vals, k)
    # relationship rows per type, then directed copies
    rel_by_type = collections.defaultdict(list)
    for r in d['relations']:
        rel_by_type[r['label']].append(r)
    edge_rows = []
    for ti, typ in enumerate(types):
        rels = rel_by_type[typ]
        for ri, r in enumerate(rels):
            a = by_eid.get(r['subj_id'])
            b = by_eid.get(r['obj_id'])
            if a is None or b is None:
                raise SystemExit('relation endpoint missing: %s' % r.get('rid'))
            srcv = a[0] * 0  # placeholder
            # vertex index = offset of label + row
            srcv = sum(len(label_rows[x]) for x in labels[:a[0]]) + a[1]
            dstv = sum(len(label_rows[x]) for x in labels[:b[0]]) + b[1]
            edge_rows.append((srcv, dstv, ti, ri, 1))
            edge_rows.append((dstv, srcv, ti, ri, 0))
        # edge property columns for this type
        props = w.types[ti][1]
        if props:
            for ci, (p, c, k) in enumerate(props):
                vals = []
                for r in rels:
                    vals.append(r.get('properties', {}).get(p))
                w.write_block(len(labels) + ti, ci, vals, k)
    edge_rows.sort()
    w.write_edges(edge_rows)
    meta = w.close(len(vertices), len(edge_rows))
    counts = {
        'source': src, 'source_sha256': sha256(src),
        'graph': os.path.basename(src), 'kind': 'cypherbench-simplekg',
        'vertices': len(vertices), 'relations': len(d['relations']), 'rel_dirs': len(edge_rows),
        'labels': {lab: len(label_rows[lab]) for lab in labels},
        'types': {typ: len(rel_by_type[typ]) for typ in types},
        'schema_labels': sorted(ent_schema), 'schema_types': sorted({r['label'] for r in d['relations']}),
        'label_props': {lab: [p for p, _, _ in w.labels[i][2]] for i, lab in enumerate(labels)},
        'type_props': {typ: [p for p, _, _ in w.types[i][1]] for i, typ in enumerate(types)},
    }
    with open(os.path.join(out, 'counts.json'), 'w') as f:
        json.dump(counts, f, indent=1)
    manifest = {'graph': counts['graph'], 'vertex_labels': labels, 'edge_labels': types,
                'aliases': {}, 'prop_types': {}}
    for lab in labels:
        manifest['prop_types'][lab] = {p: k for p, _, k in w.labels[labels.index(lab)][2]}
    for typ in types:
        manifest['prop_types']['E:' + typ] = {p: k for p, _, k in w.types[types.index(typ)][1]}
    with open(os.path.join(out, 'labels.json'), 'w') as f:
        json.dump(manifest, f, indent=1)
    return counts


def t2gq_manifest(graph, base):
    """Read the official import arguments from runtime.json for this graph."""
    rt = json.load(open(os.path.join(base, 'runtime.json')))
    for g in rt['graphs']:
        if g['graph'] == graph:
            return g
    raise SystemExit('graph not in runtime.json: ' + graph)


def write_t2gq(graph, out, base):
    """graph: t2gq_<db>; the CSVs are exported by the official driver (neo4j-v1)."""
    rt = t2gq_manifest(graph, base)
    root = rt['csv_dir']
    schema = json.load(open(os.path.join(base, 'schemas', graph + '_schema.json')))
    ent_types = {e['label']: {p: norm_type(t) for p, t in e['properties'].items()}
                 for e in schema['entities']}
    rel_types = {r['label']: {p: norm_type(t) for p, t in r.get('properties', {}).items()}
                 for r in schema['relations']}
    # map import file basename -> (kind, name)
    imports = {}
    for arg in rt['import_args']:
        parts = arg.split('=', 2)
        if len(parts) != 3:
            raise SystemExit('bad import arg: ' + arg)
        kind, name, path = parts[0].lstrip('-'), parts[1], parts[2]
        imports[os.path.basename(path)] = (kind, name)
    node_files = {}
    rel_files = collections.defaultdict(list)
    for fn, (kind, name) in imports.items():
        if kind == 'nodes':
            node_files[name] = os.path.join(root, fn)
        elif kind == 'relationships':
            rel_files[name].append(os.path.join(root, fn))
    rel_files = dict(rel_files)
    # node rows, id -> (label, row)
    labels = sorted(node_files)
    rows_by_label = {}
    id_of = {}
    for lab in labels:
        path = node_files[lab]
        with open(path, newline='') as f:
            rd = csv.reader(f)
            header = next(rd)
            ids = []
            data = []
            for row in rd:
                if not row:
                    continue
                ids.append(row[0])
                data.append(row[1:])
        rows_by_label[lab] = (header, data, ids)
        for i, x in enumerate(ids):
            if x in id_of:
                raise SystemExit('duplicate node id %s in %s' % (x, graph))
            id_of[x] = (lab, i)
    w = SidecarWriter(out)
    label_cols = {}
    # property columns: every node CSV column except the id
    for lab in labels:
        header, data, ids = rows_by_label[lab]
        cols = []
        for c, h in enumerate(header[1:]):
            name = h.rsplit(':', 1)[0]
            kind = ent_types.get(lab, {}).get(name)
            if kind is None:
                kind = norm_type(h.rsplit(':', 1)[1]) if ':' in h else 'str'
            if kind == 'list':
                continue
            cols.append((name, kind, c))
        label_cols[lab] = cols
        w.add_label(lab, len(ids), [(n, k) for n, k, _ in cols])
    types = sorted(rel_files)
    for typ in types:
        cols = [(p, k) for p, k in sorted(rel_types.get(typ, {}).items()) if k != 'list']
        w.add_type(typ, cols)
    # vertices in label order
    verts = []
    off = {}
    n = 0
    for lab in labels:
        off[lab] = n
        for _ in rows_by_label[lab][2]:
            verts.append((labels.index(lab), len(verts) - n))
        n += len(rows_by_label[lab][2])
    w.write_vertices(verts)
    for lab in labels:
        header, data, ids = rows_by_label[lab]
        for ci, (name, kind, c) in enumerate(label_cols[lab]):
            w.write_block(labels.index(lab), ci, [row[c] for row in data], kind)
    # relationship files: :START_ID(Label),:END_ID(Label)
    edge_rows = []
    per_type_counts = {}
    for ti, typ in enumerate(types):
        pairs = []
        for path in rel_files[typ]:
            with open(path, newline='') as f:
                rd = csv.reader(f)
                header = next(rd)
                for row in rd:
                    if not row:
                        continue
                    a, b = row[0], row[1]
                    if a not in id_of or b not in id_of:
                        raise SystemExit('relationship endpoint missing %s %s in %s' % (a, b, graph))
                    la, ra = id_of[a]
                    lb, rb = id_of[b]
                    pairs.append((off[la] + ra, off[lb] + rb))
        per_type_counts[typ] = len(pairs)
        for ri, (a, b) in enumerate(pairs):
            edge_rows.append((a, b, ti, ri, 1))
            edge_rows.append((b, a, ti, ri, 0))
        # no relationship property columns in the neo4j-v1 export
    edge_rows.sort()
    w.write_edges(edge_rows)
    meta = w.close(len(verts), len(edge_rows))
    counts = {
        'source_dir': os.path.abspath(root), 'kind': 'text2graphquery-neo4j-v1',
        'graph': graph, 'vertices': len(verts), 'relations': sum(per_type_counts.values()),
        'rel_dirs': len(edge_rows),
        'labels': {lab: len(rows_by_label[lab][2]) for lab in labels},
        'types': per_type_counts,
        'schema_sha256': sha256(os.path.join(base, 'schemas', graph + '_schema.json')),
        'csv_sha256': {os.path.basename(node_files[lab]): sha256(node_files[lab]) for lab in labels},
        'rel_sha256': {typ: [sha256(p) for p in rel_files[typ]] for typ in types},
        'label_props': {lab: [p for p, _, _ in w.labels[i][2]] for i, lab in enumerate(labels)},
    }
    with open(os.path.join(out, 'counts.json'), 'w') as f:
        json.dump(counts, f, indent=1)
    manifest = {'graph': graph, 'vertex_labels': labels, 'edge_labels': types,
                'aliases': {}, 'prop_types': {}}
    for lab in labels:
        manifest['prop_types'][lab] = {p: k for p, _, k in w.labels[labels.index(lab)][2]}
    for typ in types:
        manifest['prop_types']['E:' + typ] = {p: k for p, _, k in w.types[types.index(typ)][1]}
    with open(os.path.join(out, 'labels.json'), 'w') as f:
        json.dump(manifest, f, indent=1)
    return counts


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('simplekg')
    a.add_argument('--graph', required=True)
    a.add_argument('--out', required=True)
    b = sub.add_parser('t2gq')
    b.add_argument('--graph', required=True)
    b.add_argument('--base', required=True)
    b.add_argument('--out', required=True)
    args = ap.parse_args()
    if args.cmd == 'simplekg':
        c = write_simplekg(args.graph, args.out)
    else:
        c = write_t2gq(args.graph, args.out, args.base)
    print(json.dumps({'graph': c['graph'], 'vertices': c['vertices'],
                      'relations': c['relations'], 'labels': len(c['labels']),
                      'types': len(c['types'])}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
