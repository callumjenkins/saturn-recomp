// saturn-recomp runtime — the disc: a .cue with its .bin files (one per track or
// one for all), sectors by FAD, the TOC, and ISO 9660 lookups for the boot.
//
// FADs are absolute: the first track's INDEX 01 is FAD 150. A file's first
// sector follows the previous file's last; a track starts at its INDEX 01,
// so a pregap stored in the file (INDEX 00) sits before it. Data tracks are
// MODE1/2352 (raw) or MODE1/2048 (the header is made up).
//
// Files and audio tracks can be borrowed from another disc (cdrom_borrow): a borrowed file's directory
// record is rewritten to point past this disc's lead-out, where its sectors are the other disc's, and a
// borrowed track's sectors are the other disc's track, sector for sector, silent past its end.
#include "saturn.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <vector>

struct CueFile { std::string path; uint32_t fad, sectors; int secsize; bool audio; FILE* f; };
struct CueTrack { int num; uint32_t fad; bool audio; };
struct Image { std::vector<CueFile> files; std::vector<CueTrack> tracks; uint32_t leadout = 0; };
static Image g_disc, g_other;

struct Borrowed { uint32_t fad, sectors, other_fad; };      // a file's sectors past the lead-out
static std::vector<Borrowed> g_borrowed;
static std::map<uint32_t, std::array<uint8_t, 2352>> g_rewritten;   // directory sectors as served
static std::vector<int> g_borrowed_tracks;

static uint32_t msf(const std::string& s) {
    int m = 0, sec = 0, fr = 0;
    std::sscanf(s.c_str(), "%d:%d:%d", &m, &sec, &fr);
    return (uint32_t)((m * 60 + sec) * 75 + fr);
}

static bool open_image(const std::string& cue, Image& im) {
    for (CueFile& f : im.files) std::fclose(f.f);
    im = Image{};
    std::vector<CueFile>& files = im.files;
    std::vector<CueTrack>& tracks = im.tracks;
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
                CueFile& p = files.back();
                fad = p.fad + p.sectors;
            }
            first_file = false;
            bool absolute = name.size() > 2 && (name[0] == '/' || name[0] == '\\' || name[1] == ':');
            std::string path = absolute ? name : dir + name;
            FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
            std::fseek(f, 0, SEEK_END);
            long size = std::ftell(f);
            files.push_back({path, fad, 0, 2352, false, f});
            files.back().sectors = (uint32_t)(size / 2352);   // fixed below for 2048-byte files
        } else if (kw == "TRACK") {
            std::string mode;
            ss >> cur_track >> mode;
            cur_audio = mode == "AUDIO";
            CueFile& f = files.back();
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
            if (idx == 1) tracks.push_back({cur_track, files.back().fad + msf(pos), cur_audio});
        }
    }
    if (files.empty() || tracks.empty()) return false;
    im.leadout = files.back().fad + files.back().sectors;
    return true;
}

bool cdrom_open(const std::string& cue) { return open_image(cue, g_disc); }

static const CueFile* file_of(const Image& im, uint32_t fad) {
    for (const CueFile& f : im.files)
        if (fad >= f.fad && fad < f.fad + f.sectors) return &f;
    return nullptr;
}

bool cdrom_is_audio(uint32_t fad) {
    const CueFile* f = file_of(g_disc, fad);
    return f && f->audio;
}

// The track holding `fad`, and where the next begins.
static const CueTrack* track_at(const Image& im, uint32_t fad, uint32_t& end) {
    for (size_t i = 0; i < im.tracks.size(); ++i) {
        end = i + 1 < im.tracks.size() ? im.tracks[i + 1].fad : im.leadout;
        if (fad >= im.tracks[i].fad && fad < end) return &im.tracks[i];
    }
    return nullptr;
}

static bool read_image(const Image& im, uint32_t fad, uint8_t* raw) {
    const CueFile* f = file_of(im, fad);
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

bool cdrom_read(uint32_t fad, uint8_t* raw) {
    if (auto it = g_rewritten.find(fad); it != g_rewritten.end()) {
        std::memcpy(raw, it->second.data(), 2352);
        return true;
    }
    for (const Borrowed& b : g_borrowed)
        if (fad >= b.fad && fad < b.fad + b.sectors) {
            if (!read_image(g_other, b.other_fad + (fad - b.fad), raw)) return false;
            auto bcd = [](uint32_t v) { return (uint8_t)((v / 10) << 4 | (v % 10)); };
            raw[12] = bcd(fad / 75 / 60); raw[13] = bcd(fad / 75 % 60); raw[14] = bcd(fad % 75);
            return true;
        }
    uint32_t end, other_end;
    const CueTrack* t = g_borrowed_tracks.empty() ? nullptr : track_at(g_disc, fad, end);
    if (t && std::find(g_borrowed_tracks.begin(), g_borrowed_tracks.end(), t->num) != g_borrowed_tracks.end())
        for (const CueTrack& o : g_other.tracks)
            if (o.num == t->num) {
                track_at(g_other, o.fad, other_end);
                uint32_t at = o.fad + (fad - t->fad);
                if (at < other_end) return read_image(g_other, at, raw);
                std::memset(raw, 0, 2352);
                return true;
            }
    return read_image(g_disc, fad, raw);
}

int cdrom_tracks(CdTrack* out, int max) {
    int n = 0;
    for (const CueTrack& t : g_disc.tracks)
        if (n < max) out[n++] = {t.fad, t.audio ? 0x01u : 0x41u};
    return n;
}

uint32_t cdrom_leadout() { return g_disc.leadout; }

// ---- ISO 9660 ----------------------------------------------------------------------------
using Reader = std::function<bool(uint32_t fad, uint8_t* raw)>;
static const Reader kServed = cdrom_read;

static std::vector<uint8_t> read_user(uint32_t lba, uint32_t size, const Reader& read = kServed) {
    std::vector<uint8_t> out(size);
    uint8_t raw[2352];
    for (uint32_t o = 0; o < size; o += 2048)
        if (read(150 + lba + o / 2048, raw))
            std::memcpy(out.data() + o, raw + 16, size - o < 2048 ? size - o : 2048);
    return out;
}

// A directory record: its file, and where the record itself sits (FAD and offset in the sector's data).
struct Rec { std::string name; uint32_t lba, size; bool dir; uint32_t at_fad, at_off; };

static std::vector<Rec> list(uint32_t lba, uint32_t size, const Reader& read = kServed) {
    std::vector<Rec> out;
    std::vector<uint8_t> d = read_user(lba, size, read);
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
            out.push_back({name, l, s, (d[o + 25] & 2) != 0, 150 + lba + o / 2048, o % 2048});
        }
        o += len;
    }
    return out;
}

