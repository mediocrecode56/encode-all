// Minimal JSON: enough for ffprobe, yt-dlp -J and our own settings/queue files.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace ea::json {

struct Value {
    enum Type { Null, Bool, Num, Str, Arr, Obj };
    Type t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> a;
    std::vector<std::pair<std::string, Value>> o;

    Value() = default;
    Value(bool v) : t(Bool), b(v) {}
    Value(int v) : t(Num), n(v) {}
    Value(long long v) : t(Num), n((double)v) {}
    Value(unsigned long long v) : t(Num), n((double)v) {}
    Value(double v) : t(Num), n(v) {}
    Value(const char* v) : t(Str), s(v) {}
    Value(const std::string& v) : t(Str), s(v) {}
    Value(const std::wstring& v);
    static Value array() { Value v; v.t = Arr; return v; }
    static Value object() { Value v; v.t = Obj; return v; }

    bool isNull() const { return t == Null; }
    bool isObj() const { return t == Obj; }
    bool isArr() const { return t == Arr; }
    bool isStr() const { return t == Str; }
    bool isNum() const { return t == Num; }
    bool has(const char* k) const;
    const Value& operator[](const char* k) const;
    const Value& operator[](const std::string& k) const { return (*this)[k.c_str()]; }
    const Value& operator[](size_t i) const;
    const Value& operator[](int i) const { return (*this)[(size_t)(i < 0 ? 0 : i)]; }
    size_t size() const { return t == Arr ? a.size() : t == Obj ? o.size() : 0; }

    std::string str(const std::string& def = "") const;
    std::wstring wstr(const std::wstring& def = L"") const;
    double num(double def = 0) const;  // also parses numeric strings ("1234")
    long long integer(long long def = 0) const { return (long long)num((double)def); }
    bool boolean(bool def = false) const;

    Value& set(const std::string& k, Value v);
    Value& push(Value v);
};

bool parse(const std::string& text, Value& out, std::string* err = nullptr);
std::string dump(const Value& v, bool pretty = false);

}  // namespace ea::json
