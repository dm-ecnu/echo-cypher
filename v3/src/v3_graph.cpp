#include "v3_graph.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

#include "v3_json.h"

namespace v3 {

namespace {
bool read_file(const std::string& p, std::string* out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}
template <typename T>
bool read_vec(const std::string& p, std::vector<T>* v, size_t expect, std::string* err) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) {
        if (err) *err = "cannot open " + p;
        return false;
    }
    const size_t bytes = (size_t)in.tellg();
    if (bytes % sizeof(T)) {
        if (err) *err = "size mismatch " + p;
        return false;
    }
    in.seekg(0);
    v->resize(bytes / sizeof(T));
    in.read(reinterpret_cast<char*>(v->data()), (std::streamsize)bytes);
    if (expect && v->size() != expect) {
        if (err) *err = "count mismatch " + p;
        return false;
    }
    return true;
}
// A read-only mapping of a uint32 file (the memory is the page cache, not a private copy).
struct MapU32 {
    const uint32_t* p = nullptr;
    size_t n = 0;
    void* base = nullptr;
};
bool open_map(const std::string& path, size_t expect, MapU32* m, std::string* err) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || (st.st_size % 4) != 0) {
        ::close(fd);
        if (err) *err = "size mismatch " + path;
        return false;
    }
    m->n = (size_t)st.st_size / 4;
    if (expect && m->n != expect) {
        ::close(fd);
        if (err) *err = "count mismatch " + path;
        return false;
    }
    const long pages = (long)sysconf(_SC_PAGESIZE);
    const size_t len = ((m->n * 4 + (size_t)pages - 1) / (size_t)pages) * (size_t)pages;
    void* v = mmap(nullptr, len ? len : (size_t)pages, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (v == MAP_FAILED) {
        if (err) *err = "cannot map " + path;
        return false;
    }
    m->base = v;
    m->p = static_cast<const uint32_t*>(v);
    madvise(v, m->n * 4, MADV_RANDOM);
    return true;
}
// Map files stay mapped for the process lifetime (the arrays below are built from them).
std::vector<MapU32>& maps() {
    static std::vector<MapU32> m;
    return m;
}

// Property column segment: key * 256 + col (key = label id for vertices, type id for edges).
inline uint32_t seg_of(uint32_t key, uint32_t col) { return (key << 8) | (col & 0xFFu); }

// Shared graph cache.  The immutable arrays are laid out once next to the sidecar; every
// process maps graph.cache.bin read-only with MAP_SHARED.  The small header is self-describing,
// and the JSON sidecar records the size and mtime of every source file it was built from.
constexpr uint32_t kCacheVersion = 1;
constexpr uint32_t kMaxCacheSections = 32;
constexpr uint64_t kCacheAlign = 4096;

struct CacheHeader {
    char magic[8];
    uint32_t version;
    uint32_t sections;
    uint64_t bytes;
    uint64_t reserved;
};
struct CacheSection {
    char name[24];
    uint32_t elem_size;
    uint32_t reserved;
    uint64_t offset;
    uint64_t count;
};
static_assert(sizeof(CacheHeader) == 32, "cache header layout");
static_assert(sizeof(CacheSection) == 48, "cache section layout");

uint64_t align_up(uint64_t x, uint64_t a) { return (x + a - 1) / a * a; }

bool stat_source(const std::string& path, uint64_t* size, int64_t* mtime_ns) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    *size = (uint64_t)st.st_size;
    *mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000LL + (int64_t)st.st_mtim.tv_nsec;
    return true;
}

bool write_exact(int fd, const void* data, size_t bytes, uint64_t off) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (bytes) {
        const ssize_t n = ::pwrite(fd, p, bytes, (off_t)off);
        if (n <= 0) return false;
        p += n;
        bytes -= (size_t)n;
        off += (uint64_t)n;
    }
    return true;
}

std::string cache_json(const std::string& dir, uint32_t nv, uint64_t nrel, uint64_t ne,
                       uint32_t nlabels, uint32_t ntypes, uint64_t bytes) {
    const char* files[] = {"meta.json", "vertices.bin", "edges.bin", "props.json", "props.bin"};
    std::ostringstream o;
    o << "{\n \"version\": " << kCacheVersion
      << ",\n \"vertices\": " << nv
      << ",\n \"rels\": " << nrel
      << ",\n \"edges\": " << ne
      << ",\n \"labels\": " << nlabels
      << ",\n \"types\": " << ntypes
      << ",\n \"cache_bytes\": " << bytes
      << ",\n \"source\": {";
    bool first = true;
    for (const char* f : files) {
        uint64_t sz = 0;
        int64_t mt = 0;
        if (!stat_source(dir + "/" + f, &sz, &mt)) {
            o << "\n  \"" << f << "\": null";
            continue;
        }
        if (!first) o << ',';
        first = false;
        o << "\n  \"" << f << "\": {\"size\": " << sz << ", \"mtime_ns\": " << mt << "}";
    }
    o << "\n }\n}\n";
    return o.str();
}
}  // namespace

