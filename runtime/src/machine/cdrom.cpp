// saturn-recomp runtime — the disc: a .cue with its .bin files (one per track or
// one for all), sectors by FAD, the TOC, and ISO 9660 lookups for the boot.
//
// FADs are absolute: the first track's INDEX 01 is FAD 150. A file's first
// sector follows the previous file's last; a track starts at its INDEX 01,
// so a pregap stored in the file (INDEX 00) sits before it. Data tracks are
// MODE1/2352 (raw) or MODE1/2048 (the header is made up).
#include "saturn.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

struct CueFile { std::string path; uint32_t fad, sectors; int secsize; bool audio; FILE* f; };
struct CueTrack { int num; uint32_t fad; bool audio; };
static std::vector<CueFile> g_files;
static std::vector<CueTrack> g_tracks;
static uint32_t g_leadout;

static uint32_t msf(const std::string& s) {
    int m = 0, sec = 0, fr = 0;
    std::sscanf(s.c_str(), "%d:%d:%d", &m, &sec, &fr);
    return (uint32_t)((m * 60 + sec) * 75 + fr);
}

bool cdrom_open(const std::string& cue) {
    std::ifstream in(cue);
    if (!in) return false;
    std::string dir = cue.substr(0, cue.find_last_of("/\\") + 1), line;
    uint32_t fad = 150;
    int cur_track = 0;
    bool cur_audio = false, first_file = true;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string kw;
        ss >> kw;
        if (kw == "FILE") {
            size_t q1 = line.find('"'), q2 = line.rfind('"');
            std::string name = q1 != std::string::npos && q2 > q1 ? line.substr(q1 + 1, q2 - q1 - 1) : "";
            if (!first_file) {
                CueFile& p = g_files.back();
                fad = p.fad + p.sectors;
            }
            first_file = false;
            bool absolute = name.size() > 2 && (name[0] == '/' || name[0] == '\\' || name[1] == ':');
            std::string path = absolute ? name : dir + name;
            FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
            std::fseek(f, 0, SEEK_END);
            long size = std::ftell(f);
            g_files.push_back({path, fad, 0, 2352, false, f});
            g_files.back().sectors = (uint32_t)(size / 2352);   // fixed below for 2048-byte files
        } else if (kw == "TRACK") {
            std::string mode;
            ss >> cur_track >> mode;
            cur_audio = mode == "AUDIO";
            CueFile& f = g_files.back();
            f.audio = cur_audio;
            if (mode == "MODE1/2048") {
                f.secsize = 2048;
                std::fseek(f.f, 0, SEEK_END);
                f.sectors = (uint32_t)(std::ftell(f.f) / 2048);
            }
        } else if (kw == "INDEX") {
            int idx;
            std::string pos;
            ss >> idx >> pos;
            if (idx == 1) g_tracks.push_back({cur_track, g_files.back().fad + msf(pos), cur_audio});
        }
    }
    if (g_files.empty() || g_tracks.empty()) return false;
    g_leadout = g_files.back().fad + g_files.back().sectors;
    return true;
}

static const CueFile* file_of(uint32_t fad) {
    for (const CueFile& f : g_files)
        if (fad >= f.fad && fad < f.fad + f.sectors) return &f;
    return nullptr;
}

bool cdrom_is_audio(uint32_t fad) {
    const CueFile* f = file_of(fad);
    return f && f->audio;
}

bool cdrom_read(uint32_t fad, uint8_t* raw) {
    const CueFile* f = file_of(fad);
    if (!f) return false;
    long pos = (long)(fad - f->fad) * f->secsize;
    std::fseek(f->f, pos, SEEK_SET);
    if (f->secsize == 2352) return std::fread(raw, 1, 2352, f->f) == 2352;
    // MODE1/2048: sync, header (BCD MSF, mode 1), data; EDC/ECC left zero
    std::memset(raw, 0, 2352);
    std::memset(raw + 1, 0xFF, 10);
    auto bcd = [](uint32_t v) { return (uint8_t)((v / 10) << 4 | (v % 10)); };
    raw[12] = bcd(fad / 75 / 60); raw[13] = bcd(fad / 75 % 60); raw[14] = bcd(fad % 75); raw[15] = 1;
    return std::fread(raw + 16, 1, 2048, f->f) == 2048;
}

int cdrom_tracks(CdTrack* out, int max) {
    int n = 0;
    for (const CueTrack& t : g_tracks)
        if (n < max) out[n++] = {t.fad, t.audio ? 0x01u : 0x41u};
    return n;
}

uint32_t cdrom_leadout() { return g_leadout; }

// ---- ISO 9660 ----------------------------------------------------------------------------
static std::vector<uint8_t> read_user(uint32_t lba, uint32_t size) {
    std::vector<uint8_t> out(size);
    uint8_t raw[2352];
    for (uint32_t o = 0; o < size; o += 2048)
        if (cdrom_read(150 + lba + o / 2048, raw))
            std::memcpy(out.data() + o, raw + 16, size - o < 2048 ? size - o : 2048);
    return out;
}

struct Rec { std::string name; uint32_t lba, size; bool dir; };

static std::vector<Rec> list(uint32_t lba, uint32_t size) {
    std::vector<Rec> out;
    std::vector<uint8_t> d = read_user(lba, size);
    for (uint32_t o = 0; o < size;) {
        uint8_t len = d[o];
        if (!len) { o = (o / 2048 + 1) * 2048; continue; }
        uint32_t l = (uint32_t)d[o + 2] | d[o + 3] << 8 | d[o + 4] << 16 | (uint32_t)d[o + 5] << 24;
        uint32_t s = (uint32_t)d[o + 10] | d[o + 11] << 8 | d[o + 12] << 16 | (uint32_t)d[o + 13] << 24;
        uint8_t nlen = d[o + 32];
        std::string name((const char*)&d[o + 33], nlen);
        if (!(nlen == 1 && (name[0] == 0 || name[0] == 1))) {
            size_t semi = name.find(';');
            if (semi != std::string::npos) name.resize(semi);
            if (!name.empty() && name.back() == '.') name.pop_back();
            out.push_back({name, l, s, (d[o + 25] & 2) != 0});
        }
        o += len;
    }
    return out;
}

static bool root(uint32_t& lba, uint32_t& size) {
    std::vector<uint8_t> pvd = read_user(16, 2048);
    if (pvd[0] != 1 || std::memcmp(&pvd[1], "CD001", 5)) return false;
    const uint8_t* r = &pvd[156];
    lba = (uint32_t)r[2] | r[3] << 8 | r[4] << 16 | (uint32_t)r[5] << 24;
    size = (uint32_t)r[10] | r[11] << 8 | r[12] << 16 | (uint32_t)r[13] << 24;
    return true;
}

bool cdrom_find(const char* path, uint32_t* fad, uint32_t* size) {
    uint32_t lba, sz;
    if (!root(lba, sz)) return false;
    std::string p = path;
    size_t start = 0;
    for (;;) {
        size_t slash = p.find('/', start);
        std::string part = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        bool found = false;
        for (const Rec& r : list(lba, sz))
            if (r.name == part) { lba = r.lba; sz = r.size; found = true; break; }
        if (!found) return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    *fad = 150 + lba;
    *size = sz;
    return true;
}

std::string cdrom_first_file() {
    uint32_t lba, sz;
    if (!root(lba, sz)) return "";
    for (const Rec& r : list(lba, sz))
        if (!r.dir) return r.name;
    return "";
}
