#include "echo_common.h"
#include <unistd.h>
#include <memory>
#include "../vendor/nlohmann/json.hpp"

namespace echo {
namespace {
std::vector<std::unique_ptr<const PredicateDomain>>& domains() {
    static std::vector<std::unique_ptr<const PredicateDomain>> values;
    return values;
}
class PredicateQueryGraph final : public Graph {
    const PredicateDomain* domain_;
public:
    explicit PredicateQueryGraph(const PredicateDomain* domain) : Graph(false), domain_(domain) {}
    bool echoHasDomains() const override { return true; }
    bool echoPermits(ui u, VertexID v) const override { return domain_->permits(u,v); }
};
}
const PredicateDomain* predicate_domain(uint32_t id) { return domains().at(id-1).get(); }
uint32_t store_predicate_domain(PredicateDomain value) {
    domains().emplace_back(new PredicateDomain(std::move(value)));
    return domains().size();
}


Graph* to_base_query(const QueryGraph& q) {
    // Serialise once into a scratch file; the base loader only reads from files.
    std::ostringstream text;
    uint64_t m = 0;
    for (uint32_t u = 0; u < q.n; ++u) m += q.adj[u].size();
    m /= 2;
    text << "t " << q.n << " " << m << "\n";
    for (uint32_t u = 0; u < q.n; ++u) text << "v " << u << " " << q.label[u] << " " << q.adj[u].size() << "\n";
    for (uint32_t u = 0; u < q.n; ++u)
        for (uint32_t v : q.adj[u])
            if (u < v) text << "e " << u << " " << v << "\n";
    static uint64_t counter = 0;
    const char* tmp = std::getenv("TMPDIR");
    std::string path = std::string(tmp && *tmp ? tmp : "/tmp") + "/echo-q-" + std::to_string(::getpid()) + "-" +
                       std::to_string(counter++) + ".graph";
    {
        std::ofstream out(path);
        out << text.str();
    }
    Graph* g = q.domain_id ? static_cast<Graph*>(new PredicateQueryGraph(q.predicates())) : new Graph(false);
    g->loadGraphFromFile(path);
    std::remove(path.c_str());
    return g;
}

std::string Workload::query_path(uint32_t index) const {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/batch-%02u/q%03u.graph", query_batches.empty() ? index / batch_size : query_batches.at(index), index);
    return dir + buf;
}

Workload read_workload_manifest(const std::string& dir) {
    Workload w;
    w.dir = dir;
    using json = nlohmann::json;
    std::ifstream meta(dir + "/manifest.json");
    std::string text((std::istreambuf_iterator<char>(meta)), std::istreambuf_iterator<char>());
    if (text.find("\"batch_layout\"") == std::string::npos) {
        // Preserve the frozen legacy input path, including its allocation behaviour.
        auto grab = [&](const char* key) -> long {
            size_t p=text.find(key);
            if (p==std::string::npos) return -1;
            p=text.find(':',p);
            return std::strtol(text.c_str()+p+1,nullptr,10);
        };
        long queries=grab("\"queries\""), size=grab("\"batch_size\"");
        if (queries<=0 || size<=0) throw std::runtime_error("bad workload manifest in " + dir);
        w.queries=queries; w.batch_size=size;
        if (!w.queries || !w.batch_size) throw std::runtime_error("bad workload manifest in " + dir);
    } else {
        std::ifstream bi(dir + "/batch_index.json"); json batches; bi >> batches;
        for (const auto& batch : batches.at("batches")) {
            uint32_t id = batch.at("batch");
            if (id != w.starts.size()) throw std::runtime_error("non-contiguous batch IDs");
            w.starts.push_back(w.query_batches.size());
            w.sizes.push_back(batch.at("indices").size());
            for (const auto& index : batch.at("indices")) {
                if (index.get<uint32_t>() != w.query_batches.size()) throw std::runtime_error("non-contiguous query IDs");
                w.query_batches.push_back(id);
            }
        }
        w.queries = w.query_batches.size();
        std::ifstream qi(dir + "/queries.json"); json records; qi >> records;
        std::ifstream pi(dir + "/predicate-index.json"); json predicates; pi >> predicates;
        if (records.size() != w.queries) throw std::runtime_error("query metadata coverage");
        for (uint32_t i=0; i<w.queries; ++i) {
            const auto& rec = records.at(i);
            if (rec.at("index") != i) throw std::runtime_error("query index mismatch");
            QueryGraph q = read_query(w.query_path(i));
            PredicateDomain domain; domain.allowed.resize(q.n); domain.constrained.assign(q.n, 0);
            for (const auto& anchor : rec.at("query").at("anchors")) {
                uint32_t u = anchor.at("vertex");
                const auto& values = predicates.at(anchor.at("predicate_key").get<std::string>());
                std::vector<uint32_t> ids = values.get<std::vector<uint32_t>>();
                if (!std::is_sorted(ids.begin(), ids.end())) throw std::runtime_error("unsorted predicate index");
                if (!domain.constrained.at(u)) domain.allowed[u] = ids;
                else {
                    std::vector<uint32_t> both;
                    std::set_intersection(domain.allowed[u].begin(),domain.allowed[u].end(),ids.begin(),ids.end(),std::back_inserter(both));
                    domain.allowed[u] = std::move(both);
                }
                domain.constrained[u] = 1;
            }
            if (!rec.at("query").at("anchors").empty()) q.domain_id = store_predicate_domain(std::move(domain));
            w.semantic_queries.push_back(std::move(q));
        }
    }
    return w;
}

Graph* load_data_graph(const std::string& path) {
    Graph* g = new Graph(true);
    g->loadGraphFromFile(path);
    return g;
}

long long peak_rss_kb() {
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("VmHWM:", 0) == 0) return std::atoll(line.c_str() + 6);
    }
    return -1;
}

}  // namespace echo