bool GraphStore::write_cache(const std::string& dir, std::string* err) {
    const uint32_t L = (uint32_t)schema_.labels.size();
    const uint32_t T = (uint32_t)schema_.types.size();
    if (vlabel_.size() != nv_ || vrow_.size() != nv_ ||
        etype_.size() != nrel_ || esrc_row_.size() != nrel_ ||
        edst_row_.size() != nrel_ || erow_.size() != nrel_ || edir_.size() != nrel_ ||
        off_all_.size() != (size_t)nv_ + 1 || nbr_all_.size() != ne_ ||
        rel_all_.size() != ne_ || dir_all_.size() != ne_ ||
        off_type_.size() != T || nbr_type_.size() != T || rel_type_.size() != T ||
        dir_type_.size() != T || id_index_.size() != L) {
        if (err) *err = "graph arrays are not in a complete legacy state";
        return false;
    }

    std::vector<uint32_t> lbl_off((size_t)L + 1, 0);
    for (uint32_t l = 0; l < L; ++l) {
        lbl_off[l + 1] = lbl_off[l] + (uint32_t)lbl_vlist_[l].size();
    }
    if (lbl_off[L] != nv_) {
        if (err) *err = "label lists do not cover every vertex";
        return false;
    }
    std::vector<uint64_t> type_base((size_t)T + 1, 0);
    std::vector<uint64_t> id_off((size_t)L + 1, 0);
    for (uint32_t t = 0; t < T; ++t) {
        if (off_type_[t].size() != (size_t)nv_ + 1 ||
            nbr_type_[t].size() != rel_type_[t].size() ||
            dir_type_[t].size() != rel_type_[t].size()) {
            if (err) *err = "typed view is not materialised";
            return false;
        }
        type_base[t + 1] = type_base[t] + rel_type_[t].size();
    }
    if (type_base[T] != ne_) {
        if (err) *err = "typed lists do not cover every edge";
        return false;
    }
    for (uint32_t l = 0; l < L; ++l)
        id_off[l + 1] = id_off[l] + id_index_[l].size();

    struct Spec { const char* name; uint32_t elem; uint64_t count; };
    const uint64_t type_off_count = (uint64_t)T * ((uint64_t)nv_ + 1);
    const Spec specs[] = {
        {"vlabel", 4, nv_},
        {"vrow", 4, nv_},
        {"etype", 4, nrel_},
        {"esrc", 4, nrel_},
        {"edst", 4, nrel_},
        {"erow", 4, nrel_},
        {"edir", 1, nrel_},
        {"lbl_off", 4, (uint64_t)L + 1},
        {"lbl_vals", 4, nv_},
        {"off_all", 4, (uint64_t)nv_ + 1},
        {"nbr_all", 4, ne_},
        {"rel_all", 4, ne_},
        {"dir_all", 1, ne_},
        {"type_off", 4, type_off_count},
        {"type_nbr", 4, ne_},
        {"type_dir", 1, ne_},
        {"type_rel", 4, ne_},
        {"type_base", 8, (uint64_t)T + 1},
        {"id_off", 8, (uint64_t)L + 1},
        {"id_entries", (uint32_t)sizeof(IdEntry), id_off[L]},
    };
    const uint32_t nsec = (uint32_t)(sizeof(specs) / sizeof(specs[0]));
    static_assert(sizeof(specs) / sizeof(specs[0]) <= kMaxCacheSections, "cache sections");
    std::vector<CacheSection> desc(nsec);
    uint64_t off = align_up(sizeof(CacheHeader) + (uint64_t)nsec * sizeof(CacheSection), kCacheAlign);
    for (uint32_t i = 0; i < nsec; ++i) {
        std::memset(&desc[i], 0, sizeof desc[i]);
        std::strncpy(desc[i].name, specs[i].name, sizeof desc[i].name - 1);
        desc[i].elem_size = specs[i].elem;
        desc[i].offset = off;
        desc[i].count = specs[i].count;
        const uint64_t bytes = specs[i].count * specs[i].elem;
        if (bytes > std::numeric_limits<uint64_t>::max() - off) {
            if (err) *err = "cache size overflow";
            return false;
        }
        off = align_up(off + bytes, kCacheAlign);
    }
    const uint64_t file_bytes = off;
    CacheHeader h;
    std::memset(&h, 0, sizeof h);
    std::memcpy(h.magic, "V3GRPH1", 7);
    h.version = kCacheVersion;
    h.sections = nsec;
    h.bytes = file_bytes;

    const std::string cache_path = dir + "/graph.cache.bin";
    const std::string meta_path = dir + "/graph.cache.meta.json";
    const std::string tmp_path = cache_path + ".tmp." + std::to_string((long long)::getpid());
    const std::string tmp_meta = meta_path + ".tmp." + std::to_string((long long)::getpid());
    int fd = ::open(tmp_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        if (err) *err = "cannot create " + tmp_path + ": " + std::strerror(errno);
        return false;
    }
    bool ok = write_exact(fd, &h, sizeof h, 0) &&
              write_exact(fd, desc.data(), desc.size() * sizeof(CacheSection), sizeof h);
    auto wb = [&](uint32_t i, const void* p, uint64_t count, uint64_t extra_off = 0) {
        return ok && write_exact(fd, p, (size_t)(count * desc[i].elem_size),
                                 desc[i].offset + extra_off);
    };
    if (ok) ok = wb(0, vlabel_.data(), nv_) && wb(1, vrow_.data(), nv_);
    if (ok) ok = wb(2, etype_.data(), nrel_) && wb(3, esrc_row_.data(), nrel_) &&
                     wb(4, edst_row_.data(), nrel_) && wb(5, erow_.data(), nrel_) &&
                     wb(6, edir_.data(), nrel_);
    if (ok) ok = wb(7, lbl_off.data(), (uint64_t)L + 1);
    if (ok) {
        for (uint32_t l = 0; l < L; ++l)
            if (!wb(8, lbl_vlist_[l].data(), lbl_vlist_[l].size(),
                    (uint64_t)lbl_off[l] * 4)) { ok = false; break; }
    }
    if (ok) ok = wb(9, off_all_.data(), (uint64_t)nv_ + 1);
    if (ok) ok = wb(10, nbr_all_.data(), ne_) && wb(11, rel_all_.data(), ne_) &&
                     wb(12, dir_all_.data(), ne_);
    if (ok) {
        for (uint32_t t = 0; t < T; ++t) {
            if (!wb(13, off_type_[t].data(), (uint64_t)nv_ + 1,
                    (uint64_t)t * ((uint64_t)nv_ + 1) * 4) ||
                !wb(14, nbr_type_[t].data(), nbr_type_[t].size(), type_base[t] * 4) ||
                !wb(15, dir_type_[t].data(), dir_type_[t].size(), type_base[t]) ||
                !wb(16, rel_type_[t].data(), rel_type_[t].size(), type_base[t] * 4)) {
                ok = false;
                break;
            }
        }
    }
    if (ok) ok = wb(17, type_base.data(), (uint64_t)T + 1);
    if (ok) ok = wb(18, id_off.data(), (uint64_t)L + 1);
    if (ok) {
        for (uint32_t l = 0; l < L; ++l)
            if (!wb(19, id_index_[l].data(), id_index_[l].size(),
                    id_off[l] * sizeof(IdEntry))) { ok = false; break; }
    }
    if (ok) ok = ::ftruncate(fd, (off_t)file_bytes) == 0 && ::fsync(fd) == 0;
    if (::close(fd) != 0) ok = false;
    if (!ok) {
        ::unlink(tmp_path.c_str());
        if (err) *err = "failed writing " + tmp_path + ": " + std::strerror(errno);
        return false;
    }
    {
        std::ofstream mo(tmp_meta, std::ios::binary | std::ios::trunc);
        mo << cache_json(dir, nv_, nrel_, ne_, L, T, file_bytes);
        if (!mo) {
            ::unlink(tmp_path.c_str());
            ::unlink(tmp_meta.c_str());
            if (err) *err = "cannot write " + tmp_meta;
            return false;
        }
    }
    if (::rename(tmp_path.c_str(), cache_path.c_str()) != 0 ||
        ::rename(tmp_meta.c_str(), meta_path.c_str()) != 0) {
        ::unlink(tmp_path.c_str());
        ::unlink(tmp_meta.c_str());
        if (err) *err = "cannot install graph cache: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

bool GraphStore::load_shared_cache(const std::string& dir, std::string* err) {
    const std::string mp = dir + "/graph.cache.meta.json";
    std::string txt;
    if (!read_file(mp, &txt)) return false;
    json::J m;
    if (!json::parse(txt, &m, err)) return false;
    if ((uint32_t)m.intv("version", -1) != kCacheVersion) {
        if (err) *err = "graph cache version mismatch in " + mp;
        return false;
    }
    const uint64_t cache_ne = (uint64_t)m.intv("edges", -1);
    if ((uint32_t)m.intv("vertices", -1) != nv_ ||
        (uint64_t)m.intv("rels", -1) != nrel_ ||
        (uint32_t)m.intv("labels", -1) != (uint32_t)schema_.labels.size() ||
        (uint32_t)m.intv("types", -1) != (uint32_t)schema_.types.size() ||
        cache_ne > nrel_) {
        if (err) *err = "graph cache counts disagree with sidecar meta";
        return false;
    }
    ne_ = cache_ne;
    const json::J* src = m.get("source");
    if (!src) {
        if (err) *err = "graph cache meta has no source table";
        return false;
    }
    const char* files[] = {"meta.json", "vertices.bin", "edges.bin", "props.json", "props.bin"};
    for (const char* f : files) {
        const json::J* e = src->get(f);
        uint64_t sz = 0;
        int64_t mt = 0;
        if (!e || !stat_source(dir + "/" + f, &sz, &mt) ||
            (uint64_t)e->intv("size", -1) != sz ||
            (int64_t)e->intv("mtime_ns", -1) != mt) {
            if (err) *err = std::string("graph cache stale for ") + f;
            return false;
        }
    }

    const std::string cp = dir + "/graph.cache.bin";
    int fd = ::open(cp.c_str(), O_RDONLY);
    if (fd < 0) {
        if (err) *err = "cannot open " + cp;
        return false;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(CacheHeader)) {
        ::close(fd);
        if (err) *err = "bad graph cache size";
        return false;
    }
    const uint64_t bytes = (uint64_t)st.st_size;
    if ((uint64_t)m.intv("cache_bytes", -1) != bytes) {
        ::close(fd);
        if (err) *err = "graph cache size disagrees with meta";
        return false;
    }
    // The graph page-in is demand-driven by default.  Touching every page here made the
    // subsequent random accesses slower on the 15 GB SF10 cache, so prefault is opt-in.
    void* base = ::mmap(nullptr, (size_t)bytes, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (base == MAP_FAILED) {
        if (err) *err = "cannot mmap " + cp;
        return false;
    }
    const CacheHeader* h = static_cast<const CacheHeader*>(base);
    if (std::memcmp(h->magic, "V3GRPH1", 7) != 0 || h->version != kCacheVersion ||
        h->sections > kMaxCacheSections || h->bytes != bytes) {
        ::munmap(base, (size_t)bytes);
        if (err) *err = "bad graph cache header";
        return false;
    }
    const CacheSection* desc = reinterpret_cast<const CacheSection*>(
        static_cast<const uint8_t*>(base) + sizeof(CacheHeader));
    std::unordered_map<std::string, const CacheSection*> by_name;
    for (uint32_t i = 0; i < h->sections; ++i) by_name.emplace(desc[i].name, &desc[i]);
    auto sec = [&](const char* name, uint32_t elem, uint64_t count) -> const uint8_t* {
        auto it = by_name.find(name);
        if (it == by_name.end() || it->second->elem_size != elem ||
            it->second->count != count ||
            (it->second->offset & (kCacheAlign - 1)) != 0 ||
            it->second->offset > bytes ||
            count * elem > bytes - it->second->offset) {
            if (err) *err = std::string("graph cache section mismatch: ") + name;
            return nullptr;
        }
        return static_cast<const uint8_t*>(base) + it->second->offset;
    };
    const uint32_t L = (uint32_t)schema_.labels.size();
    const uint32_t T = (uint32_t)schema_.types.size();
    const uint64_t type_off_count = (uint64_t)T * ((uint64_t)nv_ + 1);
    const uint8_t* p[20];
    p[0] = sec("vlabel", 4, nv_); p[1] = sec("vrow", 4, nv_);
    p[2] = sec("etype", 4, nrel_); p[3] = sec("esrc", 4, nrel_);
    p[4] = sec("edst", 4, nrel_); p[5] = sec("erow", 4, nrel_);
    p[6] = sec("edir", 1, nrel_); p[7] = sec("lbl_off", 4, (uint64_t)L + 1);
    p[8] = sec("lbl_vals", 4, nv_); p[9] = sec("off_all", 4, (uint64_t)nv_ + 1);
    p[10] = sec("nbr_all", 4, ne_); p[11] = sec("rel_all", 4, ne_);
    p[12] = sec("dir_all", 1, ne_); p[13] = sec("type_off", 4, type_off_count);
    p[14] = sec("type_nbr", 4, ne_); p[15] = sec("type_dir", 1, ne_);
    p[16] = sec("type_rel", 4, ne_); p[17] = sec("type_base", 8, (uint64_t)T + 1);
    p[18] = sec("id_off", 8, (uint64_t)L + 1);
    for (uint32_t i = 0; i < 19; ++i) {
        if (!p[i]) {
            ::munmap(base, (size_t)bytes);
            return false;
        }
    }
    const uint64_t* id_off_p = reinterpret_cast<const uint64_t*>(p[18]);
    for (uint32_t l = 0; l < L; ++l) {
        if (id_off_p[l] > id_off_p[l + 1]) {
            ::munmap(base, (size_t)bytes);
            if (err) *err = "graph cache id offsets are not monotone";
            return false;
        }
    }
    const uint8_t* id_entries = sec("id_entries", (uint32_t)sizeof(IdEntry), id_off_p[L]);
    if (!id_entries) {
        ::munmap(base, (size_t)bytes);
        return false;
    }
    vlabel_.view(reinterpret_cast<const uint32_t*>(p[0]), nv_);
    vrow_.view(reinterpret_cast<const uint32_t*>(p[1]), nv_);
    etype_.view(reinterpret_cast<const uint32_t*>(p[2]), nrel_);
    esrc_row_.view(reinterpret_cast<const uint32_t*>(p[3]), nrel_);
    edst_row_.view(reinterpret_cast<const uint32_t*>(p[4]), nrel_);
    erow_.view(reinterpret_cast<const uint32_t*>(p[5]), nrel_);
    edir_.view(p[6], nrel_);
    lbl_off_.view(reinterpret_cast<const uint32_t*>(p[7]), (size_t)L + 1);
    lbl_vals_.view(reinterpret_cast<const uint32_t*>(p[8]), nv_);
    off_all_.view(reinterpret_cast<const uint32_t*>(p[9]), (size_t)nv_ + 1);
    nbr_all_.view(reinterpret_cast<const uint32_t*>(p[10]), ne_);
    rel_all_.view(reinterpret_cast<const uint32_t*>(p[11]), ne_);
    dir_all_.view(p[12], ne_);
    type_off_concat_.view(reinterpret_cast<const uint32_t*>(p[13]), type_off_count);
    type_nbr_concat_.view(reinterpret_cast<const uint32_t*>(p[14]), ne_);
    type_dir_concat_.view(p[15], ne_);
    type_rel_concat_.view(reinterpret_cast<const uint32_t*>(p[16]), ne_);
    type_base_.view(reinterpret_cast<const uint64_t*>(p[17]), (size_t)T + 1);
    id_off_.view(reinterpret_cast<const uint64_t*>(p[18]), (size_t)L + 1);
    if (lbl_off_[L] != nv_ || off_all_[nv_] != ne_ || type_base_[T] != ne_) {
        ::munmap(base, (size_t)bytes);
        if (err) *err = "graph cache final offsets disagree with counts";
        return false;
    }
    id_index_.assign((size_t)L, GraphArray<IdEntry>());
    for (uint32_t l = 0; l < L; ++l) {
        const uint64_t n = id_off_[l + 1] - id_off_[l];
        id_index_[l].view(reinterpret_cast<const IdEntry*>(id_entries) + id_off_[l], n);
    }
    cache_map_ = base;
    cache_bytes_ = (size_t)bytes;
    bind_shared_views();
    typed_built_.assign(T, 1);
    if (getenv("V3_GRAPH_CACHE_PREFAULT") != nullptr) {
        const long pages = (long)sysconf(_SC_PAGESIZE);
        const size_t pg = (size_t)(pages > 0 ? pages : 4096);
        volatile uint8_t sink = 0;
        ::madvise(base, (size_t)bytes, MADV_NORMAL);
        for (size_t off = 0; off < (size_t)bytes; off += pg)
            sink ^= static_cast<const uint8_t*>(base)[off];
        (void)sink;
        ::madvise(base, (size_t)bytes, MADV_RANDOM);
    }
    return true;
}

void GraphStore::bind_shared_views() {
    const uint32_t L = (uint32_t)schema_.labels.size();
    const uint32_t T = (uint32_t)schema_.types.size();
    lbl_vlist_.assign((size_t)L, GraphArray<uint32_t>());
    for (uint32_t l = 0; l < L; ++l) {
        const uint32_t b = lbl_off_[l], e = lbl_off_[l + 1];
        lbl_vlist_[l].view(lbl_vals_.data() + b, (size_t)e - b);
    }
    off_type_.assign((size_t)T, GraphArray<uint32_t>());
    nbr_type_.assign((size_t)T, GraphArray<uint32_t>());
    rel_type_.assign((size_t)T, GraphArray<uint32_t>());
    dir_type_.assign((size_t)T, GraphArray<uint8_t>());
    for (uint32_t t = 0; t < T; ++t) {
        off_type_[t].view(type_off_concat_.data() + (size_t)t * ((size_t)nv_ + 1),
                          (size_t)nv_ + 1);
        const uint64_t b = type_base_[t], e = type_base_[t + 1];
        nbr_type_[t].view(type_nbr_concat_.data() + b, (size_t)(e - b));
        dir_type_[t].view(type_dir_concat_.data() + b, (size_t)(e - b));
        rel_type_[t].view(type_rel_concat_.data() + b, (size_t)(e - b));
    }
}

bool GraphStore::load(const std::string& dir, std::string* err, bool meta_only) {
    std::string meta;
    if (!read_file(dir + "/meta.json", &meta)) {
        if (err) *err = "cannot read " + dir + "/meta.json";
        return false;
    }
    json::J m;
    if (!json::parse(meta, &m, err)) return false;
    // the label / type ids are the ones of the v2 export: vertex label id = position in the
    // store order, type id = position in the frequency order; both lists are in meta.json
    // (`label_ids`, `type_ids`, each {id, name}) and the property blocks follow them
    auto ids_of = [&](const char* key, std::vector<std::string>* out) {
        std::vector<std::pair<uint32_t, std::string>> v;
        if (const json::J* e = m.get(key))
            for (const auto& x : e->a) v.emplace_back((uint32_t)x.intv("id"), x.str("name"));
        std::sort(v.begin(), v.end());
        for (const auto& x : v) out->push_back(x.second);
    };
    std::vector<std::string> label_ids, type_ids;
    ids_of("label_ids", &label_ids);
    ids_of("type_ids", &type_ids);
    for (const auto& n : label_ids) {
        LabelDef d;
        d.name = n;
        schema_.label_id[n] = (uint32_t)schema_.labels.size();
        schema_.labels.push_back(std::move(d));
    }
    for (const auto& n : type_ids) {
        TypeDef d;
        d.name = n;
        schema_.type_id[n] = (uint32_t)schema_.types.size();
        schema_.types.push_back(std::move(d));
    }
    if (const json::J* ls = m.get("labels")) {
        for (const auto& l : ls->a) {
            auto it = schema_.label_id.find(l.str("name"));
            if (it == schema_.label_id.end()) continue;
            if (l.has("id") && (uint32_t)l.intv("id") != (uint32_t)it->second) {
                if (err) *err = "label id of " + l.str("name") + " differs";
                return false;
            }
            LabelDef& d = schema_.labels[it->second];
            if (const json::J* ps = l.get("props")) {
                for (const auto& p : ps->a) {
                    const uint32_t col = (uint32_t)p.intv("col");
                    d.prop_col[p.str("name")] = col;
                    d.nprops = std::max(d.nprops, col + 1);
                }
            }
        }
    }
    if (const json::J* ts = m.get("types")) {
        for (const auto& t : ts->a) {
            auto it = schema_.type_id.find(t.str("name"));
            if (it == schema_.type_id.end()) continue;
            TypeDef& d = schema_.types[it->second];
            if (const json::J* ps = t.get("props")) {
                for (const auto& p : ps->a) {
                    const uint32_t col = (uint32_t)p.intv("col");
                    d.prop_col[p.str("name")] = col;
                    d.nprops = std::max(d.nprops, col + 1);
                }
            }
        }
    }
    nv_ = (uint32_t)m.intv("vertices");
    nrel_ = (uint64_t)m.intv("rels");
    if (meta_only) {
        // Planning only needs each label's cardinality; fill the label lists with that many
        // entries and skip vertices.bin / edges.bin / props.bin.
        lbl_vlist_.assign(schema_.labels.size(), GraphArray<uint32_t>());
        if (const json::J* ls = m.get("labels")) {
            for (const auto& l : ls->a) {
                auto it = schema_.label_id.find(l.str("name"));
                if (it == schema_.label_id.end()) continue;
                const uint64_t rows = (uint64_t)l.intv("rows");
                lbl_vlist_[(size_t)it->second].assign((size_t)rows, 0u);
            }
        }
        return true;
    }
    bool cache_loaded = false;
    const std::string cache_meta = dir + "/graph.cache.meta.json";
    if (getenv("V3_GRAPH_CACHE_OFF") == nullptr && ::access(cache_meta.c_str(), R_OK) == 0) {
        if (!load_shared_cache(dir, err)) return false;
        cache_loaded = true;
    } else {
        {
            // vertices.bin: <label, row> per vertex (read-only, no private copy)
            MapU32 lv;
            if (!open_map(dir + "/vertices.bin", (size_t)nv_ * 2, &lv, err)) return false;
            maps().push_back(lv);
            // the map() vector has moved its storage: keep a stable copy of the pointer table
            lv = maps().back();
            vlabel_.resize(nv_);
            vrow_.resize(nv_);
            for (uint32_t k = 0; k < nv_; ++k) {
                vlabel_[k] = lv.p[2 * k];
                vrow_[k] = lv.p[2 * k + 1];
                if (vlabel_[k] >= schema_.labels.size()) {
                    if (err) *err = "vertex label out of range";
                    return false;
                }
            }
        }
        {
            // edges.bin: <src, dst, type, row, dir> per direction
            MapU32 e;
            if (!open_map(dir + "/edges.bin", (size_t)nrel_ * 5, &e, err)) return false;
            maps().push_back(e);
            e = maps().back();
            etype_.resize(nrel_);
            esrc_row_.resize(nrel_);
            edst_row_.resize(nrel_);
            erow_.resize(nrel_);
            edir_.resize(nrel_);
            for (uint64_t k = 0; k < nrel_; ++k) {
                esrc_row_[k] = e.p[5 * k];
                edst_row_[k] = e.p[5 * k + 1];
                etype_[k] = e.p[5 * k + 2];
                erow_[k] = e.p[5 * k + 3];
                edir_[k] = (uint8_t)e.p[5 * k + 4];
                if (esrc_row_[k] >= nv_ || etype_[k] >= schema_.types.size()) {
                    if (err) *err = "edge out of range at rel " + std::to_string(k);
                    return false;
                }
            }
        }
        ne_ = 0;
        for (uint64_t k = 0; k < nrel_; ++k)
            if (edst_row_[k] != kNil) ++ne_;
        build_views();
    }

    const std::string pp = dir + "/props.bin";
    {
        // props.json: the column blocks in file order (key * 256 + col of the segment)
        std::string pj;
        if (!read_file(dir + "/props.json", &pj)) {
            if (err) *err = "cannot read " + dir + "/props.json";
            return false;
        }
        json::J j;
        if (!json::parse(pj, &j, err)) return false;
        const json::J* segs = j.get("segments");
        if (!segs) {
            if (err) *err = "props.json without segments";
            return false;
        }
        for (const auto& s : segs->a) {
            const uint32_t seg = seg_of((uint32_t)s.intv("key"), (uint32_t)s.intv("col"));
            if (seg >= cols_.size()) cols_.resize(seg + 1);
            cols_[seg].rows = (uint64_t)s.intv("rows");
            cols_[seg].width = (uint64_t)s.intv("width");
            cols_[seg].off = (uint64_t)s.intv("off", -1);
        }
    }
    map_path_ = pp;
    size_t off = 0;
    struct stat st;
    if (::stat(pp.c_str(), &st) != 0) {
        if (err) *err = "cannot stat " + pp;
        return false;
    }
    map_bytes_ = (size_t)st.st_size;
    // The property file is mapped one column block at a time, on first use.  SF10 is 60 GiB of
    // strings; mapping the whole file would exceed a 64 GiB address-space cap before the first
    // query even though a round touches only a few columns.  Each block is page-aligned and stays
    // mapped for the process lifetime (the page cache is shared by all rounds on the host).
    for (auto& c : cols_) {
        c.data = nullptr;
        c.mapped = false;
        c.len = (uint64_t)(c.rows * c.width) + (uint64_t)(c.rows / 8 + 1);
        off = std::max(off, (size_t)c.off + (size_t)c.len);
    }
    if (off != map_bytes_) {
        if (err) *err = "props.bin size " + std::to_string(map_bytes_) + " does not match the column table " +
                        std::to_string(off);
        return false;
    }
    // The id index is graph metadata, built once at load for every label that has an `id`
    // column.  A cache-backed load already mapped it; the legacy path builds it here.
    if (!cache_loaded && !build_id_index(err)) return false;
    return true;
}

void GraphStore::map_col(const ColStore& c) const {
    if (c.mapped) return;
    const long pages = (long)sysconf(_SC_PAGESIZE);
    const size_t pg = (size_t)pages;
    const size_t base = (size_t)c.off & ~(pg - 1);
    const size_t delta = (size_t)c.off - base;
    const size_t len = (size_t)c.len + delta;
    int fd = ::open(map_path_.c_str(), O_RDONLY);
    if (fd >= 0) {
        void* p = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, (off_t)base);
        ::close(fd);
        if (p != MAP_FAILED) {
            c.data = reinterpret_cast<const uint8_t*>(p) + delta;
            c.mapped = true;
            madvise(p, len, MADV_RANDOM);
            return;
        }
    }
    c.data = nullptr;
    c.mapped = true;   // a missing block reads as an out-of-range property (the caller's error)
}

void GraphStore::build_views() {
    const uint32_t L = (uint32_t)schema_.labels.size(), T = (uint32_t)schema_.types.size();
    lbl_vlist_.assign(L, GraphArray<uint32_t>());
    {
        std::vector<uint32_t> cnt(L, 0);
        for (uint32_t v = 0; v < nv_; ++v) ++cnt[vlabel_[v]];
        for (uint32_t l = 0; l < L; ++l) lbl_vlist_[l].reserve(cnt[l]);
        for (uint32_t v = 0; v < nv_; ++v) lbl_vlist_[vlabel_[v]].push_back(v);
    }
    // Every relationship is stored twice (each direction once) and indexed by its source.
    std::vector<uint32_t> deg(nv_ + 1, 0);
    for (uint64_t k = 0; k < nrel_; ++k)
        if (edst_row_[k] != kNil) ++deg[esrc_row_[k] + 1];
    for (uint32_t v = 0; v < nv_; ++v) deg[v + 1] += deg[v];
    off_all_.assign(std::move(deg));
    nbr_all_.assign((size_t)ne_, 0u);
    rel_all_.assign((size_t)ne_, 0u);
    dir_all_.assign((size_t)ne_, 0u);
    {
        std::vector<uint32_t> pos(off_all_.begin(), off_all_.end() - 1);
        for (uint64_t k = 0; k < nrel_; ++k) {
            if (edst_row_[k] == kNil) continue;
            const uint32_t u = esrc_row_[k];
            nbr_all_[pos[u]] = edst_row_[k];
            rel_all_[pos[u]] = (uint32_t)k;
            dir_all_[pos[u]] = edir_[k];
            ++pos[u];
        }
    }
    off_type_.assign(T, GraphArray<uint32_t>());
    nbr_type_.assign(T, GraphArray<uint32_t>());
    rel_type_.assign(T, GraphArray<uint32_t>());
    dir_type_.assign(T, GraphArray<uint8_t>());
    std::vector<uint64_t> tcount(T + 1, 0);
    for (uint64_t k = 0; k < nrel_; ++k)
        if (edst_row_[k] != kNil) ++tcount[etype_[k] + 1];
    for (uint32_t t = 0; t < T; ++t) tcount[t + 1] += tcount[t];
    std::vector<uint32_t> packed((size_t)ne_);
    {
        std::vector<uint64_t> pos(tcount.begin(), tcount.end() - 1);
        for (uint64_t k = 0; k < nrel_; ++k) {
            if (edst_row_[k] == kNil) continue;
            packed[(size_t)pos[etype_[k]]++] = (uint32_t)k;
        }
    }
    // Typed adjacency is materialised on first use (build_type): a round touches a handful of
    // relationship types, and building all fifteen over SF10 costs seconds and ~2 GiB of offsets
    // per process before the first query runs.  The packed relationship-id list is shared.
    typed_built_.assign(T, 0);
    for (uint32_t t = 0; t < T; ++t) {
        rel_type_[t].assign((size_t)(tcount[t + 1] - tcount[t]), 0u);
        for (uint64_t p = tcount[t]; p < tcount[t + 1]; ++p)
            rel_type_[t][(size_t)(p - tcount[t])] = packed[(size_t)p];
    }
}

void GraphStore::build_type(uint32_t t) const {
    if (typed_built_[t]) return;
    typed_built_[t] = 1;
    const GraphArray<uint32_t>& rels = rel_type_[t];
    std::vector<uint32_t> d((size_t)nv_ + 1, 0);
    for (uint32_t r : rels) ++d[(size_t)esrc_row_[r] + 1];
    for (uint32_t v = 0; v < nv_; ++v) d[(size_t)v + 1] += d[(size_t)v];
    std::vector<uint32_t> ps(d.begin(), d.end() - 1);
    off_type_[t].assign(std::move(d));
    nbr_type_[t].assign(rels.size(), 0u);
    dir_type_[t].assign(rels.size(), 0u);
    for (uint32_t r : rels) {
        const uint32_t u = esrc_row_[r];
        nbr_type_[t][ps[u]] = edst_row_[r];
        dir_type_[t][ps[u]] = edir_[r];
        ++ps[u];
    }
}

// Lazy sorted adjacency for a bound-bound edge test.  Only the vertices a close edge actually
// visits are indexed, so the memory stays with the close edges of the round and not with the
// whole graph.  The cap keeps one process's index below SORTED_ADJ_CAP entries.
const std::vector<std::pair<uint32_t, uint32_t>>* GraphStore::sorted_adj(uint32_t v) const {
    constexpr size_t kCap = 4u << 20;   // 4M (neighbour, rel) pairs = 32 MB
    auto it = sorted_adj_.find(v);
    if (it != sorted_adj_.end()) return &it->second;
    const uint32_t lo = off_all_[v], hi = off_all_[v + 1];
    if (sorted_entries_ + (size_t)(hi - lo) > kCap) return nullptr;
    std::vector<std::pair<uint32_t, uint32_t>> a;
    a.reserve((size_t)(hi - lo));
    for (uint32_t k = lo; k < hi; ++k) a.emplace_back(nbr_all_[k], rel_all_[k]);
    std::sort(a.begin(), a.end());
    sorted_entries_ += a.size();
    auto ins = sorted_adj_.emplace(v, std::move(a));
    return &ins.first->second;
}

int32_t GraphStore::label_col(uint32_t label, const std::string& prop) const {
    const auto& d = schema_.labels[label];
    if (prop.empty()) return 0;
    auto it = d.prop_col.find(prop);
    return it == d.prop_col.end() ? -1 : (int32_t)it->second;
}

int32_t GraphStore::type_col(uint32_t type, const std::string& prop) const {
    const auto& d = schema_.types[type];
    if (prop.empty()) return 0;
    auto it = d.prop_col.find(prop);
    return it == d.prop_col.end() ? -1 : (int32_t)it->second;
}

Value GraphStore::col_value(uint32_t row, uint32_t key, uint32_t col, bool* is_null) const {
    *is_null = false;
    const uint32_t seg = seg_of(key, col);
    if (seg >= cols_.size() || cols_[seg].width == 0) {
        *is_null = true;
        return Value{};
    }
    const ColStore& c = cols_[seg];
    map_col(c);
    if (!c.data || row >= c.rows) {
        *is_null = true;
        return Value{};
    }
    const uint8_t* nb = c.data + c.rows * c.width;
    if (nb[row >> 3] & (1u << (row & 7))) {   // null bitmap, one bit per row
        *is_null = true;
        return Value{};
    }
    const uint8_t* rec = c.data + (size_t)row * c.width + 8;   // column record after the key
    Value out;
    if (rec[0] == 1) {
        uint32_t len;
        std::memcpy(&len, rec + 1, 4);
        out.s.assign(reinterpret_cast<const char*>(rec + 5), len);
        out.i = 0;
        out.is_str = true;
    } else {
        std::memcpy(&out.i, rec + 1, 8);
    }
    return out;
}

Value GraphStore::vertex_prop(uint32_t v, uint32_t label, uint32_t col, bool* is_null) const {
    return col_value(vrow_[v], label, col, is_null);
}

// Same record layout as col_value, but compares in place: an equality test against a constant
// needs no Value and no string copy.
bool GraphStore::col_eq(uint32_t row, uint32_t key, uint32_t col, const Value& c) const {
    if (c.is_null) return false;
    const uint32_t seg = seg_of(key, col);
    if (seg >= cols_.size() || cols_[seg].width == 0) return false;
    const ColStore& cs = cols_[seg];
    if (!cs.data) map_col(cs);
    if (!cs.data || row >= cs.rows) return false;
    const uint8_t* nb = cs.data + cs.rows * cs.width;
    if (nb[row >> 3] & (1u << (row & 7))) return false;
    const uint8_t* rec = cs.data + (size_t)row * cs.width + 8;
    if (rec[0] == 1) {
        if (!c.is_str) return false;
        uint32_t len = 0;
        std::memcpy(&len, rec + 1, 4);
        return (size_t)len == c.s.size() &&
               std::memcmp(reinterpret_cast<const char*>(rec + 5), c.s.data(), len) == 0;
    }
    if (c.is_str) return false;
    int64_t x = 0;
    std::memcpy(&x, rec + 1, 8);
    return x == c.i;
}

bool GraphStore::vertex_prop_eq(uint32_t v, uint32_t label, uint32_t col, const Value& c) const {
    return col_eq(vrow_[v], label, col, c);
}

bool GraphStore::edge_prop_eq(uint32_t rel, uint32_t type, uint32_t col, const Value& c) const {
    return col_eq(erow_[rel], (uint32_t)schema_.labels.size() + type, col, c);
}

namespace {
inline uint64_t hash_bytes(const char* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= (uint8_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}
inline uint64_t hash_value(const Value& v) {
    if (v.is_str) return hash_bytes(v.s.data(), v.s.size()) ^ 0x9e3779b97f4a7c15ULL;
    uint64_t x = (uint64_t)v.i;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}
}  // namespace

bool GraphStore::col_hash(uint32_t row, uint32_t key, uint32_t col, uint64_t* h) const {
    const uint32_t seg = seg_of(key, col);
    if (seg >= cols_.size() || cols_[seg].width == 0) return false;
    const ColStore& cs = cols_[seg];
    if (!cs.data) map_col(cs);
    if (!cs.data || row >= cs.rows) return false;
    const uint8_t* nb = cs.data + cs.rows * cs.width;
    if (nb[row >> 3] & (1u << (row & 7))) return false;
    const uint8_t* rec = cs.data + (size_t)row * cs.width + 8;
    if (rec[0] == 1) {
        uint32_t len = 0;
        std::memcpy(&len, rec + 1, 4);
        *h = hash_bytes(reinterpret_cast<const char*>(rec + 5), len) ^ 0x9e3779b97f4a7c15ULL;
    } else {
        int64_t x = 0;
        std::memcpy(&x, rec + 1, 8);
        Value v;
        v.i = x;
        *h = hash_value(v);
    }
    return true;
}

void GraphStore::warm_prop_index(uint32_t label, uint32_t col) const {
    const uint64_t key = ((uint64_t)label << 32) | col;
    PropIndex& idx = prop_idx_[key];
    if (idx.built) return;
    idx.built = true;
    const GraphArray<uint32_t>& vs = vertices_of(label);
    idx.e.clear();
    idx.e.reserve(vs.size());
    for (uint32_t v : vs) {
        uint64_t h = 0;
        if (!col_hash(vrow_[v], label, col, &h)) continue;
        idx.e.push_back(PropEntry{h, v});
    }
    std::sort(idx.e.begin(), idx.e.end(), [](const PropEntry& a, const PropEntry& b) {
        return a.h != b.h ? a.h < b.h : a.v < b.v;
    });
}

void GraphStore::prop_lookup(uint32_t label, uint32_t col, const Value& value,
                             std::vector<uint32_t>* out) const {
    if (value.is_null) return;
    warm_prop_index(label, col);
    const uint64_t key = ((uint64_t)label << 32) | col;
    const PropIndex& idx = prop_idx_[key];
    const uint64_t h = hash_value(value);
    auto it = std::lower_bound(idx.e.begin(), idx.e.end(), h,
                               [](const PropEntry& e, uint64_t x) { return e.h < x; });
    for (; it != idx.e.end() && it->h == h; ++it) {
        // The hash is only a bucket; the stored record decides equality.
        if (col_eq(vrow_[it->v], label, col, value)) out->push_back(it->v);
    }
}

Value GraphStore::edge_prop(uint32_t rel, uint32_t type, uint32_t col, bool* is_null) const {
    // Edge property blocks use key = number of vertex labels + type id.  The original LDBC
    // sidecars keyed them by type id alone, which aliases a label id; they carry no edge
    // property blocks, so the offset is backward compatible.
    return col_value(erow_[rel], (uint32_t)schema_.labels.size() + type, col, is_null);
}

int64_t GraphStore::vertex_id(uint32_t v) const {
    const uint32_t lab = vlabel_[v];
    const int32_t col = label_col(lab, "id");
    if (col < 0) return kNull;
    bool is_null = false;
    const Value x = vertex_prop(v, lab, (uint32_t)col, &is_null);
    return (is_null || x.is_str) ? kNull : x.i;
}

const GraphArray<GraphStore::IdEntry>& GraphStore::id_index(uint32_t label) const {
    static const GraphArray<IdEntry> empty;
    if (label >= id_index_.size()) return empty;
    return id_index_[label];
}

bool GraphStore::lookup_vertex(uint32_t label, int64_t id, uint32_t* v) const {
    const GraphArray<IdEntry>& idx = id_index(label);
    auto it = std::lower_bound(idx.begin(), idx.end(), id,
                               [](const IdEntry& e, int64_t x) { return e.id < x; });
    if (it == idx.end() || it->id != id) return false;
    if (v) *v = it->v;
    return true;
}

bool GraphStore::build_id_index(std::string* err) {
    (void)err;
    id_index_.assign(schema_.labels.size(), GraphArray<IdEntry>());
    for (uint32_t l = 0; l < schema_.labels.size(); ++l) {
        const LabelDef& d = schema_.labels[l];
        const auto it = d.prop_col.find("id");
        if (it == d.prop_col.end()) continue;
        const uint32_t col = it->second;
        const GraphArray<uint32_t>& vs = vertices_of(l);
        GraphArray<IdEntry>& idx = id_index_[l];
        idx.reserve(vs.size());
        for (uint32_t v : vs) {
            bool is_null = false;
            const Value x = vertex_prop(v, l, col, &is_null);
            if (is_null || x.is_str) continue;
            idx.push_back(IdEntry{x.i, v});
        }
        std::sort(idx.begin(), idx.end(),
                  [](const IdEntry& a, const IdEntry& b) { return a.id < b.id; });
    }
    return true;
}

void GraphStore::warm_query(const Query& q) const {
    auto warm = [&](uint32_t key, uint32_t nprops) {
        for (uint32_t col = 0; col < nprops; ++col) {
            const uint32_t seg = seg_of(key, col);
            if (seg < cols_.size() && cols_[seg].rows) map_col(cols_[seg]);
        }
    };
    for (const auto& v : q.vars) {
        if (v.kind != PatternVar::VERTEX) continue;
        for (const auto& L : v.labels) {
            const int32_t lid = schema_.label_of(L);
            if (lid >= 0) warm((uint32_t)lid, schema_.labels[(size_t)lid].nprops);
        }
    }
    for (const auto& e : q.edges) {
        for (const auto& T : e.types) {
            const int32_t tid = schema_.type_of(T);
            if (tid >= 0) warm((uint32_t)schema_.labels.size() + (uint32_t)tid,
                               schema_.types[(size_t)tid].nprops);
        }
    }
}

int64_t GraphStore::edge_id(uint32_t rel) const {
    const uint32_t t = etype_[rel];
    const int32_t col = type_col(t, "id");
    if (col < 0) return kNull;
    bool is_null = false;
    const Value x = edge_prop(rel, t, (uint32_t)col, &is_null);
    return (is_null || x.is_str) ? kNull : x.i;
}

bool GraphStore::validate(std::string* err) const {
    for (uint32_t v = 0; v < nv_; ++v) {
        for (uint32_t k = off_all_[v]; k < off_all_[v + 1]; ++k)
            if (esrc_row_[rel_all_[k]] != v || edst_row_[rel_all_[k]] == kNil) {
                if (err) *err = "rel src mismatch at vertex " + std::to_string(v);
                return false;
            }
    }
    for (uint32_t t = 0; t < schema_.types.size(); ++t) {
        if (off_type_[t].size() != (size_t)nv_ + 1 || nbr_type_[t].size() != off_type_[t][nv_]) {
            if (err) *err = "type view size";
            return false;
        }
        for (uint32_t v = 0; v < nv_; ++v)
            for (uint32_t k = off_type_[t][v]; k < off_type_[t][v + 1]; ++k)
                if (etype_[rel_type_[t][k]] != t || esrc_row_[rel_type_[t][k]] != v) {
                    if (err) *err = "type view mismatch";
                    return false;
                }
    }
    return true;
}

}  // namespace v3
