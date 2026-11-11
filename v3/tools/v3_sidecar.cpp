// v3_sidecar: build the v3 graph sidecar for one LDBC scale factor from the LDBC CSV export.
//
//   --raw DIR    the LDBC export root (dynamic/ and static/*.csv, CsvComposite)
//   --ids FILE   the v2 export's <graph>.ids.json (vertex index of every LDBC row per label)
//   --out DIR    sidecar: meta.json, props.json, vertices.bin, edges.bin, props.bin
//
// Encoding (v3_graph.h reads it):
//   vertices.bin  per vertex in label order: <u32 label><u32 row>; row = the CSV data row
//   edges.bin     per direction: <u32 src><u32 dst><u32 type><u32 row><u32 dir>, sorted by (src, dst);
//                 dir = 1 on the CSV direction, 0 on its reverse copy
//   props.bin     per (label, column) and per (type, column): 8-byte key + cell (int: [0][i64],
//                 string: [1][u32 len][bytes]) rows, then a null bitmap of one bit per row
//
// Columns are streamed column-wise (a whole-table read per column, written immediately), so
// memory stays at one column of one table (the earlier version kept every column in RAM).
// Relationship columns: id (column 0) and creationDate / workFrom / joinDate / language when the
// CSV has them.  A missing expected column that is not one of the pooled properties is tolerated
// (reported, absent from the schema); the pooled properties must all be present.
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "v3_json.h"

