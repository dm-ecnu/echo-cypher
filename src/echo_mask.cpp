// ECHO N4 + N5, grafted onto the vendored SubgraphMatching base.
//
// N4. The envelope pattern has one vertex per class; a class carries the *set* of its members'
// labels. The base's LDF + DPiso filter runs once on that pattern (not once per member): LDF
// builds each class candidate set from the union of the class labels through EnvDataView, and
// DPiso's three forward/backward refinement rounds prune it over the common edges. The only
// change to the base filter is the label predicate: it asks the data view whether a data vertex
// carries any label of the class (Graph::hasVertexLabel). A partial edge (one that not every
// member covering both ends has) never enters the pattern, so it only clears mask bits later.
// Every class candidate then receives a member bitmask: bit t is set when the data vertex has
// member t's label for its class vertex and its degree is at least that vertex's degree.
//
// N5. The base's GQL matching order and BuildTable candidate edge tables are built on the same
// envelope pattern and candidate lists. The walk follows that order; at each class it takes the
// candidates reachable through the edge table of every already placed common-edge neighbour,
// intersects the candidate's member mask with the live-member mask, and checks each surviving
// member's own edges to its placed vertices (including the optional/partial edges that the
// pattern does not contain). A member absent from this branch is covered by the branches that
// place its true image; a member that does not span the class departs with its prefix and is
// completed once by the base pipeline in echo_main.
#include "echo_search.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "echo_dpiso.h"
#include <cmath>
#include "echo_envelope.h"
#include "matching/BuildTable.h"
#include "matching/GenerateQueryPlan.h"
#include "matching/FilterVertices.h"

namespace echo {
namespace {

constexpr uint32_t kNoData = 0xFFFFFFFFu;
constexpr int kMaxMembers = 64;  // the member mask is one uint64_t

struct MemberCtx {
    uint32_t q = 0;                              // query size
    std::vector<uint32_t> label;                 // [q]
    std::vector<std::vector<uint32_t>> adj;      // [q] query neighbours
    std::vector<std::vector<char>> adjm;         // [q][q] adjacency matrix
    std::vector<int> wpos;                       // [q] envelope class or -1
};

// Data graph view for the envelope filter: label lookups return the union of the class label
// set, and the label predicate answers membership in the set. Everything else is the real graph.
class EnvDataView : public Graph {
public:
    EnvDataView(const Graph* data, std::vector<std::vector<uint32_t>> label_sets,
                bool (*cancel)(void*) = nullptr, void* cancel_ctx = nullptr)
        : Graph(false), data_(data), label_sets_(std::move(label_sets)) {
        by_label_.resize(label_sets_.size());
        std::vector<char> mark(data->getVerticesCount(), 0);
        for (size_t c = 0; c < label_sets_.size(); ++c) {
            if (cancel && cancel(cancel_ctx)) break;
            for (uint32_t lab : label_sets_[c]) {
                ui cnt = 0;
                const ui* vs = data->getVerticesByLabel(lab, cnt);
                for (ui k = 0; k < cnt; ++k)
                    if (!mark[vs[k]]) {
                        mark[vs[k]] = 1;
                        by_label_[c].push_back(vs[k]);
                    }
            }
            for (uint32_t v : by_label_[c]) mark[v] = 0;
            std::sort(by_label_[c].begin(), by_label_[c].end());
            max_freq_ = std::max<ui>(max_freq_, (ui)by_label_[c].size());
        }
    }
    void loadGraphFromFile(const std::string&) override {}
    void loadGraphFromFileCompressed(const std::string&, const std::string&, const std::string&) override {}
    const ui getVerticesCount() const override { return data_->getVerticesCount(); }
    const ui getEdgesCount() const override { return data_->getEdgesCount(); }
    const ui getLabelsCount() const override { return (ui)label_sets_.size(); }
    const ui getGraphMaxDegree() const override { return data_->getGraphMaxDegree(); }
    const ui getGraphMaxLabelFrequency() const override { return max_freq_; }
    const ui getVertexDegree(const VertexID id) const override { return data_->getVertexDegree(id); }
    const LabelID getVertexLabel(const VertexID id) const override { return data_->getVertexLabel(id); }
    bool hasLabelSets() const override { return true; }
    bool hasVertexLabel(const VertexID id, const LabelID label) const override {
        if (label >= label_sets_.size()) return false;
        return std::binary_search(label_sets_[label].begin(), label_sets_[label].end(),
                                  data_->getVertexLabel(id));
    }
    const ui getLabelsFrequency(const LabelID label) const override {
        return label < by_label_.size() ? (ui)by_label_[label].size() : 0;
    }
    const VertexID* getVerticesByLabel(const LabelID label, ui& count) const override {
        if (label >= by_label_.size()) {
            count = 0;
            return nullptr;
        }
        count = (ui)by_label_[label].size();
        return by_label_[label].data();
    }
    const VertexID* getVertexNeighbors(const VertexID id, ui& count) const override {
        return data_->getVertexNeighbors(id, count);
    }
    const VertexID* getNeighborsByLabel(const VertexID id, const LabelID label, ui& count) const override {
        nbr_buf_.clear();
        if (label >= by_label_.size()) {
            count = 0;
            return nullptr;
        }
        ui cnt = 0;
        const ui* nb = data_->getVertexNeighbors(id, cnt);
        const std::vector<uint32_t>& want = by_label_[label];
        for (ui k = 0; k < cnt; ++k)
            if (std::binary_search(want.begin(), want.end(), nb[k])) nbr_buf_.push_back(nb[k]);
        std::sort(nbr_buf_.begin(), nbr_buf_.end());
        count = (ui)nbr_buf_.size();
        return nbr_buf_.data();
    }

private:
    const Graph* data_;
    std::vector<std::vector<uint32_t>> label_sets_;
    std::vector<std::vector<uint32_t>> by_label_;
    mutable std::vector<VertexID> nbr_buf_;
    ui max_freq_ = 1;
};

struct Ctxt {
    const Graph* data = nullptr;
    const Envelope* env = nullptr;
    int mode = 0;
    int m = 0;                 // members in the envelope
    bool candidate_exceeded = false;
    bool multi_member = false; // m > 1: the single-survivor handoff is a sharing optimisation
    int q = 0;                 // query vertices per member
    int npos = 0;              // envelope classes
    uint32_t V = 0;
    uint32_t ewords = 0;

