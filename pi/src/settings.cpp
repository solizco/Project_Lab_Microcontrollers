#include "settings.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

std::string Settings::defaultPath() {
    const char* home = getenv("HOME");
    std::string base = home ? home : ".";
    return base + "/.config/connect4/settings.conf";
}

bool Settings::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    Settings tmp;
    std::string line;
    bool any = false, v2 = false;
    uint8_t input = tmp.screen_input;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        if (key == "version") { v2 = (val == "2"); }
        else if (key == "last_mode") {
            char* end = nullptr;
            long v = strtol(val.c_str(), &end, 10);
            if (end != val.c_str() && v >= 0 && v <= 3) { tmp.last_mode = (uint8_t)v; any = true; }
        } else if (key == "screen_input") {
            if (val == "keyboard" || val == "buttons") { input = (val == "buttons") ? 1 : 0; any = true; }
        } else if (key == "screen_only") {
            if (val == "1" || val == "0") { tmp.screen_only = (val == "1"); any = true; }
        }
    }
    if (!any) return false;
    if (v2) tmp.screen_input = input;     // version 1 files saved keyboard as the old default: ignore it
    *this = tmp;
    return true;
}

static void mkdirs(const std::string& path) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

bool Settings::save(const std::string& path) const {
    mkdirs(path);
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        f << "version=2\n";
        f << "last_mode=" << (int)last_mode << "\n";
        f << "screen_only=" << (screen_only ? 1 : 0) << "\n";
        f << "screen_input=" << (screen_input ? "buttons" : "keyboard") << "\n";
        f.flush();
        if (!f) return false;
    }
    return rename(tmp.c_str(), path.c_str()) == 0;
}
