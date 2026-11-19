#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "util.h"

namespace ea::json {

static const Value kNull;

Value::Value(const std::wstring& v) : t(Str), s(narrow(v)) {}

bool Value::has(const char* k) const {
    if (t != Obj) return false;
    for (auto& p : o)
        if (p.first == k) return true;
    return false;
}

const Value& Value::operator[](const char* k) const {
    if (t != Obj) return kNull;
    for (auto& p : o)
        if (p.first == k) return p.second;
    return kNull;
}

const Value& Value::operator[](size_t i) const {
    if (t != Arr || i >= a.size()) return kNull;
    return a[i];
}

std::string Value::str(const std::string& def) const {
    if (t == Str) return s;
    if (t == Num) {
        char b[64];
        if (n == std::floor(n) && std::fabs(n) < 1e15)
            snprintf(b, sizeof b, "%lld", (long long)n);
        else
            snprintf(b, sizeof b, "%.17g", n);
        return b;
    }
    if (t == Bool) return b ? "true" : "false";
    return def;
}

std::wstring Value::wstr(const std::wstring& def) const {
    if (t == Null) return def;
    return widen(str());
}

double Value::num(double def) const {
    if (t == Num) return std::isfinite(n) ? n : def;
    if (t == Str && !s.empty()) {
        char* e = nullptr;
        double v = strtod(s.c_str(), &e);
        if (e && *e == 0 && std::isfinite(v)) return v;
    }
    if (t == Bool) return b ? 1 : 0;
    return def;
}

bool Value::boolean(bool def) const {
    if (t == Bool) return b;
    if (t == Num) return n != 0;
    if (t == Str) return s == "1" || s == "true";
    return def;
}

Value& Value::set(const std::string& k, Value v) {
    if (t != Obj) {
        *this = object();
    }
    for (auto& p : o)
        if (p.first == k) {
            p.second = std::move(v);
            return p.second;
        }
    o.emplace_back(k, std::move(v));
    return o.back().second;
}

Value& Value::push(Value v) {
    if (t != Arr) *this = array();
    a.push_back(std::move(v));
    return a.back();
}

namespace {

struct Parser {
    const char* p;
    const char* end;
    std::string err;
    int depth = 0;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    bool fail(const char* m) {
        if (err.empty()) err = m;
        return false;
    }
    bool lit(const char* w) {
        size_t n = strlen(w);
        if ((size_t)(end - p) < n || memcmp(p, w, n) != 0) return false;
        p += n;
        return true;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80)
            o += (char)cp;
        else if (cp < 0x800) {
            o += (char)(0xC0 | (cp >> 6));
            o += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += (char)(0xE0 | (cp >> 12));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        } else {
            o += (char)(0xF0 | (cp >> 18));
            o += (char)(0x80 | ((cp >> 12) & 0x3F));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& v) {
        if (end - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool string(std::string& o) {
        p++;  // opening quote
        const char* run = p;
        while (p < end) {
            char c = *p;
            if (c == '"') {
                o.append(run, p);
                p++;
                return true;
            }
            if (c == '\\') {
                o.append(run, p);
                p++;
                if (p >= end) return fail("bad escape");
                char e = *p++;
                switch (e) {
                    case '"': o += '"'; break;
                    case '\\': o += '\\'; break;
                    case '/': o += '/'; break;
                    case 'b': o += '\b'; break;
                    case 'f': o += '\f'; break;
                    case 'n': o += '\n'; break;
                    case 'r': o += '\r'; break;
                    case 't': o += '\t'; break;
                    case 'u': {
                        unsigned cp;
                        if (!hex4(cp)) return fail("bad \\u");
                        if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                            p += 2;
                            unsigned lo;
                            if (!hex4(lo)) return fail("bad surrogate");
                            if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else { utf8(o, 0xFFFD); cp = lo; }
                        } else if (cp >= 0xD800 && cp < 0xE000) {
                            cp = 0xFFFD;
                        }
                        utf8(o, cp);
                        break;
                    }
                    default: return fail("bad escape");
                }
                run = p;
                continue;
            }
            p++;
        }
        return fail("unterminated string");
    }
    bool value(Value& v) {
        if (++depth > 512) return fail("too deep");
        ws();
        if (p >= end) return fail("unexpected end");
        bool ok = true;
        char c = *p;
        if (c == '{') {
            v = Value::object();
            p++;
            ws();
            if (p < end && *p == '}') {
                p++;
            } else {
                for (;;) {
                    ws();
                    if (p >= end || *p != '"') { ok = fail("expected key"); break; }
                    std::string k;
                    if (!string(k)) { ok = false; break; }
                    ws();
                    if (p >= end || *p != ':') { ok = fail("expected ':'"); break; }
                    p++;
                    v.o.emplace_back(std::move(k), Value());
                    if (!value(v.o.back().second)) { ok = false; break; }
                    ws();
                    if (p < end && *p == ',') { p++; continue; }
                    if (p < end && *p == '}') { p++; break; }
                    ok = fail("expected ',' or '}'");
                    break;
                }
            }
        } else if (c == '[') {
            v = Value::array();
            p++;
            ws();
            if (p < end && *p == ']') {
                p++;
            } else {
                for (;;) {
                    v.a.emplace_back();
                    if (!value(v.a.back())) { ok = false; break; }
                    ws();
                    if (p < end && *p == ',') { p++; continue; }
                    if (p < end && *p == ']') { p++; break; }
                    ok = fail("expected ',' or ']'");
                    break;
                }
            }
        } else if (c == '"') {
            v.t = Value::Str;
            ok = string(v.s);
        } else if (lit("true")) {
            v = Value(true);
        } else if (lit("false")) {
            v = Value(false);
        } else if (lit("null")) {
            v = Value();
        } else if (lit("NaN")) {
            v = Value(NAN);
        } else if (lit("Infinity")) {
            v = Value(INFINITY);
        } else if (lit("-Infinity")) {
            v = Value(-INFINITY);
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            char* e = nullptr;
            // strtod needs a terminated buffer; copy the token.
            const char* q = p;
            while (q < end && (strchr("+-.eE", *q) || (*q >= '0' && *q <= '9'))) q++;
            std::string tok(p, q);
            double d = strtod(tok.c_str(), &e);
            if (!e || *e) return fail("bad number");
            v = Value(d);
            p = q;
        } else {
            ok = fail("unexpected character");
        }
        depth--;
        return ok;
    }
};

void esc(std::string& o, const std::string& s) {
    o += '"';
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
                    snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    o += '"';
}

void write(std::string& o, const Value& v, bool pretty, int ind) {
    auto nl = [&](int d) {
        if (!pretty) return;
        o += '\n';
        o.append(d * 2, ' ');
    };
    switch (v.t) {
        case Value::Null: o += "null"; break;
        case Value::Bool: o += v.b ? "true" : "false"; break;
        case Value::Num:
            if (!std::isfinite(v.n)) o += "null";
            else o += v.str();
            break;
        case Value::Str: esc(o, v.s); break;
        case Value::Arr:
            o += '[';
            for (size_t i = 0; i < v.a.size(); i++) {
                if (i) o += ',';
                nl(ind + 1);
                write(o, v.a[i], pretty, ind + 1);
            }
            if (!v.a.empty()) nl(ind);
            o += ']';
            break;
        case Value::Obj:
            o += '{';
            for (size_t i = 0; i < v.o.size(); i++) {
                if (i) o += ',';
                nl(ind + 1);
                esc(o, v.o[i].first);
                o += pretty ? ": " : ":";
                write(o, v.o[i].second, pretty, ind + 1);
            }
            if (!v.o.empty()) nl(ind);
            o += '}';
            break;
    }
}

}  // namespace

bool parse(const std::string& text, Value& out, std::string* err) {
    Parser ps{text.data(), text.data() + text.size(), {}};
    // Skip a UTF-8 BOM.
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        ps.p += 3;
    out = Value();
    bool ok = ps.value(out);
    if (ok) {
        ps.ws();
        if (ps.p != ps.end) ok = ps.fail("trailing data");
    }
    if (!ok && err) *err = ps.err;
    return ok;
}

std::string dump(const Value& v, bool pretty) {
    std::string o;
    write(o, v, pretty, 0);
    return o;
}

}  // namespace ea::json
