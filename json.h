#pragma once
// Minimal JSON value model — just enough for BaconSaver's config.json.
// Objects preserve insertion order so re-saved configs stay stable.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace json {

struct value;
using array = std::vector<value>;
using object = std::vector<std::pair<std::string, value>>;

struct value {
    enum kind {
        null_t,
        bool_t,
        num_t,
        str_t,
        arr_t,
        obj_t
    } type = null_t;
    bool b = false;
    double num = 0;
    std::string str;
    std::shared_ptr<array> arr;
    std::shared_ptr<object> obj;

    value() = default;
    value(bool v) : type(bool_t), b(v) {}
    value(double v) : type(num_t), num(v) {}
    value(int v) : type(num_t), num(v) {}
    value(const std::string& v) : type(str_t), str(v) {}
    value(const char* v) : type(str_t), str(v) {}

    static value make_array() {
        value v;
        v.type = arr_t;
        v.arr = std::make_shared<array>();
        return v;
    }
    static value make_object() {
        value v;
        v.type = obj_t;
        v.obj = std::make_shared<object>();
        return v;
    }

    bool is_object() const { return type == obj_t; }
    bool is_array() const { return type == arr_t; }
    bool is_string() const { return type == str_t; }
    bool is_number() const { return type == num_t; }

    // Object access by key; returns nullptr if missing or not an object.
    const value* find(const std::string& key) const {
        if (type != obj_t || !obj)
            return nullptr;
        for (auto& kv : *obj)
            if (kv.first == key)
                return &kv.second;
        return nullptr;
    }

    void set(const std::string& key, value v) {
        if (type != obj_t) {
            type = obj_t;
            obj = std::make_shared<object>();
        }
        for (auto& kv : *obj)
            if (kv.first == key) {
                kv.second = std::move(v);
                return;
            }
        obj->emplace_back(key, std::move(v));
    }

    std::string as_string(const std::string& def = "") const { return type == str_t ? str : def; }
    int as_int(int def = 0) const { return type == num_t ? (int)std::llround(num) : def; }
    bool as_bool(bool def = false) const { return type == bool_t ? b : def; }
};

// --- Parsing ---

namespace detail {

struct parser {
    const char* p;
    const char* end;

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    }

    bool parse_string(std::string& out) {
        if (p >= end || *p != '"')
            return false;
        ++p;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\' && p < end) {
                char e = *p++;
                switch (e) {
                case 'n':
                    out += '\n';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case '/':
                    out += '/';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '"':
                    out += '"';
                    break;
                case 'u': {
                    if (end - p < 4)
                        return false;
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = *p++;
                        cp <<= 4;
                        if (h >= '0' && h <= '9')
                            cp |= (h - '0');
                        else if (h >= 'a' && h <= 'f')
                            cp |= (h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F')
                            cp |= (h - 'A' + 10);
                        else
                            return false;
                    }
                    // BMP only — adequate for config paths.
                    if (cp < 0x80)
                        out += (char)cp;
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default:
                    out += e;
                    break;
                }
            } else {
                out += c;
            }
        }
        if (p >= end)
            return false;
        ++p; // closing quote
        return true;
    }

    bool parse_value(value& out) {
        skip_ws();
        if (p >= end)
            return false;
        char c = *p;
        if (c == '"') {
            out.type = value::str_t;
            return parse_string(out.str);
        }
        if (c == '{')
            return parse_object(out);
        if (c == '[')
            return parse_array(out);
        if (c == 't') {
            if (end - p >= 4 && strncmp(p, "true", 4) == 0) {
                p += 4;
                out = value(true);
                return true;
            }
            return false;
        }
        if (c == 'f') {
            if (end - p >= 5 && strncmp(p, "false", 5) == 0) {
                p += 5;
                out = value(false);
                return true;
            }
            return false;
        }
        if (c == 'n') {
            if (end - p >= 4 && strncmp(p, "null", 4) == 0) {
                p += 4;
                out = value();
                return true;
            }
            return false;
        }
        // number
        char* numend = nullptr;
        double d = strtod(p, &numend);
        if (numend == p)
            return false;
        p = numend;
        out = value(d);
        return true;
    }

    bool parse_array(value& out) {
        ++p; // [
        out = value::make_array();
        skip_ws();
        if (p < end && *p == ']') {
            ++p;
            return true;
        }
        for (;;) {
            value v;
            if (!parse_value(v))
                return false;
            out.arr->push_back(std::move(v));
            skip_ws();
            if (p >= end)
                return false;
            if (*p == ',') {
                ++p;
                continue;
            }
            if (*p == ']') {
                ++p;
                return true;
            }
            return false;
        }
    }

    bool parse_object(value& out) {
        ++p; // {
        out = value::make_object();
        skip_ws();
        if (p < end && *p == '}') {
            ++p;
            return true;
        }
        for (;;) {
            skip_ws();
            std::string key;
            if (!parse_string(key))
                return false;
            skip_ws();
            if (p >= end || *p != ':')
                return false;
            ++p;
            value v;
            if (!parse_value(v))
                return false;
            out.obj->emplace_back(std::move(key), std::move(v));
            skip_ws();
            if (p >= end)
                return false;
            if (*p == ',') {
                ++p;
                continue;
            }
            if (*p == '}') {
                ++p;
                return true;
            }
            return false;
        }
    }
};

inline void dump_string(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            out += c;
            break;
        }
    }
    out += '"';
}

inline void dump_value(const value& v, std::string& out, int indent, int depth) {
    std::string pad(indent * (depth + 1), ' ');
    std::string pad_close(indent * depth, ' ');
    switch (v.type) {
    case value::null_t:
        out += "null";
        break;
    case value::bool_t:
        out += v.b ? "true" : "false";
        break;
    case value::num_t: {
        double d = v.num;
        if (d == std::floor(d) && std::fabs(d) < 1e15) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", (long long)d);
            out += buf;
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", d);
            out += buf;
        }
        break;
    }
    case value::str_t:
        dump_string(v.str, out);
        break;
    case value::arr_t: {
        if (!v.arr || v.arr->empty()) {
            out += "[]";
            break;
        }
        out += "[\n";
        for (size_t i = 0; i < v.arr->size(); ++i) {
            out += pad;
            dump_value((*v.arr)[i], out, indent, depth + 1);
            if (i + 1 < v.arr->size())
                out += ',';
            out += '\n';
        }
        out += pad_close;
        out += ']';
        break;
    }
    case value::obj_t: {
        if (!v.obj || v.obj->empty()) {
            out += "{}";
            break;
        }
        out += "{\n";
        for (size_t i = 0; i < v.obj->size(); ++i) {
            out += pad;
            dump_string((*v.obj)[i].first, out);
            out += ": ";
            dump_value((*v.obj)[i].second, out, indent, depth + 1);
            if (i + 1 < v.obj->size())
                out += ',';
            out += '\n';
        }
        out += pad_close;
        out += '}';
        break;
    }
    }
}

} // namespace detail

inline bool parse(const std::string& text, value& out) {
    detail::parser ps{ text.c_str(), text.c_str() + text.size() };
    if (!ps.parse_value(out))
        return false;
    return true;
}

inline std::string dump(const value& v, int indent = 2) {
    std::string out;
    detail::dump_value(v, out, indent, 0);
    return out;
}

} // namespace json
