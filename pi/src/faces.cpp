#include "faces.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include "json_lite.h"
#include "libdeflate.h"

std::string faceGroupName(const std::string& fileName) {
    std::string stem = fileName;
    size_t dot = stem.rfind('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    std::string g = stem;
    while (!g.empty() && isdigit((unsigned char)g.back())) g.pop_back();
    while (!g.empty() && (g.back() == '_' || g.back() == '-')) g.pop_back();
    return g.empty() ? stem : g;
}

static bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && std::equal(suf.rbegin(), suf.rend(), s.rbegin(),
           [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); });
}

static bool readPack(const std::string& path, FaceClip& clip, int& w, int& h, int& rot, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open"; return false; }
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    static const char MAGIC[] = "FACE1\n";
    if (data.size() < 10 || data.compare(0, 6, MAGIC) != 0) { err = "not a converted face file"; return false; }
    uint32_t n = (uint8_t)data[6] | ((uint8_t)data[7] << 8) | ((uint8_t)data[8] << 16) | ((uint32_t)(uint8_t)data[9] << 24);
    if (10 + (size_t)n > data.size()) { err = "damaged header"; return false; }
    std::string hjson = data.substr(10, n);
    JVal hdr;
    if (!JParser(hjson).parse(hdr) || hdr.type != JVal::OBJ) { err = "bad header"; return false; }
    const JVal* canvas = hdr.get("canvas");
    const JVal* sizes = hdr.get("sizes");
    const JVal* durs = hdr.get("durations");
    const JVal* pitch = hdr.get("pitch");
    if (!canvas || canvas->arr.size() != 2 || !sizes || !durs || !pitch) { err = "header missing fields"; return false; }
    if (hdr.get("bits") && hdr.get("bits")->numOr(16) != 16) { err = "only 16-bit faces are supported"; return false; }
    w = (int)canvas->arr[0].num; h = (int)canvas->arr[1].num;
    rot = hdr.get("rotate") ? (int)hdr.get("rotate")->numOr(0) : 0;
    clip.pitch = (int)pitch->num;
    const JVal* src = hdr.get("source");
    clip.name = src && src->type == JVal::STR ? src->str : path.substr(path.find_last_of('/') + 1);
    clip.group = faceGroupName(clip.name);
    size_t p = 10 + n;
    for (const auto& s : sizes->arr) {
        size_t len = (size_t)s.num;
        if (p + len > data.size()) { err = "file is incomplete, convert it again"; return false; }
        clip.blobs.emplace_back(data.begin() + p, data.begin() + p + len);
        p += len;
    }
    for (const auto& d : durs->arr) clip.durations.push_back(std::max(1.0, d.num));
    if (clip.blobs.size() != clip.durations.size() || p != data.size() || clip.blobs.empty()) {
        err = "file is damaged or incomplete, convert it again"; return false;
    }
    if (clip.pitch < w * 2) { err = "bad pitch"; return false; }
    return true;
}

bool FacePlayer::loadFolder(const std::string& dir, std::string& message) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    std::vector<std::string> files;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (endsWith(n, ".face")) files.push_back(n);
    }
    closedir(d);
    if (files.empty()) return false;
    std::sort(files.begin(), files.end());

    clips.clear(); groups_.clear();
    int bw = 0, bh = 0, brot = 0;
    std::string skipped;
    for (const auto& n : files) {
        FaceClip c; int w, h, rot; std::string err;
        if (!readPack(dir + "/" + n, c, w, h, rot, err)) { skipped += " " + n + " (" + err + ")"; continue; }
        if (clips.empty()) { bw = w; bh = h; brot = rot; }
        else if (w != bw || h != bh || rot != brot) {
            skipped += " " + n + " (converted with different settings)"; continue;
        }
        groups_[c.group].push_back((int)clips.size());
        clips.push_back(std::move(c));
    }
    if (clips.empty()) { message = "No readable faces in " + dir + "." + skipped; return false; }
    width = bw; height = bh; bakedRotate = ((brot % 360) + 360) % 360;
    folder = dir;
    decoded_.assign((size_t)width * height, 0);
    decodedClip_ = decodedFrame_ = -1;
    size_t bytes = 0; for (auto& c : clips) for (auto& b : c.blobs) bytes += b.size();
    char buf[200];
    snprintf(buf, sizeof buf, "Loaded %zu faces from %s (%dx%d, %.1f MB).", clips.size(), dir.c_str(), width, height, bytes / 1e6);
    message = buf;
    if (!skipped.empty()) message += " Skipped:" + skipped;
    if (!has(idle)) idle = groups_.begin()->first;
    started_ = false;
    play(idle, 0);
    return true;
}

