#pragma once
// Settings file (replaces ESP32 NVS). Plain "key=value" lines.
// Default location: ~/.config/connect4/settings.conf
// A missing or corrupt file is NOT an error: defaults are used.
#include <stdint.h>
#include <string>

struct Settings {
    uint8_t last_mode = 2;   // 0 Pass&Play, 1 Noob, 2 Pro, 3 Hacker. Menu starts here.
    bool screen_only = false; // play on the screen only, gantry parked (Options > Screen only)
    uint8_t screen_input = 0; // in screen-only games: 0 = Pi keyboard, 1 = Motion Controller buttons

    static std::string defaultPath();
    // Returns true if a valid file was read. On false, *this is left at defaults.
    bool load(const std::string& path);
    // Writes to a temp file then renames, so a power cut mid-save can't leave
    // a half-written file. Returns false if the write failed.
    bool save(const std::string& path) const;
};