namespace {

// LDBC element id -> vertex index, by label.
struct IdIndex {
    std::map<std::string, std::vector<std::pair<int64_t, uint32_t>>> by_label;
    bool load(const std::string& path) {
        std::ifstream in(path);
        if (!in) return false;
        std::stringstream ss;
        ss << in.rdbuf();
        std::string err;
        v3::json::J j;
        if (!v3::json::parse(ss.str(), &j, &err)) return false;
        const v3::json::J* bl = j.get("by_label");
        if (!bl) return false;
        for (const auto& label : bl->o) {
            auto& v = by_label[label.first];
            v.reserve(label.second.o.size());
            for (const auto& e : label.second.o)
                v.emplace_back(std::strtoll(e.first.c_str(), nullptr, 10), (uint32_t)e.second.i);
            std::sort(v.begin(), v.end());
        }
        return true;
    }
    // the LDBC 0.3 decomposed id 16 * (T + 1) + x of a table T with local ids below the table's
    // vertex count maps to T's local id x (SF0.1 carries those; the export decoded them)
    uint32_t find_decoded(const std::string& label, int64_t id) const {
        auto it = by_label.find(label);
        if (it == by_label.end() || it->second.empty()) return 0xFFFFFFFFu;
        int64_t hi = -1;
        for (const auto& e : it->second) hi = std::max(hi, e.first);
        if (id >= 16 && id % 16 == 0) {
            const int64_t x = id / 16 - 1;
            if (x >= 0 && x <= hi) return find_one(label, x);
        }
        return 0xFFFFFFFFu;
    }
    // Resolve a CSV entity id to the concrete v2 label and its per-label index.  The CSV
    // tables name the base entity class, while v2 stores City / Country / Place and
    // Company / Organisation separately.
    bool which(const std::string& label, int64_t id, std::string* actual, uint32_t* local) const {
        static const std::map<std::string, std::vector<const char*>> subl = {
            {"Place", {"City", "Country", "Place"}},
            {"Organisation", {"Company", "Organisation"}},
            {"Forum", {"Forum"}},
        };
        auto it = subl.find(label);
        if (it != subl.end()) {
            for (const char* sub : it->second) {
                const uint32_t x = find_one(sub, id);
                if (x != 0xFFFFFFFFu) {
                    *actual = sub;
                    *local = x;
                    return true;
                }
            }
            return false;
        }
        const uint32_t x = find_one(label, id);
        if (x == 0xFFFFFFFFu) return false;
        *actual = label;
        *local = x;
        return true;
    }
    // SF0.1 carries LDBC 0.3 decomposed ids 16 * (T + 1) + x in the relationship tables.
    bool which_decoded(const std::string& label, int64_t id, std::string* actual, uint32_t* local) const {
        if (which(label, id, actual, local)) return true;
        if (id >= 16 && id % 16 == 0) return which(label, id / 16 - 1, actual, local);
        return false;
    }
    std::vector<int64_t> base;   // label id -> the first sidecar vertex index of that label
    uint32_t find_one(const std::string& label, int64_t id) const {
        auto it = by_label.find(label);
        if (it == by_label.end()) return 0xFFFFFFFFu;
        auto p = std::lower_bound(it->second.begin(), it->second.end(), std::make_pair(id, 0u));
        if (p == it->second.end() || p->first != id) return 0xFFFFFFFFu;
        return p->second;
    }
};

// Buffered line reader over a file descriptor.
class Lines {
public:
    Lines(const std::string& path) : path_(path), buf_(1 << 22) {
        fd_ = ::open(path.c_str(), O_RDONLY);
    }
    ~Lines() {
        if (fd_ >= 0) ::close(fd_);
    }
    bool ok() const { return fd_ >= 0; }
    bool next(std::string* out) {
        out->clear();
        while (true) {
            if (pos_ == len_) {
                if (eof_) return !out->empty();
                const ssize_t n = ::read(fd_, buf_.data(), buf_.size());
                if (n <= 0) {
                    eof_ = true;
                    return !out->empty();
                }
                len_ = (size_t)n;
                pos_ = 0;
            }
            const char* p = buf_.data() + pos_;
            const char* q = static_cast<const char*>(std::memchr(p, '\n', len_ - pos_));
            if (q) {
                out->append(p, (size_t)(q - p));
                pos_ += (size_t)(q - p) + 1;
                if (!out->empty() && out->back() == '\r') out->pop_back();
                return true;
            }
            out->append(p, len_ - pos_);
            pos_ = len_;
        }
    }
    const std::string& path() const { return path_; }
private:
    std::string path_;
    int fd_ = -1;
    std::vector<char> buf_;
    size_t pos_ = 0, len_ = 0;
    bool eof_ = false;
};

std::vector<std::string> split(const std::string& str) {
    std::vector<std::string> o;
    size_t a = 0;
    while (a <= str.size()) {
        const size_t b = str.find('|', a);
        if (b == std::string::npos) {
            o.push_back(str.substr(a));
            return o;
        }
        o.push_back(str.substr(a, b - a));
        a = b + 1;
    }
    return o;
}

bool header_positions(const std::string& path, std::vector<std::string>* names, std::string* err) {
    Lines in(path);
    std::string line;
    if (!in.ok() || !in.next(&line)) {
        *err = "cannot read header of " + path;
        return false;
    }
    *names = split(line);
    return true;
}

int32_t col_of(const std::vector<std::string>& header, const std::string& name) {
    for (size_t k = 0; k < header.size(); ++k)
        if (header[k] == name) return (int32_t)k;
    return -1;
}

struct Col {
    std::string name;
    bool is_str = false;
};

struct Out {
    static bool open(const std::string& dir, const char* name, int* fd) {
        const std::string p = dir + "/" + name;
        *fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (*fd < 0) std::fprintf(stderr, "cannot write %s: %s\n", p.c_str(), std::strerror(errno));
        return *fd >= 0;
    }
    static bool write(int fd, const void* p, size_t n) {
        const char* c = static_cast<const char*>(p);
        while (n) {
            const ssize_t k = ::write(fd, c, n);
            if (k <= 0) return false;
            c += k;
            n -= (size_t)k;
        }
        return true;
    }
};

// A header column may be spelled differently (newer LDBC versions).
std::string alt_header(const std::string& name) {
    if (name == "imageFile") return "content";
    if (name == "content") return "imageFile";
    if (name == "url") return "URL";
    if (name == "URL") return "url";
    return "";
}

int32_t find_col(const std::vector<std::string>& header, const std::string& name) {
    int32_t c = col_of(header, name);
    if (c < 0 && !alt_header(name).empty()) c = col_of(header, alt_header(name));
    return c;
}

// Write one column: rows[r] = (key, text); an int column stores the text as int64.
// The source id of a row of a relationship table: the CSV's value, or the vertex index in the
// v2 export when the CSV carries LDBC 0.3 "decomposed" ids the export rewrote (SF0.1).
struct IdMap {
    std::map<std::string, std::vector<std::pair<int64_t, uint32_t>>> by;
};

bool write_col(int fd, const std::vector<std::pair<int64_t, std::string>>& rows, bool is_str, size_t* width_out) {
    std::vector<uint8_t> nulls(rows.size() / 8 + 1, 0);
    size_t w = 8 + 1 + (is_str ? 4 : 8);
    if (is_str)
        for (size_t r = 0; r < rows.size(); ++r) w = std::max(w, (size_t)(8 + 1 + 4 + rows[r].second.size()));
    std::vector<uint8_t> buf(rows.size() * w + nulls.size(), 0);
    for (size_t r = 0; r < rows.size(); ++r) {
        uint8_t* p = buf.data() + r * w;
        std::memcpy(p, &rows[r].first, 8);
        if (is_str) {
            p[8] = 1;
            const uint32_t len = (uint32_t)rows[r].second.size();
            std::memcpy(p + 9, &len, 4);
            std::memcpy(p + 13, rows[r].second.data(), len);
            if (rows[r].second.empty()) nulls[r / 8] |= (uint8_t)(1u << (r % 8));
        } else {
            const int64_t v = rows[r].second.empty() ? 0 : std::strtoll(rows[r].second.c_str(), nullptr, 10);
            p[8] = 0;
            std::memcpy(p + 9, &v, 8);
            if (rows[r].second.empty()) nulls[r / 8] |= (uint8_t)(1u << (r % 8));
        }
    }
    *width_out = w;
    // records first, then the bitmap (one write: the file is laid out as the loader reads it)
    std::memcpy(buf.data() + rows.size() * w, nulls.data(), nulls.size());
    return Out::write(fd, buf.data(), buf.size());
}

}  // namespace

