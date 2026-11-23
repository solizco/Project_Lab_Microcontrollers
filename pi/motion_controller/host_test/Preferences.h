#pragma once
#include <map>
#include <string>
extern std::map<std::string, int> g_nvs;     // simulated flash, survives a "restart"
struct Preferences {
    void begin(const char*, bool) {}
    void end() {}
    int getInt(const char* k, int def) { auto it = g_nvs.find(k); return it == g_nvs.end() ? def : it->second; }
    void putInt(const char* k, int v) { g_nvs[k] = v; }
};
