// ECHO v3: in-memory LDBC graph with entities, typed directed relationships and property columns.
//
// The sidecar directory written by tools/v3_sidecar holds
//   meta.json     schema, counts, source SHAs of the CSVs and of the v2 vertex index
//   vertices.bin  per vertex: <u32 label><u32 property row>
//   edges.bin     per relationship row: <u32 src><u32 dst><u32 type><u32 row><u32 dir>; every relationship
//                 is stored twice (once per direction, dir = 1 on the CSV direction) and
//                 indexed by its source
//   props.bin     per label column / per type column: a row store (8-byte key, 9-byte int cell or
//                 1 + 4 + bytes string cell) followed by a one-bit-per-row null bitmap
//
// Vertices are the v2 export's vertex indices under their stored label (`<graph>.ids.json`).
// Relationship rows are the CSV sequence numbers of the relationship's table; v2's graph and
// ids sidecars are read-only inputs to tools/v3_sidecar.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "v3_ir.h"

namespace v3 {

// A read-only array view.  A legacy sidecar without a shared graph cache owns the vector; a
// cache-backed store points straight at one MAP_SHARED mapping and therefore keeps no private
// copy of the graph arrays.
template <typename T>
class GraphArray {
public:
    GraphArray() = default;

    void assign(size_t n, const T& value = T()) {
        auto a = std::make_shared<std::vector<T>>(n, value);
        p_ = a->data();
        n_ = n;
        own_ = std::move(a);
    }
    void assign(std::vector<T>&& v) {
        auto a = std::make_shared<std::vector<T>>(std::move(v));
        p_ = a->data();
        n_ = a->size();
        own_ = std::move(a);
    }
    void reserve(size_t n) {
        if (!own_) own_ = std::make_shared<std::vector<T>>();
        own_->reserve(n);
        p_ = own_->data();
        n_ = own_->size();
    }
    void resize(size_t n) {
        if (!own_) own_ = std::make_shared<std::vector<T>>();
        own_->resize(n);
        p_ = own_->data();
        n_ = own_->size();
    }
    void push_back(const T& value) {
        if (!own_) own_ = std::make_shared<std::vector<T>>();
        own_->push_back(value);
        p_ = own_->data();
        n_ = own_->size();
    }
    void view(const T* p, size_t n) {
        own_.reset();
        p_ = p;
        n_ = n;
    }

    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    const T* data() const { return p_; }
    T* data() { return const_cast<T*>(p_); }
    const T* begin() const { return p_; }
    const T* end() const { return p_ + n_; }
    T* begin() { return const_cast<T*>(p_); }
    T* end() { return const_cast<T*>(p_) + n_; }
    const T& operator[](size_t i) const { return p_[i]; }
    T& operator[](size_t i) { return const_cast<T&>(p_[i]); }

private:
    std::shared_ptr<std::vector<T>> own_;
    const T* p_ = nullptr;
    size_t n_ = 0;
};

// Typed adjacency view: for all types, or one type; both directions from the same rows.
struct AdjView {
    const GraphArray<uint32_t>* off = nullptr;   // degree offsets, size nv + 1
    const GraphArray<uint32_t>* nbr = nullptr;   // neighbour ids, size ne
    const GraphArray<uint8_t>* dir = nullptr;    // 1: the CSV direction of the relationship, 0: its reverse
};

class GraphStore {
public:
    // Loads all arrays and maps the property store (read-only).  With `meta_only`, only the
    // schema, counts and label cardinalities are read; that is enough for --plan-only and does
    // not touch props.bin (67 GB on SF10).
    bool load(const std::string& dir, std::string* err, bool meta_only = false);

    const Schema& schema() const { return schema_; }
    uint32_t num_vertices() const { return nv_; }
    uint64_t num_edges() const { return ne_; }
    uint64_t num_rels() const { return nrel_; }

    uint32_t label(uint32_t v) const { return vlabel_[v]; }
    // Vertices of a label, in ascending id.
    const GraphArray<uint32_t>& vertices_of(uint32_t label) const { return lbl_vlist_[label]; }
    AdjView all() const { return AdjView{&off_all_, &nbr_all_, &dir_all_}; }
    // Degree in the all-relationship view (the adjacency the masked walk scans).
    uint32_t degree(uint32_t v) const { return off_all_[v + 1] - off_all_[v]; }
    // Neighbours of v as (neighbour, relationship row), sorted by neighbour id.  A close edge has
    // both endpoints bound, so its test is a membership lookup on the more selective end, not a
    // scan of the other end's fan-out.  The index is built lazily per vertex and capped; nullptr
    // means the cap is reached and the caller keeps the linear scan.
    const std::vector<std::pair<uint32_t, uint32_t>>* sorted_adj(uint32_t v) const;
    AdjView typed(uint32_t type) const {
        if (!typed_built_[type]) build_type(type);
        return AdjView{&off_type_[type], &nbr_type_[type], &dir_type_[type]};
    }
    const GraphArray<uint32_t>& all_rels() const { return rel_all_; }
    const GraphArray<uint32_t>& typed_rels(uint32_t type) const {
        if (!typed_built_[type]) build_type(type);
        return rel_type_[type];
    }
    uint32_t all_rel(uint32_t k) const { return rel_all_[k]; }
    // Source vertex of a relationship row (it is stored once per direction).
    uint32_t rel_src(uint32_t rel) const { return esrc_row_[rel]; }
    // Relationship row of the t-th typed neighbour of v (neighbours are ascending by id).
    uint32_t typed_rel(uint32_t type, uint32_t v, uint32_t t) const {
        if (!typed_built_[type]) build_type(type);
        return rel_type_[type][off_type_[type][v] + t];
    }
    uint32_t edge_type(uint32_t rel) const { return etype_[rel]; }
    // The property row of a relationship: both directed copies share it, so it identifies the
    // relationship independently of which direction the walk took.
    uint32_t edge_row(uint32_t rel) const { return erow_[rel]; }
    uint8_t edge_dir(uint32_t rel) const { return edir_[rel]; }

