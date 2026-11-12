// saturn-recomp runtime — pads in lockstep (lockstep.h). Messages are bytes: a kind, then little-endian fields, frames
// as 32 bits, which a VBlank count fills after two years.
#include "lockstep.h"
#include <algorithm>
#include <cstring>

namespace {

const char kPads = 'P';
const size_t kMaxSent = 32;           // frames in a pads message
const size_t kMaxReported = 64;       // frames in a loss report: more than the frames two members can be apart
const size_t kHashesKept = 16;

struct Writer {
    std::vector<uint8_t> out;
    explicit Writer(uint8_t kind) { out.push_back(kind); }
    void u8(int v) { out.push_back((uint8_t)v); }
    void u16(uint16_t v) { out.push_back((uint8_t)v); out.push_back((uint8_t)(v >> 8)); }
    void u32(uint64_t v) { for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) out.push_back((uint8_t)(v >> (8 * i))); }
};

struct Reader {
    const std::vector<uint8_t>& in;
    size_t pos = 1;
    bool ok = true;
    explicit Reader(const std::vector<uint8_t>& m) : in(m) {}
    uint64_t take(int bytes) {
        if (pos + bytes > in.size()) { ok = false; return 0; }
        uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v |= (uint64_t)in[pos + i] << (8 * i);
        pos += bytes;
        return v;
    }
    int u8() { return (int)take(1); }
    uint16_t u16() { return (uint16_t)take(2); }
    uint64_t u32() { return take(4); }
    uint64_t u64() { return take(8); }
    bool done() const { return ok && pos == in.size(); }
};

bool kind_is(const std::vector<uint8_t>& m, uint8_t kind) { return !m.empty() && m[0] == kind; }

// `n` frames of `count` pads each, from a message, or nothing if it is short.
std::vector<std::vector<uint16_t>> read_frames(Reader& r, size_t n, int count) {
    std::vector<std::vector<uint16_t>> frames(n, std::vector<uint16_t>(count));
    for (auto& f : frames)
        for (auto& pad : f) pad = r.u16();
    if (!r.ok) frames.clear();
    return frames;
}

}  // namespace

std::vector<uint8_t> lockstep_start_message(const LockstepStart& s) {
    Writer w((uint8_t)LockstepKind::Start);
    w.u32(s.frame);
    w.u8(s.delay);
    w.u8(s.ahead);
    w.u8((int)s.members.size());
    for (const LockstepMember& m : s.members) {
        w.u8((int)m.id.size());
        w.out.insert(w.out.end(), m.id.begin(), m.id.end());
        w.u8(m.first);
        w.u8(m.count);
    }
    return w.out;
}

std::optional<LockstepStart> lockstep_read_start(const std::vector<uint8_t>& m) {
    if (!kind_is(m, (uint8_t)LockstepKind::Start)) return std::nullopt;
    Reader r(m);
    LockstepStart s;
    s.frame = r.u32();
    s.delay = r.u8();
    s.ahead = r.u8();
    const int n = r.u8();
    for (int i = 0; i < n && r.ok; ++i) {
        LockstepMember member;
        const int length = r.u8();
        if (r.pos + length > m.size()) return std::nullopt;
        member.id.assign(m.begin() + (long)r.pos, m.begin() + (long)(r.pos + length));
        r.pos += length;
        member.first = r.u8();
        member.count = r.u8();
        if (member.first + member.count > kLockstepSlots) return std::nullopt;
        s.members.push_back(std::move(member));
    }
    if (!r.done() || s.members.empty() || s.delay < (s.ahead ? 0 : 1)) return std::nullopt;
    return s;
}

std::vector<uint8_t> lockstep_time_message(LockstepKind kind, uint64_t ns) {
    Writer w((uint8_t)kind);
    w.u64(ns);
    return w.out;
}

std::optional<uint64_t> lockstep_read_time(const std::vector<uint8_t>& m) {
    Reader r(m);
    const uint64_t ns = r.u64();
    if (!r.done()) return std::nullopt;
    return ns;
}

