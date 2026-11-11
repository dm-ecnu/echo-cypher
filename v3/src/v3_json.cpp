#include "v3_json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace v3 {
namespace json {

const J* J::get(const std::string& k) const {
    for (const auto& kv : o)
        if (kv.first == k) return &kv.second;
    return nullptr;
}

std::string J::str(const std::string& k, const std::string& dflt) const {
    const J* v = get(k);
    return (v && v->t == STR) ? v->s : dflt;
}

long long J::intv(const std::string& k, long long dflt) const {
    const J* v = get(k);
    if (!v) return dflt;
    if (v->t == NUM) return v->is_int ? v->i : (long long)v->num;
    if (v->t == STR) return std::strtoll(v->s.c_str(), nullptr, 10);
    if (v->t == NUL) return dflt;
    return dflt;
}

bool J::boolv(const std::string& k, bool dflt) const {
    const J* v = get(k);
    return v && v->t == BOOL ? v->b : dflt;
}

namespace {
struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string err;
    explicit Parser(const std::string& x) : s(x) {}
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
    }
    bool fail(const std::string& m) {
        if (err.empty()) err = m + " at " + std::to_string(i);
        return false;
    }
    bool str(std::string* out) {
        if (i >= s.size() || s[i] != '"') return fail("expected string");
        ++i;
        out->clear();
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out->push_back(c); continue; }
            if (i >= s.size()) break;
            char e = s[i++];
            switch (e) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    if (i + 4 > s.size()) return fail("bad \\u");
                    unsigned cp = (unsigned)std::strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' &&
                        s[i + 1] == 'u') {
                        unsigned lo = (unsigned)std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
                        i += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    // UTF-8 encode
                    if (cp < 0x80) out->push_back((char)cp);
                    else if (cp < 0x800) {
                        out->push_back((char)(0xC0 | (cp >> 6)));
                        out->push_back((char)(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        out->push_back((char)(0xE0 | (cp >> 12)));
                        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                        out->push_back((char)(0x80 | (cp & 0x3F)));
                    } else {
                        out->push_back((char)(0xF0 | (cp >> 18)));
                        out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                        out->push_back((char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool value(J* out, int depth) {
        if (depth > 200) return fail("too deep");
        ws();
        if (i >= s.size()) return fail("eof");
        char c = s[i];
        if (c == '{') {
            ++i;
            out->t = J::OBJ;
            ws();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            while (true) {
                ws();
                std::string k;
                if (!str(&k)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return fail("expected :");
                ++i;
                out->o.emplace_back(k, J());
                if (!value(&out->o.back().second, depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return fail("expected , or }");
            }
        }
        if (c == '[') {
            ++i;
            out->t = J::ARR;
            ws();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            while (true) {
                out->a.emplace_back();
                if (!value(&out->a.back(), depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return fail("expected , or ]");
            }
        }
        if (c == '"') {
            out->t = J::STR;
            return str(&out->s);
        }
        if (s.compare(i, 4, "true") == 0) { i += 4; out->t = J::BOOL; out->b = true; return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; out->t = J::BOOL; out->b = false; return true; }
        if (s.compare(i, 4, "null") == 0) { i += 4; out->t = J::NUL; return true; }
        size_t j = i;
        if (j < s.size() && (s[j] == '-' || s[j] == '+')) ++j;
        bool is_int = true;
        while (j < s.size() && (isdigit((unsigned char)s[j]) || s[j] == '.' || s[j] == 'e' ||
                                s[j] == 'E' || s[j] == '-' || s[j] == '+')) {
            if (s[j] == '.' || s[j] == 'e' || s[j] == 'E') is_int = false;
            ++j;
        }
        if (j == i) return fail("unexpected char");
        std::string num = s.substr(i, j - i);
        out->t = J::NUM;
        out->num = std::strtod(num.c_str(), nullptr);
        out->is_int = is_int;
        out->i = is_int ? std::strtoll(num.c_str(), nullptr, 10) : (long long)out->num;
        i = j;
        return true;
    }
};
}  // namespace

bool parse(const std::string& text, J* out, std::string* err) {
    Parser p(text);
    if (!p.value(out, 0)) {
        if (err) *err = p.err;
        return false;
    }
    p.ws();
    if (p.i != text.size()) {
        if (err) *err = "trailing characters at " + std::to_string(p.i);
        return false;
    }
    return true;
}

std::string Writer::quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o.push_back((char)c);
                }
        }
    }
    return o + "\"";
}

}  // namespace json
}  // namespace v3
