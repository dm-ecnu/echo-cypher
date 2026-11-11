// Minimal JSON reader / writer for the v3 executor (plan files are produced by
// scripts/v3_build_rounds.py; result files are consumed by scripts/v3_compare.py).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace v3 {
namespace json {

struct J {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false;
    double num = 0;
    bool is_int = false;
    long long i = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;

    const J* get(const std::string& k) const;
    bool has(const std::string& k) const { return get(k) != nullptr; }
    std::string str(const std::string& k, const std::string& dflt = "") const;
    long long intv(const std::string& k, long long dflt = 0) const;
    bool boolv(const std::string& k, bool dflt = false) const;
    size_t size() const { return t == ARR ? a.size() : (t == OBJ ? o.size() : 0); }
    const J& operator[](size_t k) const { return a[k]; }
};

bool parse(const std::string& text, J* out, std::string* err);

// Writer with stable output (numbers as integers when they are integral).
class Writer {
public:
    void raw(const std::string& s) { out_ += s; }
    void key(const std::string& k) { out_ += quote(k) + ":"; }
    static std::string quote(const std::string& s);
    const std::string& str() const { return out_; }
private:
    std::string out_;
};

}  // namespace json
}  // namespace v3