    std::vector<std::vector<std::pair<int, int>>> span;  // [class] (member, vertex)
    std::vector<std::vector<int>> member_at;              // [m][class] -> vertex or -1
    std::vector<MemberCtx> mem;

    // N4 output: one candidate list per class and a member mask per candidate.
    std::vector<std::vector<uint32_t>> clist;
    std::vector<std::vector<uint64_t>> cmask;
    uint64_t all_members = 0;

    // N5: base matching order, tables, and the per-branch placement state.
    Graph* pattern = nullptr;
    Edges*** edge_matrix = nullptr;
    std::vector<uint32_t> order;
    std::vector<int> val_pos;                          // [class] position or -1
    std::vector<std::vector<uint64_t>> inuse;          // [m] data vertices used by member m
    std::vector<std::vector<uint32_t>> val;            // [m][q] data vertex or kNoData
    std::vector<std::vector<std::vector<std::pair<uint32_t, uint32_t>>>> depart;

    std::vector<std::vector<uint32_t>> pos_at;  // [depth] candidates of that class, snapshot
    std::vector<uint32_t> base_pos;             // common-edge expansion scratch
    std::vector<uint32_t> domain;               // one member's narrowed domain scratch
    std::vector<uint32_t> stamp, stamp_base, stamp_union;
    uint32_t serial = 0;
    uint32_t stamp_cur = 0;