    // Property value of an entity; `is_null` set when absent or NULL.
    Value vertex_prop(uint32_t v, uint32_t label, uint32_t col, bool* is_null) const;
    Value edge_prop(uint32_t rel, uint32_t type, uint32_t col, bool* is_null) const;
    // Equality of one stored column value with a constant, without constructing the Value (a
    // string column was copied into a fresh std::string per test, which dominated the label
    // class scans of the text-to-Cypher rounds: 32k vertices x 3 members = 15 ms).
    bool vertex_prop_eq(uint32_t v, uint32_t label, uint32_t col, const Value& c) const;
    bool edge_prop_eq(uint32_t rel, uint32_t type, uint32_t col, const Value& c) const;
    // Equality anchor index: (label, column) -> vertices sorted by value hash.  A constant
    // property anchor (`Person {name: "..."}` or `WHERE p.name = "..."`) otherwise scans the
    // whole label class and tests the predicate per vertex, which was 15 ms for 32k vertices
    // x 3 candidates on the text-to-Cypher company graph.  The index is warmed at load, for the
    // per-query executor and the masked planner alike, so neither path pays it inside a cell.
    void warm_prop_index(uint32_t label, uint32_t col) const;
    void prop_lookup(uint32_t label, uint32_t col, const Value& value,
                     std::vector<uint32_t>* out) const;
    // Column position of `prop` in a label / type; -1 when the property does not exist there.
    int32_t label_col(uint32_t label, const std::string& prop) const;
    int32_t type_col(uint32_t type, const std::string& prop) const;

    // The `id` property as an int64.
    int64_t vertex_id(uint32_t v) const;
    int64_t edge_id(uint32_t rel) const;
    // Eagerly built, read-only per-label id -> vertex index (the label's `id` property
    // column).  It is loaded with the graph and lives as long as it; an anchored root is one
    // lookup instead of a scan of the label class.
    struct IdEntry {
        int64_t id = 0;
        uint32_t v = 0;
    };
    const GraphArray<IdEntry>& id_index(uint32_t label) const;
    bool lookup_vertex(uint32_t label, int64_t id, uint32_t* v) const;
    // Map (without reading) the property columns a query can touch, so a forked cell
    // inherits the mappings instead of paying one mmap per cell for its RETURN columns.
    void warm_query(const Query& q) const;

    bool validate(std::string* err) const;
    // Write the immutable graph arrays next to the sidecar as graph.cache.bin/meta.json.  The
    // writer uses a legacy (private-array) load and is only called by tools/v3_cache.
    bool write_cache(const std::string& dir, std::string* err);

private:
    void build_views();
    Value col_value(uint32_t row, uint32_t key, uint32_t col, bool* is_null) const;
    bool col_eq(uint32_t row, uint32_t key, uint32_t col, const Value& c) const;
    bool col_hash(uint32_t row, uint32_t key, uint32_t col, uint64_t* h) const;
    struct PropEntry {
        uint64_t h;
        uint32_t v;
    };
    struct PropIndex {
        std::vector<PropEntry> e;
        bool built = false;
    };
    mutable std::unordered_map<uint64_t, PropIndex> prop_idx_;
    Schema schema_;
    uint32_t nv_ = 0;
    uint64_t ne_ = 0, nrel_ = 0;
    GraphArray<uint32_t> vlabel_, vrow_;
    GraphArray<uint32_t> etype_, esrc_row_, edst_row_, erow_;
    GraphArray<uint8_t> edir_;
    GraphArray<uint32_t> lbl_off_, lbl_vals_;
    std::vector<GraphArray<uint32_t>> lbl_vlist_;
    GraphArray<uint32_t> off_all_, nbr_all_, rel_all_;
    GraphArray<uint8_t> dir_all_;
    GraphArray<uint32_t> type_off_concat_, type_nbr_concat_, type_rel_concat_;
    GraphArray<uint8_t> type_dir_concat_;
    GraphArray<uint64_t> type_base_;
    mutable std::vector<GraphArray<uint32_t>> off_type_, nbr_type_, rel_type_;
    mutable std::vector<GraphArray<uint8_t>> dir_type_;
    mutable std::vector<uint8_t> typed_built_;
    GraphArray<uint64_t> id_off_;
    std::vector<GraphArray<IdEntry>> id_index_;
    void* cache_map_ = nullptr;
    size_t cache_bytes_ = 0;
    bool build_id_index(std::string* err);
    bool load_shared_cache(const std::string& dir, std::string* err);
    void bind_shared_views();
    // Lazily sorted adjacency of one side, used by the close-edge lookup from the lower-degree
    // endpoint (main34).  The id index above is the main36 point-lookup structure.
    mutable std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> sorted_adj_;
    mutable size_t sorted_entries_ = 0;
    void build_type(uint32_t type) const;

    struct ColStore {
        mutable const uint8_t* data = nullptr;
        uint64_t rows = 0, width = 0, off = UINT64_MAX;   // off: byte offset in props.bin (props.json)
        uint64_t len = 0;                                  // bytes of this block
        mutable bool mapped = false;
    };
    mutable std::vector<ColStore> cols_;
    size_t map_bytes_ = 0;
    std::string map_path_;
    void map_col(const ColStore& c) const;
};

}  // namespace v3