static bool root(uint32_t& lba, uint32_t& size, const Reader& read = kServed) {
    std::vector<uint8_t> pvd = read_user(16, 2048, read);
    if (pvd[0] != 1 || std::memcmp(&pvd[1], "CD001", 5)) return false;
    const uint8_t* r = &pvd[156];
    lba = (uint32_t)r[2] | r[3] << 8 | r[4] << 16 | (uint32_t)r[5] << 24;
    size = (uint32_t)r[10] | r[11] << 8 | r[12] << 16 | (uint32_t)r[13] << 24;
    return true;
}

// The record of the file at `path` ("DIR/FILE.EXT", a leading slash allowed).
static bool find_rec(const char* path, Rec& out, const Reader& read = kServed) {
    uint32_t lba, sz;
    if (!root(lba, sz, read)) return false;
    std::string p = path[0] == '/' ? path + 1 : path;
    size_t start = 0;
    for (;;) {
        size_t slash = p.find('/', start);
        std::string part = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        bool found = false;
        for (const Rec& r : list(lba, sz, read))
            if (r.name == part) { out = r; lba = r.lba; sz = r.size; found = true; break; }
        if (!found) return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

bool cdrom_find(const char* path, uint32_t* fad, uint32_t* size) {
    Rec r;
    if (!find_rec(path, r)) return false;
    *fad = 150 + r.lba;
    *size = r.size;
    return true;
}

static void put_both_endian(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) { p[i] = (uint8_t)(v >> 8 * i); p[7 - i] = (uint8_t)(v >> 8 * i); }
}

bool cdrom_borrow(const std::string& other_cue, const std::string& files, const std::string& tracks, std::string& error) {
    for (CueFile& f : g_other.files) std::fclose(f.f);
    g_other = Image{};
    g_borrowed.clear();
    g_rewritten.clear();
    g_borrowed_tracks.clear();
    if (!open_image(other_cue, g_other)) { error = "cannot open " + other_cue; return false; }
    Reader own = [](uint32_t fad, uint8_t* raw) { return read_image(g_disc, fad, raw); };
    Reader other = [](uint32_t fad, uint8_t* raw) { return read_image(g_other, fad, raw); };
    uint32_t next = g_disc.leadout + 150;
    std::stringstream list_of(files);
    std::string item;
    while (std::getline(list_of, item, ',')) {
        if (item.empty()) continue;
        size_t eq = item.find('=');
        std::string mine = item.substr(0, eq), theirs = eq == std::string::npos ? mine : item.substr(eq + 1);
        Rec m, o;
        if (!find_rec(mine.c_str(), m, own)) { error = mine + " is not on this disc"; return false; }
        if (!find_rec(theirs.c_str(), o, other)) { error = theirs + " is not on " + other_cue; return false; }
        uint32_t sectors = (o.size + 2047) / 2048;
        g_borrowed.push_back({next, sectors, 150 + o.lba});
        auto it = g_rewritten.find(m.at_fad);
        if (it == g_rewritten.end()) {
            std::array<uint8_t, 2352> raw{};
            read_image(g_disc, m.at_fad, raw.data());
            it = g_rewritten.emplace(m.at_fad, raw).first;
        }
        uint8_t* rec = it->second.data() + 16 + m.at_off;
        put_both_endian(rec + 2, next - 150);
        put_both_endian(rec + 10, o.size);
        next += sectors;
    }
    std::stringstream track_list(tracks);
    while (std::getline(track_list, item, ','))
        if (!item.empty()) g_borrowed_tracks.push_back(std::atoi(item.c_str()));
    return true;
}

std::string cdrom_first_file() {
    uint32_t lba, sz;
    if (!root(lba, sz)) return "";
    for (const Rec& r : list(lba, sz))
        if (!r.dir) return r.name;
    return "";
}