    long long nodes = 0, steps = 0, solutions = 0, mask_kills = 0;
    long long member_cell_sum = 0;
    std::atomic<bool>* abort = nullptr;
    bool expired = false;
    Clock::time_point deadline = Clock::time_point::max();
    Clock::time_point query_deadline = Clock::time_point::max();
    std::vector<std::vector<std::vector<uint32_t>>>* per_query = nullptr;
};

void free_tables(const Graph* query, Edges*** edge_matrix) {
    for (ui i = 0; i < query->getVerticesCount(); ++i) {
        for (ui j = 0; j < query->getVerticesCount(); ++j)
            if (edge_matrix[i][j] != nullptr) delete edge_matrix[i][j];
        delete[] edge_matrix[i];
    }
    delete[] edge_matrix;
}

// Build the envelope pattern as an ordinary base graph: class c has label id c, and its
// neighbours are the common edges only.
Graph* make_pattern(const Envelope& env, const std::vector<QueryGraph>& queries,
                    std::vector<std::vector<uint32_t>>& padj, std::vector<std::vector<uint32_t>>& label_sets) {
    padj.assign(env.n, {});
    for (uint32_t c = 0; c < env.n; ++c)
        for (uint32_t c2 = c + 1; c2 < env.n; ++c2) {
            bool all = true, any = false;
            for (uint32_t t = 0; t < (uint32_t)env.members.size() && all; ++t) {
                int uc = -1, uc2 = -1;
                for (auto& code : env.span_v[c])
                    if ((code >> 16) == t) uc = (int)(code & 0xFFFF);
                for (auto& code : env.span_v[c2])
                    if ((code >> 16) == t) uc2 = (int)(code & 0xFFFF);
                if (uc < 0 && uc2 < 0) continue;
                if (uc < 0 || uc2 < 0) {
                    all = false;
                    break;
                }
                const QueryGraph& q = queries[env.members[t]];
                if (std::binary_search(q.adj[uc].begin(), q.adj[uc].end(), (uint32_t)uc2)) any = true;
                else {
                    all = false;
                    break;
                }
            }
            if (any && all) {
                padj[c].push_back(c2);
                padj[c2].push_back(c);
            }
        }
    label_sets.assign(env.n, {});
    for (uint32_t c = 0; c < env.n; ++c)
        for (auto& code : env.span_v[c]) {
            uint32_t t = code >> 16, r = code & 0xFFFF;
            label_sets[c].push_back(queries[env.members[t]].label[r]);
        }
    for (auto& s : label_sets) {
        std::sort(s.begin(), s.end());
        s.erase(std::unique(s.begin(), s.end()), s.end());
    }
    QueryGraph pg;
    pg.n = env.n;
    pg.label.resize(env.n);
    pg.adj = padj;
    for (uint32_t c = 0; c < env.n; ++c) pg.label[c] = c;
    return to_base_query(pg);
}

bool expired(Ctxt& C);
bool cancelled_filter(void* p) { return expired(*static_cast<Ctxt*>(p)); }

bool setup(Ctxt& C, const std::vector<QueryGraph>& queries, const Graph* data, const Envelope& env, int mode, double max_candidates) {
    C.data = data;
    C.env = &env;
    C.mode = mode;
    C.m = (int)env.members.size();
    C.multi_member = C.m > 1;
    C.V = data->getVerticesCount();
    C.ewords = words_needed(C.V);
    if (C.m <= 0 || C.m > kMaxMembers || env.n == 0) return false;
    C.q = (int)queries[env.members[0]].n;
    for (int t = 1; t < C.m; ++t)
        if ((int)queries[env.members[t]].n != C.q) {
            std::fprintf(stderr, "ECHO: members of different sizes in one envelope\n");
            return false;
        }
    C.npos = (int)env.n;
    C.all_members = (C.m == 64) ? ~0ULL : ((1ULL << C.m) - 1);

    C.span.assign(C.npos, {});
    std::vector<int> seen((size_t)C.m * C.q, 0);
    for (uint32_t c = 0; c < env.n; ++c)
        for (auto& code : env.span_v[c]) {
            int t = (int)(code >> 16), r = (int)(code & 0xFFFF);
            if (t >= C.m || r >= C.q) return false;
            if (++seen[(size_t)t * C.q + r] != 1) {
                std::fprintf(stderr, "ECHO: member vertex spanned twice\n");
                return false;
            }
            C.span[c].emplace_back(t, r);
        }
    for (int t = 0; t < C.m; ++t)
        for (int r = 0; r < C.q; ++r)
            if (seen[(size_t)t * C.q + r] != 1) return false;  // a hole cannot be completed here

    C.mem.assign(C.m, MemberCtx());
    C.member_at.assign(C.m, std::vector<int>(C.npos, -1));
    for (int t = 0; t < C.m; ++t) {
        MemberCtx& M = C.mem[t];
        const QueryGraph& g = queries[env.members[t]];
        M.q = C.q;
        M.label = g.label;
        M.adj = g.adj;
        M.adjm.assign(C.q, std::vector<char>(C.q, 0));
        for (int r = 0; r < C.q; ++r)
            for (uint32_t w : g.adj[r]) M.adjm[r][w] = 1;
        M.wpos.assign(C.q, -1);
        for (int c = 0; c < C.npos; ++c)
            for (auto& mr : C.span[c])
                if (mr.first == t) {
                    M.wpos[mr.second] = c;
                    C.member_at[t][c] = mr.second;
                }
    }

    // N4: the base LDF + DPiso filter on the envelope pattern. A partial edge (one that not
    // every member covering both ends has) is not in the pattern, so the pattern can be
    // disconnected; DPiso's plan is a BFS order and needs a connected graph. Run the base
    // filter once per connected component (still once per envelope class, not per member) and
    // keep the component's candidates for its classes.
    std::vector<std::vector<uint32_t>> padj, label_sets;
    C.pattern = make_pattern(env, queries, padj, label_sets);
    if (C.pattern == nullptr) return false;
    EnvDataView view(data, label_sets, cancelled_filter, &C);
    if (expired(C)) return false;

    std::vector<std::vector<uint32_t>> comps;
    {
        std::vector<int> comp_id(C.npos, -1);
        for (int s0 = 0; s0 < C.npos; ++s0) {
            if (comp_id[s0] >= 0) continue;
            std::vector<uint32_t> comp;
            std::vector<int> st{s0};
            comp_id[s0] = (int)comps.size();
            while (!st.empty()) {
                int u = st.back();
                st.pop_back();
                comp.push_back((uint32_t)u);
                for (uint32_t w : padj[u])
                    if (comp_id[w] < 0) {
                        comp_id[w] = (int)comps.size();
                        st.push_back((int)w);
                    }
            }
            comps.push_back(std::move(comp));
        }
    }

    std::vector<std::vector<uint32_t>> raw(C.npos);
    C.order.clear();
    for (auto& comp : comps) {
        if (expired(C)) return false;
        const uint32_t s = (uint32_t)comp.size();
        std::vector<int> loc(C.npos, -1);
        for (uint32_t i = 0; i < s; ++i) loc[comp[i]] = (int)i;
        QueryGraph lq;
        lq.n = s;
        lq.label.assign(s, 0);
        lq.adj.assign(s, {});
        for (uint32_t i = 0; i < s; ++i) {
            lq.label[i] = comp[i];  // global class id; EnvDataView indexes its label sets by it
            for (uint32_t c2 : padj[comp[i]]) lq.adj[i].push_back((uint32_t)loc[c2]);
        }
        PredicateDomain common_domain; common_domain.allowed.resize(s); common_domain.constrained.assign(s, 0);
        bool any_common_domain = false;
        for (uint32_t i=0; i<s; ++i) {
            uint32_t c = comp[i]; bool common = C.span[c].size() == (size_t)C.m, first = true;
            std::vector<uint32_t> domain;
            for (const auto& mr : C.span[c]) {
                const auto& g = queries[env.members[mr.first]];
                if (!g.predicates() || !g.predicates()->constrained[mr.second]) { common = false; break; }
                if (first) { domain = g.predicates()->allowed[mr.second]; first = false; }
                else if (domain != g.predicates()->allowed[mr.second]) { common = false; break; }
            }
            if (common && !first) { common_domain.constrained[i] = 1; common_domain.allowed[i] = std::move(domain); any_common_domain = true; }
        }
        if (any_common_domain) lq.domain_id = store_predicate_domain(std::move(common_domain));
        Graph* lqg = to_base_query(lq);

        ui** cand = nullptr;
        ui* cand_count = nullptr;
        ui* filter_order = nullptr;
        TreeNode* tree = nullptr;
        bool ok = FilterVertices::DPisoFilter(&view, lqg, cand, cand_count, filter_order, tree, cancelled_filter, &C);
        delete[] filter_order;
        delete[] tree;
        if (!ok) {
            for (ui i = 0; i < s; ++i) delete[] cand[i];
            delete[] cand;
            delete[] cand_count;
            delete lqg;
            delete C.pattern;
            C.pattern = nullptr;
            return false;
        }
        for (uint32_t i = 0; i < s; ++i) {
            // The base filter checked the deadline once per class.
            std::vector<uint32_t>& dst = raw[comp[i]];
            dst.assign(cand[i], cand[i] + cand_count[i]);
            std::sort(dst.begin(), dst.end());
            dst.erase(std::unique(dst.begin(), dst.end()), dst.end());
        }
        for (ui i = 0; i < s; ++i) delete[] cand[i];
        delete[] cand;
        delete[] cand_count;

        // Base GQL order for this component.
        ui** ca = new ui*[s];
        ui* cc = new ui[s];
        for (uint32_t i = 0; i < s; ++i) {
            ca[i] = raw[comp[i]].data();
            cc[i] = (ui)raw[comp[i]].size();
        }
        ui* gql_order = nullptr;
        ui* pivot = nullptr;
        GenerateQueryPlan::generateGQLQueryPlan(&view, lqg, cc, gql_order, pivot);
        delete[] pivot;
        for (uint32_t i = 0; i < s; ++i) C.order.push_back(comp[gql_order[i]]);
        delete[] gql_order;
        delete[] ca;
        delete[] cc;
        delete lqg;
    }

    C.clist.assign(C.npos, {});
    C.cmask.assign(C.npos, {});
    for (int c = 0; c < C.npos; ++c) {
        if (expired(C)) return false;
        for (uint32_t v : raw[c]) {
            uint64_t mask = 0;
            for (auto& mr : C.span[c]) {
                int t = mr.first, r = mr.second;
                const QueryGraph& g = queries[env.members[t]];
                if (data->getVertexLabel(v) == g.label[r] &&
                    data->getVertexDegree(v) >= (ui)g.adj[r].size() && g.permits(r, v))
                    mask |= 1ULL << t;
            }
            if (mask == 0) continue;
            C.clist[c].push_back(v);
            C.cmask[c].push_back(mask);
            C.member_cell_sum += (long long)__builtin_popcountll(mask);
        }
        // An empty private class kills only members owning that class. Other
        // members depart before it and are completed by their constrained base.
        bool predicate_private_empty = false;
        if (C.clist[c].empty() && C.span[c].size() < (size_t)C.m)
            for (const auto& mr : C.span[c]) {
                const auto* predicates = queries[env.members[mr.first]].predicates();
                if (predicates && predicates->constrained[mr.second]) predicate_private_empty = true;
            }
        if (C.clist[c].empty() && !predicate_private_empty) {
            delete C.pattern;
            C.pattern = nullptr;
            return false;
        }
    }

    // Reject growth immediately after the class domains exist, before edge-table
    // construction or N5 buffers. The work spent building them remains charged.
    uint64_t actual_candidates = 0;
    for (const auto& list : C.clist) actual_candidates += list.size();
    if ((double)actual_candidates > max_candidates) {
        C.candidate_exceeded = true;
        return false;
    }

    // N5: candidate edge tables on the (possibly disconnected) pattern. BuildTable only walks
    // the pattern edges, so it does not need an order; the per-component GQL orders above are
    // the base matching order the walk follows.
    ui** ca = new ui*[C.npos];
    ui* cc = new ui[C.npos];
    for (int c = 0; c < C.npos; ++c) {
        ca[c] = C.clist[c].data();
        cc[c] = (ui)C.clist[c].size();
    }
    C.edge_matrix = new Edges**[C.npos];
    for (int c = 0; c < C.npos; ++c) C.edge_matrix[c] = new Edges*[C.npos];
    BuildTable::buildTables(&view, C.pattern, ca, cc, C.edge_matrix, cancelled_filter, &C);
    delete[] ca;
    delete[] cc;
    if (expired(C)) { free_tables(C.pattern, C.edge_matrix); C.edge_matrix=nullptr; return false; }

    C.val_pos.assign(C.npos, -1);
    C.pos_at.assign(C.npos, {});
    C.inuse.assign(C.m, std::vector<uint64_t>(C.ewords, 0ULL));
    C.val.assign(C.m, std::vector<uint32_t>(C.q, kNoData));
    C.depart.assign(C.m, {});
    size_t max_list = 0;
    for (auto& l : C.clist) max_list = std::max(max_list, l.size());
    C.stamp.assign(max_list + 1, 0);
    C.stamp_base.assign(max_list + 1, 0);
    C.stamp_union.assign(max_list + 1, 0);
    if (expired(C)) { free_tables(C.pattern, C.edge_matrix); C.edge_matrix=nullptr; return false; }
    return true;
}

int pop_next(uint64_t& mask) {
    if (mask == 0) return -1;
    int t = __builtin_ctzll(mask);
    mask &= mask - 1;
    return t;
}

void walk(Ctxt& C, int depth, uint64_t alive);

inline bool expired(Ctxt& C) {
    if (C.expired) return true;
    if (Clock::now() < C.deadline) return false;
    C.expired=true;
    if (C.abort && C.query_deadline <= C.deadline) *C.abort=true;
    return true;
}

// Candidates of class c reachable through the base edge table of every placed common-edge
// neighbour. The table maps a position in the neighbour's candidate list to positions in c.
// Edge-table expansion: positions of class c adjacent to the placed value of every placed
// common-edge neighbour. Returns false when no such neighbour exists (the caller then uses all
// positions); an empty output means the class has no adjacent candidate and the branch dies.
bool gather_positions(Ctxt& C, int c, std::vector<uint32_t>& out) {
    out.clear();
    bool first = true;
    ui qn = 0;
    const ui* nbrs = C.pattern->getVertexNeighbors(c, qn);
    for (ui k = 0; k < qn; ++k) {
        ui q = nbrs[k];
        if (C.val_pos[q] < 0 || C.edge_matrix[q][c] == nullptr) continue;
        const Edges* e = C.edge_matrix[q][c];
        ui pos = (ui)C.val_pos[q];
        ui begin = e->offset_[pos], end = e->offset_[pos + 1];
        if (first) {
            out.assign(e->edge_ + begin, e->edge_ + end);
            first = false;
        } else {
            ++C.stamp_cur;
            for (ui i = begin; i < end; ++i) {
                if ((i&1023u)==0 && expired(C)) return true;
                C.stamp[e->edge_[i]]=C.stamp_cur;
            }
            size_t w = 0;
            for (uint32_t p : out)
                if (C.stamp[p] == C.stamp_cur) out[w++] = p;
            out.resize(w);
        }
        if (out.empty()) return true;
    }
    return !first;
}

// One member's narrowed domain at class c: positions in C.clist[c] that the member's placed
// neighbours allow and that carry the member's mask bit. Optional edges (not in the pattern)
// are checked here against the member's own placed values; this is what keeps an edit class
// with a large union candidate set from being scanned once per candidate of every member.
void member_domain(Ctxt& C, int t, int c, std::vector<uint32_t>& out) {
    out.clear();
    int r = C.member_at[t][c];
    if (r < 0) return;
    const MemberCtx& M = C.mem[t];
    int bestw = -1;
    ui bestdeg = 0xFFFFFFFFu;
    for (uint32_t w : M.adj[r]) {
        uint32_t x = C.val[t][w];
        if (x == kNoData) continue;
        ui d = C.data->getVertexDegree(x);
        if (d < bestdeg) { bestdeg = d; bestw = (int)w; }
    }
    if (bestw < 0) {
        for (size_t pos = 0; pos < C.clist[c].size(); ++pos)
            if ((C.cmask[c][pos] >> t) & 1ULL) out.push_back((uint32_t)pos);
        return;
    }
    ui cnt = 0;
    const ui* nb = C.data->getVertexNeighbors(C.val[t][bestw], cnt);
    for (ui k = 0; k < cnt; ++k) {
        if ((k&1023u)==0 && expired(C)) { out.clear();return; }
        uint32_t v = nb[k];
        auto it = std::lower_bound(C.clist[c].begin(), C.clist[c].end(), v);
        if (it == C.clist[c].end() || *it != v) continue;
        size_t pos = (size_t)(it - C.clist[c].begin());
        if (((C.cmask[c][pos] >> t) & 1ULL) == 0) continue;
        bool ok = true;
        for (uint32_t w : M.adj[r]) {
            if ((int)w == bestw) continue;
            uint32_t x = C.val[t][w];
            if (x == kNoData) continue;
            if (x == v || !C.data->checkEdgeExistence(v, x)) { ok = false; break; }
        }
        if (ok) out.push_back((uint32_t)pos);
    }
}

void walk(Ctxt& C, int depth, uint64_t alive) {
    ++C.nodes;
    ++C.steps;
    if (expired(C) || (C.abort && C.abort->load())) return;

    if (depth == C.npos) {
        uint64_t live = alive;
        while (live) {
            int t = pop_next(live);
            bool ok = true;
            if (C.mode == kNoMask) {
                MemberCtx& M = C.mem[t];
                for (int r = 0; r < C.q && ok; ++r) {
                    int c = M.wpos[r];
                    uint32_t pos = (uint32_t)C.val_pos[c];
                    ok = (c >= 0 && pos < C.cmask[c].size() && ((C.cmask[c][pos] >> t) & 1ULL));
                    if (!ok) break;
                    for (uint32_t w : M.adj[r]) {
                        if (!C.data->checkEdgeExistence(C.val[t][r], C.val[t][w])) {
                            ok = false;
                            break;
                        }
                    }
                }
            }
            if (ok) {
                C.per_query->at(t).push_back(C.val[t]);
                ++C.solutions;
            }
        }
        return;
    }

    int c = (int)C.order[depth];
    uint64_t live = alive;
    // Members that do not span this class leave the walk with the prefix placed so far.
    uint64_t scan = live;
    while (scan) {
        int t = pop_next(scan);
        if (C.member_at[t][c] >= 0) continue;
        std::vector<std::pair<uint32_t, uint32_t>> pre;
        for (int r = 0; r < C.q; ++r)
            if (C.val[t][r] != kNoData) pre.emplace_back((uint32_t)r, C.val[t][r]);
        C.depart[t].push_back(std::move(pre));
        live &= ~(1ULL << t);
    }
    if (live == 0) return;
    // Once only one member is left in the branch, sharing is over: hand its prefix to the base
    // pipeline (N6 completion) instead of walking the rest of the classes with a one-member
    // envelope. This is the same departure path as a member that does not span a class, and it
    // keeps an edit class from being scanned once per candidate of a member that already split
    // off from the others.
    if (C.multi_member && (live & (live - 1)) == 0) {
        int t = __builtin_ctzll(live);
        std::vector<std::pair<uint32_t, uint32_t>> pre;
        for (int r = 0; r < C.q; ++r)
            if (C.val[t][r] != kNoData) pre.emplace_back((uint32_t)r, C.val[t][r]);
        C.depart[t].push_back(std::move(pre));
        return;
    }
    // A merged envelope whose live set has already shrunk to two members is close to the
    // per-member case; keep the joint walk only while at least three members still share it.
    if (C.multi_member && C.m > 2 && __builtin_popcountll(live) <= 2) {
        uint64_t rest = live;
        while (rest) {
            int t = pop_next(rest);
            std::vector<std::pair<uint32_t, uint32_t>> pre;
            for (int r = 0; r < C.q; ++r)
                if (C.val[t][r] != kNoData) pre.emplace_back((uint32_t)r, C.val[t][r]);
            C.depart[t].push_back(std::move(pre));
        }
        return;
    }

    bool restricted = gather_positions(C, c, C.base_pos);
    if (C.expired) return;
    uint32_t base_serial = 0;
    if (restricted) {
        base_serial = ++C.serial;
        for (uint32_t p : C.base_pos) C.stamp_base[p] = base_serial;
    }
    const uint32_t union_serial = ++C.serial;
    C.pos_at[depth].clear();
    uint64_t scan_members = live;
    while (scan_members) {
        int t = pop_next(scan_members);
        member_domain(C, t, c, C.domain);
        if (C.expired) return;
        for (uint32_t p : C.domain) {
            if (restricted && C.stamp_base[p] != base_serial) continue;
            if (C.stamp_union[p] == union_serial) continue;
            C.stamp_union[p] = union_serial;
            C.pos_at[depth].push_back(p);
        }
    }
    for (uint32_t pos : C.pos_at[depth]) {
        if (expired(C)) break;
        uint32_t v = C.clist[c][pos];
        uint64_t m = (C.mode == kNoMask) ? live : (C.cmask[c][pos] & live);
        if (m == 0) continue;
        uint64_t accepted = 0;
        uint64_t cand_m = m;
        while (cand_m) {
            int t = pop_next(cand_m);
            if ((C.inuse[t][v >> 6] >> (v & 63)) & 1ULL) {
                ++C.mask_kills;
                continue;
            }
            bool ok = true;
            if (C.mode != kNoMask) {
                int r = C.member_at[t][c];
                const MemberCtx& M = C.mem[t];
                for (uint32_t w : M.adj[r]) {
                    uint32_t x = C.val[t][w];
                    if (x == kNoData) continue;
                    if (x == v || !C.data->checkEdgeExistence(v, x)) {
                        ok = false;
                        break;
                    }
                }
            }
            if (ok) accepted |= 1ULL << t;
            else ++C.mask_kills;
        }
        if (accepted == 0) continue;

        uint64_t place = accepted;
        while (place) {
            int t = pop_next(place);
            int r = C.member_at[t][c];
            C.val[t][r] = v;
            C.inuse[t][v >> 6] |= 1ULL << (v & 63);
        }
        C.val_pos[c] = (int)pos;
        walk(C, depth + 1, accepted);
        C.val_pos[c] = -1;
        place = accepted;
        while (place) {
            int t = pop_next(place);
            int r = C.member_at[t][c];
            C.val[t][r] = kNoData;
            C.inuse[t][v >> 6] &= ~(1ULL << (v & 63));
        }
        if (C.abort && C.abort->load()) break;
    }
}

}  // namespace

EnvRun run_envelope(const std::vector<QueryGraph>& queries, const Graph* data, const Envelope& env,
                    const std::vector<uint32_t>& label_vertices, int mode, std::atomic<bool>* abort,
                    Clock::time_point deadline, EnvLimits limits) {
    EnvRun r;
    // A predicate-empty envelope has zero matches for each member, not missing
    // result slots. Keep the unconstrained legacy allocation path unchanged.
    for (uint32_t member : env.members) if (queries[member].domain_id) {
        r.per_query.assign(env.members.size(), {}); break;
    }
    (void)label_vertices;
    Ctxt C;
    C.abort = abort;
    C.deadline = deadline;
    C.query_deadline = deadline;
    Stopwatch sw;
    if (std::isfinite(limits.attempt_seconds)) {
        r.n5_deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(std::max(0.0, limits.attempt_seconds)));
        C.deadline = std::min(deadline, r.n5_deadline);
    }
    if (!setup(C, queries, data, env, mode, limits.max_candidates)) {
        r.filter_sec = sw.seconds();
        r.ok = false;
        r.candidate_exceeded = C.candidate_exceeded;
        r.budget_exceeded = C.expired && r.n5_deadline <= deadline;
        r.filter_budget_exceeded = r.budget_exceeded;
        r.timed_out = C.expired && !r.budget_exceeded;
        for (const auto& list : C.clist) r.cell_sum += list.size();
        r.per_query.assign(env.members.size(), {});
        if (C.pattern) delete C.pattern;
        return r;
    }
    r.filter_sec = sw.seconds();
    r.npos = (uint32_t)C.npos;
    for (auto& l : C.clist) {
        r.cell_sum += l.size();
        r.cell_max = std::max<uint32_t>(r.cell_max, (uint32_t)l.size());
    }
    r.member_cell_sum = (uint64_t)C.member_cell_sum;

