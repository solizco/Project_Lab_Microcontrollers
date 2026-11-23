#pragma once
// ============================================================
// faces.h -- C++ port of Finn's FacePlayer (gifface.py), playback side only
// ============================================================
// Reads the .face files made by Finn's convert_gifs.py on the laptop:
//   "FACE1\n" + uint32 LE header length + JSON header + one zlib blob per frame
//   frames are RGB565 little-endian, canvas WxH (default 400x240), row pitch in header
// Frames stay compressed in memory and are unpacked one at a time while
// playing (same memory strategy as Finn's player).
//
// Variants: bad_move.face and bad_move2.face are both group "bad_move";
// play("bad_move") picks one at random, never the same one twice in a row.
// Raw GIFs are NOT read here: run convert.bat / convert_gifs.py first.
#include <map>
#include <random>
#include <string>
#include <vector>
#include <stdint.h>

struct FaceClip {
    std::string name;                        // file name, e.g. bad_move2.face
    std::string group;                       // e.g. bad_move
    std::vector<std::vector<uint8_t>> blobs; // zlib-compressed frames
    std::vector<double> durations;           // ms per frame (speed already baked in)
    int pitch = 0;                           // bytes per row in the decoded frame
};

class FacePlayer {
public:
    // Searches the folders in order and loads the first one that has .face files.
    bool load(const std::vector<std::string>& folders, std::string& message);
    bool loadFolder(const std::string& folder, std::string& message);

    bool loaded() const { return !clips.empty(); }
    bool has(const std::string& group) const { return groups_.count(group) > 0; }
    std::vector<std::string> names() const;

    // Same meaning as Finn's play(): loops = how many times (0 = forever),
    // then = what to switch to afterwards (default idle, looping).
    void play(const std::string& group, int loops = 1, const std::string& then = "");
    void playClip(int index, int loops = 0);   // wraps around

    // Advance by the clock. Returns true when the frame to show has changed.
    bool update(uint64_t now_ms);
    // The current frame, decoded (w*h RGB565, tightly packed). Never null once loaded.
    const uint16_t* frame();

    int width = 0, height = 0, bakedRotate = 0;
    std::string folder, idle = "idle", current;  // current = group playing now
    int clipIndex = -1;                           // index into clips of what is playing
    bool oneShotPlaying() const { return clip_ >= 0 && loopsLeft_ > 0; }
    // The current clip keeps looping (cancels a pending finishPassThen).
    void keepLooping() { loopsLeft_ = 0; then_ = idle; thenLoops_ = 0; }
    // Let the current clip finish the pass it is on, then play `group`
    // (loops: 0 = forever). Never cuts a clip off mid-pass.
    void finishPassThen(const std::string& group, int loops = 0);
    std::vector<FaceClip> clips;

private:
    std::map<std::string, std::vector<int>> groups_;
    std::map<std::string, int> lastVariant_;
    std::mt19937 rng_{std::random_device{}()};
    int clip_ = -1, fi_ = 0, loopsLeft_ = 0;
    std::string then_;
    int thenLoops_ = 0;
    double nextDue_ = 0;
    uint64_t now_ = 0;
    bool started_ = false;
    std::vector<uint16_t> decoded_;
    int decodedClip_ = -1, decodedFrame_ = -1;
    void start(int clip, int loops, const std::string& then);
};

// "bad_move2.gif" -> "bad_move", "idle_2" -> "idle"  (same rule as Finn's _group_name)
std::string faceGroupName(const std::string& fileName);