int main(int argc, char** argv) {
    std::string raw, ids_path, out;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--raw" && i + 1 < argc) raw = argv[++i];
        else if (a == "--ids" && i + 1 < argc) ids_path = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
    }
    if (raw.empty() || ids_path.empty() || out.empty()) {
        std::fprintf(stderr, "usage: v3_sidecar --raw DIR --ids ids.json --out DIR\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    IdIndex idx;
    if (!idx.load(ids_path)) {
        std::fprintf(stderr, "cannot read ids %s\n", ids_path.c_str());
        return 1;
    }
    // the v2 vertex index of a row: label id * rows + row (the export's labelling).  The sidecar
    // keeps every label block in table order, so its own index is that plus the label's base of
    // the export (sum of the rows of the previous labels, frequency order) minus the table offset.
    const std::string mk = "mkdir -p '" + out + "'";
    if (std::system(mk.c_str()) != 0) return 1;
    int vfd = -1, efd = -1, pfd = -1;
    if (!Out::open(out, "vertices.bin.part", &vfd) || !Out::open(out, "edges.bin.part", &efd) ||
        !Out::open(out, "props.bin.part", &pfd))
        return 1;

    std::vector<std::string> tables = {"Person", "Forum", "Post", "Comment", "Place", "Organisation", "Tag", "TagClass"};
    std::vector<std::string> files = {"dynamic/person_0_0.csv", "dynamic/forum_0_0.csv", "dynamic/post_0_0.csv",
                                      "dynamic/comment_0_0.csv", "static/place_0_0.csv",
                                      "static/organisation_0_0.csv", "static/tag_0_0.csv",
                                      "static/tagclass_0_0.csv"};
    // The pooled properties (v3/ldbc/cqplus properties) -- these must exist.
    const std::set<std::string> pooled = {"id", "firstName", "lastName", "gender", "birthday", "creationDate",
                                          "locationIP", "browserUsed", "title", "imageFile", "content", "length",
                                          "name", "workFrom", "joinDate"};
    auto cols_of = [](const std::string& label) {
        std::vector<Col> c;
        auto add = [&](const char* n, bool s = false) { c.push_back(Col{n, s}); };
        if (label == "Person")
            for (auto p : {std::make_pair("id", false), std::make_pair("firstName", true), std::make_pair("lastName", true),
                           std::make_pair("gender", true), std::make_pair("birthday", false),
                           std::make_pair("creationDate", false), std::make_pair("locationIP", true),
                           std::make_pair("browserUsed", true), std::make_pair("language", true),
                           std::make_pair("email", true)})
                add(p.first, p.second);
        else if (label == "Forum")
            add("id"), add("title", true), add("creationDate");
        else if (label == "Post")
            for (auto p : {std::make_pair("id", false), std::make_pair("imageFile", true), std::make_pair("creationDate", false),
                           std::make_pair("locationIP", true), std::make_pair("browserUsed", true),
                           std::make_pair("language", true), std::make_pair("content", true), std::make_pair("length", false)})
                add(p.first, p.second);
        else if (label == "Comment")
            for (auto p : {std::make_pair("id", false), std::make_pair("creationDate", false), std::make_pair("locationIP", true),
                           std::make_pair("browserUsed", true), std::make_pair("content", true), std::make_pair("length", false)})
                add(p.first, p.second);
        else if (label == "Place")
            add("id"), add("name", true), add("url", true), add("type", true);
        else if (label == "Organisation")
            add("id"), add("type", true), add("name", true), add("url", true);
        else
            add("id"), add("name", true), add("url", true);
        return c;
    };

    // v2 vertex index (label id * rows + row) -> sidecar index
    std::vector<std::string> order_labels;
    {
        const char* lf = std::getenv("V3_LABELS_JSON");
        if (lf) {
            std::ifstream in(lf);
            std::stringstream ss;
            ss << in.rdbuf();
            std::string err;
            v3::json::J j;
            if (v3::json::parse(ss.str(), &j, &err))
                if (const v3::json::J* vl = j.get("vertex_labels"))
                    for (const auto& x : vl->a) order_labels.push_back(x.s);
        }
    }
    // The v2 ids are a single global vertex index (the `by_label` values are ranges in the
    // frequency order of the labels), while the CSV tables are in file order.  Map the former
    // to the sidecar index once; edges and vertices then use the same index space.
    uint32_t nv2 = 0;
    for (const auto& l : idx.by_label)
        for (const auto& e : l.second) nv2 = std::max(nv2, e.second + 1);
    std::vector<uint32_t> sidecar_of_v2((size_t)nv2, 0xFFFFFFFFu);
    std::string props_tab;
    std::vector<std::array<uint32_t, 2>> vertex_rows;   // (label, row) in label order
    uint64_t pbytes = 0;
    size_t ncfg = 0;
    std::map<std::string, std::vector<int32_t>> header_pos;   // file -> column positions
    std::map<std::string, std::vector<Col>> kept;             // file -> columns written
    // Pass 1: every header (and the pooled columns must be in it).
    // the vertex label ids are the stored labels' frequency order of the v2 export
    // (<graph>.labels.json `vertex_labels`), which is also the order of the data labels
    static const char* kOrder[] = {"City", "Comment", "Company", "Country", "Forum", "Organisation",
                                   "Person", "Place", "Post", "Tag", "TagClass"};
    for (size_t t = 0; t < tables.size(); ++t) {
        const std::string path = raw + "/" + files[t];
        std::vector<std::string> header;
        std::string err;
        if (!header_positions(path, &header, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        std::vector<Col> keep;
        bool bad = false;
        for (const Col& c : cols_of(tables[t])) {
            const int32_t p = find_col(header, c.name);
            if (p < 0) {
                std::fprintf(stderr, "note %s: column %s absent\n", files[t].c_str(), c.name.c_str());
                if (pooled.count(c.name)) bad = true;
                continue;
            }
            keep.push_back(c);
        }
        if (bad) {
            std::fprintf(stderr, "STOP: %s lacks a pooled column (wrong --raw?)\n", path.c_str());
            return 1;
        }
        kept[files[t]] = keep;
    }
    // Pass 2: the vertex tables, column by column.  A CSV table can carry several v2 labels
    // (Place -> Country / City / Place, Organisation -> Company / Organisation); each label gets
    // its own property block and its own row space, so queries on :City / :Country / :Company
    // read the rows that actually carry that label.
    std::map<std::string, uint32_t> lid;
    for (uint32_t i = 0; i < sizeof kOrder / sizeof kOrder[0]; ++i) lid[kOrder[i]] = i;
    std::map<std::string, std::vector<Col>> label_cols;
    std::map<std::string, size_t> label_row_count;
    std::string labels_meta;
    size_t nlabels = 0;
    for (size_t t = 0; t < tables.size(); ++t) {
        const std::string path = raw + "/" + files[t];
        Lines in0(path);
        std::string line;
        in0.next(&line);
        const std::vector<std::string> header = split(line);
        const std::vector<Col>& cols = kept[files[t]];
        size_t r = 0;
        const int32_t idpos = find_col(header, cols[0].name);
        std::vector<int64_t> vrows;
        {
            Lines in(path);
            in.next(&line);
            std::vector<std::string> f;
            while (in.next(&line)) {
                if (line.empty()) continue;
                f = split(line);
                vrows.push_back(std::strtoll(f[(size_t)idpos].c_str(), nullptr, 10));
                ++r;
            }
        }
        std::vector<std::string> actuals(r);
        std::vector<uint32_t> local_row(r);
        std::map<std::string, uint32_t> next_row;
        for (size_t k = 0; k < vrows.size(); ++k) {
            std::string actual;
            uint32_t v2 = 0;
            if (!idx.which(tables[t], vrows[k], &actual, &v2)) actual = tables[t];
            actuals[k] = actual;
            local_row[k] = next_row[actual]++;
            if (!label_cols.count(actual)) label_cols[actual] = cols;
        }
        for (size_t c = 0; c < cols.size(); ++c) {
            std::map<std::string, std::vector<std::pair<int64_t, std::string>>> rows_by;
            for (const auto& kv : next_row) rows_by[kv.first].reserve(kv.second);
            {
                Lines in(path);
                in.next(&line);
                std::vector<std::string> f;
                const size_t pos = (size_t)find_col(header, cols[c].name);
                size_t k = 0;
                while (in.next(&line)) {
                    if (line.empty()) continue;
                    f = split(line);
                    rows_by[actuals[k]].emplace_back(vrows[k], f[pos]);
                    ++k;
                }
            }
            for (uint32_t li = 0; li < sizeof kOrder / sizeof kOrder[0]; ++li) {
                auto it = rows_by.find(kOrder[li]);
                if (it == rows_by.end() || it->second.empty()) continue;
                size_t w = 0;
                if (!write_col(pfd, it->second, cols[c].is_str, &w)) return 1;
                if (ncfg++) props_tab += ",";
                props_tab += "{\"key\":" + std::to_string(li) + ",\"col\":" + std::to_string(c) +
                             ",\"rows\":" + std::to_string(it->second.size()) + ",\"width\":" + std::to_string(w) +
                             ",\"off\":" + std::to_string(pbytes) + "}";
                pbytes += it->second.size() * w + it->second.size() / 8 + 1;
            }
        }
        // vertices.bin keeps the per-label property row; edges use the sidecar index assigned
        // here.  `sidecar_of_v2` is the bridge from the v2 global ids the relationship tables
        // carry.
        size_t mapped = 0;
        for (size_t k = 0; k < vrows.size(); ++k) {
            const uint32_t vidx = (uint32_t)vertex_rows.size();
            vertex_rows.push_back({lid[actuals[k]], local_row[k]});
            std::string actual;
            uint32_t v2 = 0;
            if (idx.which(tables[t], vrows[k], &actual, &v2) && v2 < sidecar_of_v2.size()) {
                sidecar_of_v2[v2] = vidx;
                ++mapped;
            }
        }
        for (const auto& kv : next_row) label_row_count[kv.first] = kv.second;
        std::printf("label %s rows %zu mapped %zu labels", tables[t].c_str(), r, mapped);
        for (const auto& kv : next_row) std::printf(" %s:%u", kv.first.c_str(), kv.second);
        std::printf("\n");
    }
    for (uint32_t li = 0; li < sizeof kOrder / sizeof kOrder[0]; ++li) {
        const std::string name = kOrder[li];
        if (!label_row_count.count(name)) continue;
        if (nlabels++) labels_meta += ",";
        labels_meta += "{\"name\":" + v3::json::Writer::quote(name) + ",\"rows\":" +
                       std::to_string(label_row_count[name]) + ",\"props\":[";
        const auto& cols = label_cols[name];
        for (size_t c = 0; c < cols.size(); ++c) {
            if (c) labels_meta += ",";
            labels_meta += "{\"name\":" + v3::json::Writer::quote(cols[c].name) +
                           ",\"col\":" + std::to_string(c) +
                           ",\"type\":" + (cols[c].is_str ? "\"str\"" : "\"int\"") + "}";
        }
        labels_meta += "]}";
    }
    // vertices.bin
    for (const auto& x : vertex_rows) {
        const uint32_t two[2] = {x[0], x[1]};
        if (!Out::write(vfd, two, sizeof two)) return 1;
    }
    // relationships: types sorted; every relationship twice (one row per direction)
    struct Rel {
        std::string from, to, type, file;
        bool static_or = false;        // the target is a static place (v2 exported it unchanged, not as City/Country)
        bool from_place = false;       // the source is a Place keyed by the Place table's id
        bool to_place = false;         // the target is a Place keyed by the Place table's id
    };
    std::vector<Rel> rels = {
        {"Person", "Person", "KNOWS", "dynamic/person_knows_person_0_0.csv"},
        {"Person", "Place", "IS_LOCATED_IN", "dynamic/person_isLocatedIn_place_0_0.csv"},
        {"Person", "Tag", "HAS_INTEREST", "dynamic/person_hasInterest_tag_0_0.csv"},
        {"Person", "Organisation", "WORK_AT", "dynamic/person_workAt_organisation_0_0.csv"},
        {"Person", "Organisation", "STUDY_AT", "dynamic/person_studyAt_organisation_0_0.csv"},
        {"Forum", "Post", "CONTAINER_OF", "dynamic/forum_containerOf_post_0_0.csv"},
        {"Forum", "Person", "HAS_MEMBER", "dynamic/forum_hasMember_person_0_0.csv"},
        {"Forum", "Person", "HAS_MODERATOR", "dynamic/forum_hasModerator_person_0_0.csv"},
        {"Forum", "Tag", "HAS_TAG", "dynamic/forum_hasTag_tag_0_0.csv"},
        {"Post", "Person", "HAS_CREATOR", "dynamic/post_hasCreator_person_0_0.csv"},
        {"Post", "Tag", "HAS_TAG", "dynamic/post_hasTag_tag_0_0.csv"},
        {"Post", "Place", "IS_LOCATED_IN", "dynamic/post_isLocatedIn_place_0_0.csv"},
        {"Comment", "Person", "HAS_CREATOR", "dynamic/comment_hasCreator_person_0_0.csv"},
        {"Comment", "Tag", "HAS_TAG", "dynamic/comment_hasTag_tag_0_0.csv"},
        {"Comment", "Place", "IS_LOCATED_IN", "dynamic/comment_isLocatedIn_place_0_0.csv"},
        {"Comment", "Post", "REPLY_OF", "dynamic/comment_replyOf_post_0_0.csv"},
        {"Comment", "Comment", "REPLY_OF", "dynamic/comment_replyOf_comment_0_0.csv"},
        {"Person", "Post", "LIKES", "dynamic/person_likes_post_0_0.csv"},
        {"Person", "Comment", "LIKES", "dynamic/person_likes_comment_0_0.csv"},
        {"Place", "Place", "IS_PART_OF", "static/place_isPartOf_place_0_0.csv", false, true, true},
        {"Organisation", "Place", "IS_LOCATED_IN", "static/organisation_isLocatedIn_place_0_0.csv", true, false, true},
        {"Tag", "TagClass", "HAS_TYPE", "static/tag_hasType_tagclass_0_0.csv"},
        {"TagClass", "TagClass", "IS_SUBCLASS_OF", "static/tagclass_isSubclassOf_tagclass_0_0.csv"},
    };
    std::sort(rels.begin(), rels.end(), [](const Rel& a, const Rel& b) {
        if (a.type != b.type) return a.type < b.type;
        return a.file < b.file;
    });
    std::vector<std::string> types;
    for (const auto& r : rels)
        if (types.empty() || types.back() != r.type) types.push_back(r.type);
    uint64_t nrels = 0;
    std::map<uint32_t, std::vector<std::string>> type_props;
    size_t k = 0;
    for (size_t ti = 0; ti < types.size(); ++ti) {
        const size_t first = k;
        while (k < rels.size() && rels[k].type == types[ti]) ++k;
        uint64_t base = 0;
        std::vector<std::array<uint32_t, 5>> dirs;
        std::vector<std::pair<std::string, uint64_t>> tinfo;   // table file -> first row
        std::map<std::string, std::vector<std::string>> theader;
        for (size_t j = first; j < k; ++j) {
            const Rel& r = rels[j];
            const std::string path = raw + "/" + r.file;
            std::vector<std::string> header;
            std::string err;
            if (!header_positions(path, &header, &err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return 1;
            }
            tinfo.emplace_back(r.file, base);
            theader[r.file] = header;
            // endpoints: the from-side `X.id` column, and the to-side `Y.id` column (a static
            // place is keyed by `Organisation.id`, a self-relation may repeat `X.id`)
            int32_t sc = col_of(header, r.from + ".id");
            int32_t tc = header.size() >= 3 ? col_of(header, r.to + ".id") : -1;
            // a self-relation repeats the same header (Person.id|Person.id, Comment.id|Comment.id):
            // the target is the second occurrence, not the source's first one
            if (r.from == r.to) {
                tc = -1;
                for (int32_t q = (int32_t)header.size() - 1; q >= 0; --q)
                    if (header[(size_t)q] == r.to + ".id") {
                        tc = q;
                        break;
                    }
            }
            if (header.size() == 2 && header[0] == r.from + ".id" && header[1] == r.to + ".id" && r.from != r.to) tc = 1;
            if (tc < 0 && r.to == "Place") tc = col_of(header, "Organisation.id");
            if (tc < 0 && r.to == "Place") tc = col_of(header, "Place.id");
            if (tc < 0 && r.to == "Organisation") tc = col_of(header, "Organisation.id");
            if (tc < 0 && r.to == "Organisation") tc = col_of(header, "Company.id");
            if (tc < 0 && header.size() == 2 && header[1].size() > 4 &&
                (header[1] == "Place.id" || header[1] == r.to + ".id" || header[1] == "Organisation.id"))
                tc = 1;
            if (sc < 0 || tc < 0) {
                std::fprintf(stderr, "%s: endpoint columns %s.id / %s.id missing\n", r.file.c_str(), r.from.c_str(),
                             r.to.c_str());
                return 1;
            }
            // A relationship row names its endpoints by LDBC id.  Resolve the concrete v2
            // label (Place -> City / Country / Place, Organisation -> Company / Organisation)
            // and then read the sidecar index assigned in the vertex pass.
            auto endpoint = [&](const std::string& label, int64_t id) -> uint32_t {
                std::string actual;
                uint32_t v2 = 0;
                if (!idx.which_decoded(label, id, &actual, &v2) || v2 >= sidecar_of_v2.size()) return 0xFFFFFFFFu;
                return sidecar_of_v2[v2];
            };
            Lines in(path);
            std::string line;
            in.next(&line);
            uint64_t row = 0, used = 0, missing = 0;
            std::vector<std::string> f;
            while (in.next(&line)) {
                if (line.empty()) continue;
                f = split(line);
                const int64_t sid = std::strtoll(f[(size_t)sc].c_str(), nullptr, 10);
                const int64_t did = std::strtoll(f[(size_t)tc].c_str(), nullptr, 10);
                const uint32_t u = endpoint(r.from, sid);
                const uint32_t v = endpoint(r.to, did);
                if (u == 0xFFFFFFFFu || v == 0xFFFFFFFFu) {
                    ++missing;
                    ++row;
                    continue;
                }
                dirs.push_back({u, v, (uint32_t)ti, (uint32_t)(base + row), 1u});
                dirs.push_back({v, u, (uint32_t)ti, (uint32_t)(base + row), 0u});
                ++row;
                ++used;
            }
            base += row;
            std::printf("rel %s <- %s rows %llu used %llu missing %llu\n", types[ti].c_str(), r.file.c_str(),
                        (unsigned long long)row, (unsigned long long)used, (unsigned long long)missing);
        }
        std::sort(dirs.begin(), dirs.end(), [](const std::array<uint32_t, 5>& a, const std::array<uint32_t, 5>& b) {
            if (a[0] != b[0]) return a[0] < b[0];
            if (a[1] != b[1]) return a[1] < b[1];
            return a[2] < b[2];
        });
        if (!Out::write(efd, dirs.data(), dirs.size() * sizeof(dirs[0]))) return 1;
        // relationship property columns: every non-endpoint column of the type's tables, one
        // block keyed by (type, column); a row without the column is NULL
        {
            std::vector<std::string> ecols;
            for (const auto& th : tinfo) {
                for (const auto& h : theader[th.first]) {
                    if (h.size() >= 3 && h.compare(h.size() - 3, 3, ".id") == 0) continue;
                    if (h == "id") continue;
                    if (std::find(ecols.begin(), ecols.end(), h) == ecols.end()) ecols.push_back(h);
                }
            }
            std::sort(ecols.begin(), ecols.end());
            type_props[(uint32_t)ti] = ecols;
            for (size_t c = 0; c < ecols.size(); ++c) {
                std::vector<std::pair<int64_t, std::string>> rows((size_t)base);
                for (const auto& th : tinfo) {
                    const std::vector<std::string>& header = theader[th.first];
                    const int32_t pos = col_of(header, ecols[c]);
                    if (pos < 0) continue;
                    Lines in(raw + "/" + th.first);
                    std::string line;
                    in.next(&line);
                    uint64_t i = 0;
                    std::vector<std::string> f;
                    while (in.next(&line)) {
                        if (line.empty()) continue;
                        f = split(line);
                        if (th.second + i < base) rows[(size_t)(th.second + i)] = {(int64_t)(th.second + i), f[(size_t)pos]};
                        ++i;
                    }
                }
                size_t w = 0;
                if (!write_col(pfd, rows, ecols[c] == "language", &w)) return 1;
                if (ncfg++) props_tab += ",";
                props_tab += "{\"key\":" + std::to_string(ti) + ",\"col\":" + std::to_string(c) +
                             ",\"rows\":" + std::to_string(rows.size()) + ",\"width\":" + std::to_string(w) +
                             ",\"off\":" + std::to_string(pbytes) + "}";
                pbytes += rows.size() * w + rows.size() / 8 + 1;
            }
        }
        nrels += dirs.size();
    }
    ::close(vfd);
    ::close(efd);
    ::close(pfd);
    std::rename((out + "/vertices.bin.part").c_str(), (out + "/vertices.bin").c_str());
    std::rename((out + "/edges.bin.part").c_str(), (out + "/edges.bin").c_str());
    std::rename((out + "/props.bin.part").c_str(), (out + "/props.bin").c_str());
    {
        std::ofstream f(out + "/props.json");
        f << "{\"segments\":[" << props_tab << "]}\n";
    }
    {
        std::ofstream f(out + "/meta.json");
        f << "{\n\"vertices\":" << vertex_rows.size() << ",\n\"rels\":" << nrels << ",\n\"props_bytes\":" << pbytes
          << ",\n\"labels\":[" << labels_meta << "],\n\"types\":[";
        for (size_t ti = 0; ti < types.size(); ++ti) {
            f << (ti ? "," : "") << "{\"id\":" << ti << ",\"name\":" << v3::json::Writer::quote(types[ti])
              << ",\"props\":[";
            const auto& tp = type_props[(uint32_t)ti];
            for (size_t c = 0; c < tp.size(); ++c)
                f << (c ? "," : "") << "{\"name\":" << v3::json::Writer::quote(tp[c]) << ",\"col\":" << c << "}";
            f << "]}";
        }
        f << "],\n\"label_ids\":[";
        for (size_t k = 0; k < sizeof kOrder / sizeof kOrder[0]; ++k)
            f << (k ? "," : "") << "{\"id\":" << k << ",\"name\":" << v3::json::Writer::quote(kOrder[k]) << "}";
        f << "],\n\"type_ids\":[";
        for (size_t ti = 0; ti < types.size(); ++ti) {
            f << (ti ? "," : "") << "{\"id\":" << ti << ",\"name\":" << v3::json::Writer::quote(types[ti])
              << ",\"props\":[";
            const auto& tp = type_props[(uint32_t)ti];
            for (size_t c = 0; c < tp.size(); ++c)
                f << (c ? "," : "") << "{\"name\":" << v3::json::Writer::quote(tp[c]) << ",\"col\":" << c << "}";
            f << "]}";
        }
        f << "]\n}\n";
    }
    std::printf("vertices %zu rels %llu props_bytes %llu\n", vertex_rows.size(), (unsigned long long)nrels,
                (unsigned long long)pbytes);
    return 0;
}