int lockstep_delay_for(uint64_t rtt_ns, bool rollback) {
    const uint64_t frame_ns = 1'000'000'000 / 60, margin_ns = 8'000'000;
    const uint64_t frames = (rtt_ns / 2 + margin_ns + frame_ns - 1) / frame_ns + 1;
    const int delay = (int)std::clamp<uint64_t>(frames, 2, 8);
    return rollback ? (delay + 1) / 2 : delay;
}

uint64_t lockstep_hash(const uint8_t* p, size_t n, uint64_t seed) {
    uint64_t h = seed ^ 0xCBF29CE484222325ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t word;
        std::memcpy(&word, p + i, 8);
        h = (h ^ word) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

Lockstep::Lockstep(const LockstepStart& start, int self)
    : members_(start.members), members_state_(start.members.size()), self_(self), delay_(start.delay),
      ahead_(start.ahead), start_(start.frame), now_(start.frame) {
    for (size_t i = 0; i < members_.size(); ++i) {
        Member& m = members_state_[i];
        m.base = start_;
        m.acked = start_;
        m.inputs.assign(delay_, std::vector<uint16_t>(members_[i].count));   // the frames before the delay: idle
    }
}

int Lockstep::member_of(const std::string& id) const {
    for (size_t i = 0; i < members_.size(); ++i)
        if (members_[i].id == id) return (int)i;
    return -1;
}

void Lockstep::local(uint64_t frame, const uint16_t* pads) {
    now_ = std::max(now_, frame);
    const uint64_t at = frame + delay_;
    if (at < through(self_)) return;
    std::vector<uint16_t> now(pads, pads + members_[self_].count);
    while (through(self_) < at) store(self_, through(self_), members_state_[self_].inputs.empty() ? now : members_state_[self_].inputs.back());
    store(self_, at, std::move(now));
}

std::vector<uint8_t> Lockstep::pads_message(int member) const {
    const Member& own = members_state_[self_];
    const uint64_t from = std::max(members_state_[member].acked, own.base);
    const uint64_t to = std::min(through(self_), from + kMaxSent);
    const Member& them = members_state_[member];
    const int64_t gap = them.heard ? (int64_t)them.at - (int64_t)now_ : 0;
    Writer w(kPads);
    w.u32(through(member));
    w.u32(now_);
    w.u16((uint16_t)(int16_t)std::clamp<int64_t>(gap, INT16_MIN, INT16_MAX));
    w.u32(from);
    w.u8((int)(to - from));
    w.u8(members_[self_].count);
    for (uint64_t f = from; f < to; ++f)
        for (uint16_t pad : own.inputs[f - own.base]) w.u16(pad);
    return w.out;
}

bool Lockstep::hear_pads(int member, const std::vector<uint8_t>& m) {
    if (!kind_is(m, kPads) || member == self_) return false;
    Reader r(m);
    const uint64_t ack = r.u32(), at = r.u32();
    const int gap = (int16_t)r.u16();
    const uint64_t first = r.u32();
    const size_t n = r.u8();
    const int count = r.u8();
    if (!r.ok || count != members_[member].count) return false;
    auto frames = read_frames(r, n, count);
    if (!r.done()) return false;
    Member& them = members_state_[member];
    them.acked = std::max(them.acked, ack);
    if (!them.heard || at >= them.at) {
        them.heard = true;
        them.at = at;
        them.gap = gap;
    }
    // Once its loss is reported, what this peer holds of it must stay what the report said.
    if (them.leaving || them.gone_from) return true;
    for (size_t i = 0; i < frames.size(); ++i) {
        const uint64_t f = first + i;
        if (f < through(member)) continue;
        if (f > through(member)) break;
        store(member, f, std::move(frames[i]));
    }
    return true;
}

bool Lockstep::ready(uint64_t frame) const { return frame < confirmed(); }

uint64_t Lockstep::confirmed() const {
    uint64_t first = UINT64_MAX;
    for (size_t i = 0; i < members_.size(); ++i)
        if (!members_state_[i].gone_from) first = std::min(first, through((int)i));
    return first;
}

std::vector<int> Lockstep::waiting_on(uint64_t frame) const {
    std::vector<int> out;
    for (size_t i = 0; i < members_.size(); ++i) {
        const Member& m = members_state_[i];
        if (m.gone_from && frame >= *m.gone_from) continue;
        if (frame >= through((int)i)) out.push_back((int)i);
    }
    return out;
}

void Lockstep::pads(uint64_t frame, uint16_t out[kLockstepSlots]) const {
    std::fill(out, out + kLockstepSlots, 0);
    for (size_t i = 0; i < members_.size(); ++i) {
        const Member& m = members_state_[i];
        if (m.gone_from && frame >= *m.gone_from) continue;
        const std::vector<uint16_t>* in = input((int)i, frame);
        if (!in && frame >= through((int)i) && !m.inputs.empty()) in = &m.inputs.back();
        if (in) std::copy(in->begin(), in->end(), out + members_[i].first);
    }
}

bool Lockstep::owned(int slot) const {
    for (const LockstepMember& m : members_)
        if (slot >= m.first && slot < m.first + m.count) return true;
    return false;
}

std::vector<uint8_t> Lockstep::lose(int member) {
    Member& gone = members_state_[member];
    if (member == self_ || gone.leaving || gone.gone_from) return {};
    gone.leaving = true;
    for (size_t i = 0; i < members_.size(); ++i) {
        const Member& m = members_state_[i];
        if ((int)i != self_ && (int)i != member && !m.leaving && !m.gone_from) gone.reports_due.push_back((int)i);
    }
    // A member still owed a report on another loss will not send it now.
    for (size_t i = 0; i < members_.size(); ++i) {
        auto& due = members_state_[i].reports_due;
        if (!members_state_[i].leaving || (int)i == member) continue;
        due.erase(std::remove(due.begin(), due.end(), member), due.end());
        settle((int)i);
    }
    const uint64_t to = through(member);
    const uint64_t from = std::max(gone.base, to > kMaxReported ? to - kMaxReported : 0);
    gone.latest = std::max(gone.latest, to);
    Writer w((uint8_t)LockstepKind::Lost);
    w.u8(member);
    w.u32(to);
    w.u32(from);
    w.u8((int)(to - from));
    for (uint64_t f = from; f < to; ++f)
        for (uint16_t pad : gone.inputs[f - gone.base]) w.u16(pad);
    settle(member);
    return w.out;
}

Lockstep::LostHeard Lockstep::hear_lost(int from, const std::vector<uint8_t>& m) {
    LostHeard heard;
    if (!kind_is(m, (uint8_t)LockstepKind::Lost)) return heard;
    Reader r(m);
    const int member = r.u8();
    const uint64_t to = r.u32(), first = r.u32();
    const size_t n = r.u8();
    if (!r.ok || member < 0 || member >= (int)members_.size()) return heard;
    if (member == self_) {
        heard.self = true;
        return heard;
    }
    auto frames = read_frames(r, n, members_[member].count);
    if (!r.done()) return heard;
    Member& gone = members_state_[member];
    if (!gone.leaving && !gone.gone_from) heard.report = lose(member);
    auto& due = gone.reports_due;
    if (!gone.leaving || std::find(due.begin(), due.end(), from) == due.end()) return heard;
    due.erase(std::remove(due.begin(), due.end(), from), due.end());
    gone.latest = std::max(gone.latest, to);
    for (size_t i = 0; i < frames.size(); ++i) gone.reported.emplace_back(first + i, std::move(frames[i]));
    settle(member);
    return heard;
}

std::vector<uint8_t> Lockstep::hash_message(uint64_t frame, uint64_t hash) {
    own_hashes_.emplace_back(frame, hash);
    if (own_hashes_.size() > kHashesKept) own_hashes_.erase(own_hashes_.begin());
    for (const auto& [from, h] : heard_hashes_)
        if (h.first == frame && h.second != hash && (!parted_ || frame < *parted_)) {
            parted_ = frame;
            parted_with_ = from;
        }
    Writer w((uint8_t)LockstepKind::Hash);
    w.u32(frame);
    w.u64(hash);
    return w.out;
}

void Lockstep::hear_hash(int from, const std::vector<uint8_t>& m) {
    if (!kind_is(m, (uint8_t)LockstepKind::Hash)) return;
    Reader r(m);
    const uint64_t frame = r.u32(), hash = r.u64();
    if (!r.done()) return;
    for (const auto& [f, h] : own_hashes_)
        if (f == frame && h != hash && (!parted_ || frame < *parted_)) {
            parted_ = frame;
            parted_with_ = from;
        }
    heard_hashes_.push_back({from, {frame, hash}});
    if (heard_hashes_.size() > kHashesKept * members_.size()) heard_hashes_.erase(heard_hashes_.begin());
}

void Lockstep::ran(uint64_t frame, const uint16_t pads[kLockstepSlots]) {
    if (ran_.empty() || frame < ran_base_ || frame > ran_base_ + ran_.size()) {
        ran_.clear();
        ran_base_ = frame;
    }
    ran_.resize(frame - ran_base_);
    ran_.emplace_back();
    std::copy(pads, pads + kLockstepSlots, ran_.back().begin());
}

std::optional<uint64_t> Lockstep::first_wrong() {
    std::optional<uint64_t> wrong;
    for (size_t i = 0; i < ran_.size() && !wrong; ++i) {
        uint16_t now[kLockstepSlots];
        pads(ran_base_ + i, now);
        if (!std::equal(now, now + kLockstepSlots, ran_[i].begin())) wrong = ran_base_ + i;
    }
    const uint64_t right_before = std::min(confirmed(), wrong.value_or(UINT64_MAX));
    while (ran_.size() > 1 && ran_base_ < right_before) {
        ran_.pop_front();
        ++ran_base_;
    }
    return wrong;
}

int Lockstep::lead() const {
    int lead = 0;
    for (size_t i = 0; i < members_.size(); ++i) {
        const Member& m = members_state_[i];
        if ((int)i == self_ || !m.heard || m.gone_from) continue;
        // It said this peer was `gap` frames ahead of it, and this peer finds it `mine` frames ahead of this one. The
        // time the messages took is in both and cancels out of the difference.
        const int64_t mine = (int64_t)m.at - (int64_t)now_;
        lead = std::max(lead, (int)((m.gap - mine) / 2));
    }
    return lead;
}

void Lockstep::forget_before(uint64_t frame) {
    for (Member& m : members_state_)
        while (m.base < frame && m.inputs.size() > 1) {
            m.inputs.pop_front();
            ++m.base;
        }
}

uint64_t Lockstep::through(int member) const {
    const Member& m = members_state_[member];
    return m.base + m.inputs.size();
}

const std::vector<uint16_t>* Lockstep::input(int member, uint64_t frame) const {
    const Member& m = members_state_[member];
    if (frame < m.base || frame >= through(member)) return nullptr;
    return &m.inputs[frame - m.base];
}

void Lockstep::store(int member, uint64_t frame, std::vector<uint16_t> pads) {
    if (frame == through(member)) members_state_[member].inputs.push_back(std::move(pads));
}

// A loss whose reports are all in: its pads go idle after the latest frame any member held, and the frames this peer
// lacks before that come from the reports.
void Lockstep::settle(int member) {
    Member& gone = members_state_[member];
    if (!gone.leaving || !gone.reports_due.empty()) return;
    while (through(member) < gone.latest) {
        const uint64_t f = through(member);
        auto it = std::find_if(gone.reported.begin(), gone.reported.end(), [f](const auto& r) { return r.first == f; });
        store(member, f, it != gone.reported.end() ? it->second : std::vector<uint16_t>(members_[member].count));
    }
    gone.gone_from = gone.latest;
    gone.leaving = false;
    gone.reported.clear();
}