bool FacePlayer::load(const std::vector<std::string>& folders, std::string& message) {
    for (const auto& f : folders) {
        std::string msg;
        if (loadFolder(f, msg)) { message = msg; return true; }
        if (!msg.empty()) message = msg;
    }
    if (message.empty()) message = "No .face files found. Run convert.bat on the laptop and copy the faces folder to the SD card.";
    return false;
}

std::vector<std::string> FacePlayer::names() const {
    std::vector<std::string> v;
    for (auto& g : groups_) v.push_back(g.first);
    return v;
}

void FacePlayer::start(int clip, int loops, const std::string& then) {
    clip_ = clip; clipIndex = clip;
    current = clips[clip].group;
    loopsLeft_ = loops;
    then_ = then.empty() ? idle : then;
    thenLoops_ = 0;
    fi_ = 0;
    nextDue_ = (double)now_ + clips[clip].durations[0];
    started_ = true;
}

void FacePlayer::play(const std::string& group, int loops, const std::string& then) {
    auto it = groups_.find(group);
    if (it == groups_.end()) return;
    const auto& variants = it->second;
    int last = lastVariant_.count(group) ? lastVariant_[group] : -1;
    std::vector<int> choices;
    for (int i = 0; i < (int)variants.size(); i++) if (i != last) choices.push_back(i);
    if (choices.empty()) choices.push_back(0);
    int vi = choices[rng_() % choices.size()];
    lastVariant_[group] = vi;
    start(variants[vi], loops, then);
}

void FacePlayer::finishPassThen(const std::string& group, int loops) {
    if (clip_ < 0 || clips.empty()) { play(group, loops); return; }
    loopsLeft_ = 1;          // update() switches to then_ when this pass ends
    then_ = group;
    thenLoops_ = loops;
}

void FacePlayer::playClip(int index, int loops) {
    if (clips.empty()) return;
    int n = (int)clips.size();
    start(((index % n) + n) % n, loops, "");
}

bool FacePlayer::update(uint64_t now_ms) {
    if (clips.empty()) return false;
    bool first = (now_ == 0);
    now_ = now_ms;
    if (first) { nextDue_ = (double)now_ + clips[clip_].durations[fi_]; return true; }
    if ((double)now_ < nextDue_) return false;
    const FaceClip& c = clips[clip_];
    int nxt = fi_ + 1;
    if (nxt >= (int)c.blobs.size()) {
        nxt = 0;
        if (loopsLeft_ > 0) {
            loopsLeft_--;
            if (loopsLeft_ == 0) { play(then_, thenLoops_); return true; }
        }
    }
    fi_ = nxt;
    nextDue_ += c.durations[nxt];
    if (nextDue_ < (double)now_ - 250) nextDue_ = (double)now_ + c.durations[nxt];   // fell behind: resync
    return true;
}

const uint16_t* FacePlayer::frame() {
    if (clips.empty()) return nullptr;
    if (clip_ == decodedClip_ && fi_ == decodedFrame_) return decoded_.data();
    const FaceClip& c = clips[clip_];
    static thread_local libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
    std::vector<uint8_t> raw((size_t)c.pitch * height);
    size_t got = 0;
    const auto& blob = c.blobs[fi_];
    if (libdeflate_zlib_decompress(dec, blob.data(), blob.size(), raw.data(), raw.size(), &got) == LIBDEFLATE_SUCCESS
        && got == raw.size()) {
        for (int y = 0; y < height; y++)
            memcpy(&decoded_[(size_t)y * width], raw.data() + (size_t)y * c.pitch, (size_t)width * 2);
    }
    // on a decode failure the previous picture just stays up
    decodedClip_ = clip_; decodedFrame_ = fi_;
    return decoded_.data();
}