    sw.reset();
    if (std::isfinite(limits.walk_seconds)) {
        r.n5_deadline = std::min(r.n5_deadline, Clock::now() + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(std::max(0.0, limits.walk_seconds))));
        C.deadline = std::min(deadline, r.n5_deadline);
    }
    r.per_query.assign(C.m, {});
    C.per_query = &r.per_query;
    walk(C, 0, C.all_members);
    r.search_sec = sw.seconds();

    r.nodes = C.nodes;
    r.mask_kills = (uint64_t)C.mask_kills;
    r.solutions = C.solutions;
    for (int m = 0; m < C.m; ++m)
        if (!C.depart[m].empty()) r.depart.push_back({m, C.depart[m]});
    r.budget_exceeded = C.expired && r.n5_deadline <= deadline && Clock::now() >= r.n5_deadline;
    r.timed_out = (C.expired && !r.budget_exceeded) || (abort && abort->load());
    if (r.budget_exceeded) {
        r.discarded_solutions = r.solutions;
        r.solutions = 0;
        r.per_query.assign(C.m, {});
        r.depart.clear();
    }
    free_tables(C.pattern, C.edge_matrix);
    delete C.pattern;
    return r;
}

MatcherPlan plan_for(const QueryGraph& q, const std::vector<uint32_t>& label_vertices) {
    MatcherPlan p;
    (void)q;
    (void)label_vertices;
    return p;
}

}  // namespace echo
