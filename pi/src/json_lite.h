#pragma once
// Minimal JSON reader: enough for Finn's .face headers and connect4_stats.json.
// Objects, arrays, numbers, strings, true/false/null. No external libraries.
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct JVal {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JVal> arr;
    std::map<std::string, JVal> obj;

    const JVal* get(const std::string& k) const {
        if (type != OBJ) return nullptr;
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
    double numOr(double d) const { return type == NUM ? num : d; }
};

class JParser {
public:
    explicit JParser(const std::string& s) : s_(s) {}
    bool parse(JVal& out) { ws(); if (!value(out)) return false; ws(); return i_ == s_.size(); }
private:
    const std::string& s_;
    size_t i_ = 0;
    void ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) i_++; }
    bool lit(const char* w) { size_t n = strlen_(w); if (s_.compare(i_, n, w) != 0) return false; i_ += n; return true; }
    static size_t strlen_(const char* w) { size_t n = 0; while (w[n]) n++; return n; }
    bool value(JVal& v) {
        if (i_ >= s_.size()) return false;
        char c = s_[i_];
        if (c == '{') return object(v);
        if (c == '[') return array(v);
        if (c == '"') { v.type = JVal::STR; return string(v.str); }
        if (lit("true"))  { v.type = JVal::BOOL; v.b = true;  return true; }
        if (lit("false")) { v.type = JVal::BOOL; v.b = false; return true; }
        if (lit("null"))  { v.type = JVal::NUL; return true; }
        return number(v);
    }
    bool number(JVal& v) {
        const char* start = s_.c_str() + i_;
        char* end = nullptr;
        double d = strtod(start, &end);
        if (end == start) return false;
        i_ += (size_t)(end - start);
        v.type = JVal::NUM; v.num = d;
        return true;
    }
    bool string(std::string& out) {
        i_++;  // opening quote
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c == '\\' && i_ < s_.size()) {
                char e = s_[i_++];
                switch (e) {
                case 'n': out += '\n'; break; case 't': out += '\t'; break;
                case 'r': out += '\r'; break; case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                case 'u': if (i_ + 4 <= s_.size()) { out += '?'; i_ += 4; } break;   // non-ASCII: not needed here
                default: out += e;
                }
            } else out += c;
        }
        return false;
    }
    bool array(JVal& v) {
        v.type = JVal::ARR; i_++; ws();
        if (i_ < s_.size() && s_[i_] == ']') { i_++; return true; }
        for (;;) {
            JVal e; ws(); if (!value(e)) return false;
            v.arr.push_back(std::move(e)); ws();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') { i_++; continue; }
            if (s_[i_] == ']') { i_++; return true; }
            return false;
        }
    }
    bool object(JVal& v) {
        v.type = JVal::OBJ; i_++; ws();
        if (i_ < s_.size() && s_[i_] == '}') { i_++; return true; }
        for (;;) {
            ws(); if (i_ >= s_.size() || s_[i_] != '"') return false;
            std::string k; if (!string(k)) return false;
            ws(); if (i_ >= s_.size() || s_[i_] != ':') return false; i_++;
            ws(); JVal e; if (!value(e)) return false;
            v.obj[k] = std::move(e); ws();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') { i_++; continue; }
            if (s_[i_] == '}') { i_++; return true; }
            return false;
        }
    }
};
