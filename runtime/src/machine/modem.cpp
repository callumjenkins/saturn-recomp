// saturn-recomp runtime — a NetLink modem (modem.h).
//
// The UART is a 16550 with its FIFO: the software sets the baud rate through the divisor latch, polls the
// line status for received bytes and the modem status for the carrier. What the modem says reaches the
// FIFO at the baud rate, a byte each start bit, data bits, parity and stop bits.
//
// In command mode a line ending in CR is an AT command; E, V, Q, Z, &F, S0 (rings before answering), A, D,
// H and O act, and anything else is accepted with OK. Online, every byte goes down the line until
// "+++" with a second of silence either side returns to command mode with the call held.
#include "modem.h"
#include "saturn.h"
#include <cctype>
#include <cstdio>

std::unique_ptr<Modem> g_modem;

namespace {

const uint64_t kSecond = 1000000000ull;
const uint64_t kRingEvery = 6 * kSecond;
const size_t kFifo = 16;

enum { kDcd = 0x80, kRi = 0x40, kDeltaDcd = 0x08, kRiEnded = 0x04 };

struct NoLine : ModemLine {
    bool dial(const std::string&) override { return false; }
    bool answered() override { return false; }
    bool ringing() override { return false; }
    bool answer() override { return false; }
    void hang_up() override {}
    bool connected() override { return false; }
    void send(uint8_t) override {}
    bool receive(uint8_t&) override { return false; }
};

}  // namespace

std::unique_ptr<ModemLine> modem_no_line() { return std::make_unique<NoLine>(); }

Modem::Modem(std::unique_ptr<ModemLine> line) : line_(std::move(line)) {}

uint64_t Modem::byte_ns() const {
    unsigned divisor = (unsigned)dlm_ << 8 | dll_;
    unsigned bits = 1 + 5 + (lcr_ & 3) + (lcr_ & 4 ? 2 : 1) + (lcr_ & 8 ? 1 : 0);
    return (uint64_t)bits * kSecond * (divisor ? divisor : 1) / 115200;
}

void Modem::result(const char* text) {
    sat_note("modem: <- %s", text);
    if (quiet_) return;
    std::string s;
    if (verbose_) {
        s = std::string("\r\n") + text + "\r\n";
    } else {
        static const struct { const char* text; const char* code; } codes[] = {
            {"OK", "0"}, {"CONNECT", "1"}, {"RING", "2"}, {"NO CARRIER", "3"}, {"ERROR", "4"}};
        s = "4";
        for (auto& c : codes)
            if (std::string(text).rfind(c.text, 0) == 0) s = c.code;
        s += "\r";
    }
    to_host_.insert(to_host_.end(), s.begin(), s.end());
}

void Modem::connect(uint64_t now) {
    online_ = data_ = true;
    rings_ = 0;
    next_ring_ = 0;
    pluses_ = 0;
    escape_at_ = 0;
    last_tx_ = now;
    msr_ = (msr_ & ~kRi) | kDcd | kDeltaDcd;
    result("CONNECT 28800");
}

void Modem::bring_up(uint64_t now) {
    if (online_ || calling_until_ || now < next_try_) return;
    next_try_ = now + kSecond / 10;
    bool q = quiet_;
    quiet_ = true;
    if (line_->ringing() && line_->answer()) connect(now);
    else if (line_->dial("")) calling_until_ = ~0ull;
    quiet_ = q;
}

void Modem::dialled(bool started, uint64_t now) {
    if (started) calling_until_ = now + (uint64_t)wait_for_carrier_ * kSecond;
    else result("NO CARRIER");
}

void Modem::drop() {
    if (!online_) return;
    line_->hang_up();
    online_ = data_ = false;
    msr_ = (msr_ & ~kDcd) | kDeltaDcd;
    result("NO CARRIER");
}

void Modem::hang_up() {
    if (calling_until_) {
        calling_until_ = 0;
        line_->hang_up();
    }
    if (online_) {
        line_->hang_up();
        online_ = data_ = false;
        msr_ = (msr_ & ~kDcd) | kDeltaDcd;
    }
}

// Z and &F: the settings as at power-on, and a call held in command mode ends.
void Modem::reset() {
    hang_up();
    echo_ = verbose_ = true;
    quiet_ = false;
    auto_answer_ = 0;
}

void Modem::command(const std::string& text, uint64_t now) {
    size_t i = 0;
    auto number = [&](int fallback) {
        if (i >= text.size() || !std::isdigit((unsigned char)text[i])) return fallback;
        int n = 0;
        while (i < text.size() && std::isdigit((unsigned char)text[i])) n = n * 10 + (text[i++] - '0');
        return n;
    };
    while (i < text.size()) {
        char c = (char)std::toupper((unsigned char)text[i++]);
        switch (c) {
        case ' ': break;
        case 'E': echo_ = number(0) != 0; break;
        case 'V': verbose_ = number(0) != 0; break;
        case 'Q': quiet_ = number(0) != 0; break;
        case 'Z': reset(); number(0); break;
        case '&':
            if (i < text.size() && std::toupper((unsigned char)text[i]) == 'F') reset();
            if (i < text.size()) ++i;
            number(0);
            break;
        case '\\': case '%':
            if (i < text.size()) ++i;
            number(0);
            break;
        case 'S': {
            int reg = number(0);
            if (i < text.size() && text[i] == '=') {
                ++i;
                int v = number(0);
                if (reg == 0) auto_answer_ = v;
                if (reg == 7) wait_for_carrier_ = v;
            } else if (i < text.size() && text[i] == '?') {
                ++i;
                char buf[8];
                std::snprintf(buf, sizeof buf, "%03d", reg == 0 ? auto_answer_ : 0);
                result(buf);
            }
            break;
        }
        case 'H':
            number(0);
            hang_up();
            break;
        case 'A':
            if (line_->ringing() && line_->answer()) connect(now);
            else result("NO CARRIER");
            return;
        case 'D': {
            std::string digits;
            for (; i < text.size(); ++i) {
                char d = (char)std::toupper((unsigned char)text[i]);
                if (std::isdigit((unsigned char)d) || d == '*' || d == '#') digits += d;
            }
            dialled(line_->dial(digits), now);
            return;
        }
        case 'O':
            number(0);
            if (!online_) { result("NO CARRIER"); return; }
            data_ = true;
            result("CONNECT 28800");
            return;
        default:
            number(0);
            break;
        }
    }
    result("OK");
}

void Modem::from_host(uint8_t b, uint64_t now) {
    if (data_) {
        line_->send(b);
        if (b == '+' && (pluses_ || now - last_tx_ >= kSecond)) {
            if (++pluses_ == 3) escape_at_ = now + kSecond;
        } else {
            pluses_ = 0;
            escape_at_ = 0;
        }
        last_tx_ = now;
        return;
    }
    if (echo_) to_host_.push_back(b);
    if (b == '\r') {
        std::string line = cmd_;
        cmd_.clear();
        if (line.size() >= 2 && std::toupper((unsigned char)line[0]) == 'A' && line[1] == '/') line = last_cmd_;
        if (line.size() >= 2 && std::toupper((unsigned char)line[0]) == 'A' && std::toupper((unsigned char)line[1]) == 'T') {
            last_cmd_ = line;
            sat_note("modem: -> %s", line.c_str());
            command(line.substr(2), now);
        }
    } else if (b == 8) {
        if (!cmd_.empty()) cmd_.pop_back();
    } else if (b != '\n' && cmd_.size() < 256) {
        cmd_ += (char)b;
    }
}

// A byte queued while the modem was quiet starts on its way when it is queued, not when the queue last moved.
void Modem::idle_until(uint64_t now) {
    if (to_host_.empty() && next_rx_ < now) next_rx_ = now;
}

void Modem::poll(uint64_t now) {
    idle_until(now);
    if (line_->leased()) bring_up(now);
    if (calling_until_) {
        if (line_->answered()) {
            calling_until_ = 0;
            bool q = quiet_;
            quiet_ = quiet_ || line_->leased();
            connect(now);
            quiet_ = q;
        } else if (now >= calling_until_ || !line_->connected()) {
            calling_until_ = 0;
            line_->hang_up();
            result("NO CARRIER");
        }
    }
    if (online_ && !line_->connected()) drop();
    if (data_ && escape_at_ && now >= escape_at_) {
        data_ = false;
        escape_at_ = 0;
        pluses_ = 0;
        result("OK");
    }
    if (!online_) {
        if (line_->ringing()) {
            if (now >= next_ring_) {
                next_ring_ = now + kRingEvery;
                msr_ |= kRi;
                result("RING");
                if (auto_answer_ && ++rings_ >= auto_answer_ && line_->answer()) connect(now);
            }
        } else if (msr_ & kRi) {
            msr_ = (msr_ & ~kRi) | kRiEnded;
            rings_ = 0;
            next_ring_ = 0;
        }
    }
    uint8_t b;
    while (data_ && to_host_.size() < 4096 && line_->receive(b)) to_host_.push_back(b);
    while (!to_host_.empty() && fifo_.size() < kFifo && next_rx_ + byte_ns() <= now) {
        fifo_.push_back(to_host_.front());
        to_host_.pop_front();
        next_rx_ += byte_ns();
    }
}

uint8_t Modem::pending() const {
    if ((ier_ & 4) && overrun_) return 0x06;
    if ((ier_ & 1) && !fifo_.empty()) return 0x04;
    if ((ier_ & 2) && thre_) return 0x02;
    if ((ier_ & 8) && (msr_ & 0x0F)) return 0x00;
    return 0x01;
}

bool Modem::interrupting(uint64_t now) {
    poll(now);
    return (mcr_ & 8) && pending() != 0x01;
}

uint8_t Modem::read(int reg, uint64_t now) {
    bool dlab = lcr_ & 0x80;
    switch (reg & 7) {
    case 0:
        if (dlab) return dll_;
        poll(now);
        if (fifo_.empty()) return 0;
        {
            uint8_t b = fifo_.front();
            fifo_.pop_front();
            return b;
        }
    case 1: return dlab ? dlm_ : ier_;
    case 2: {
        poll(now);
        uint8_t v = pending();
        if (v == 0x02) thre_ = false;             // reading IIR clears a transmit interrupt it reports
        return (fcr_ & 1 ? 0xC0 : 0) | v;
    }
    case 3: return lcr_;
    case 4: return mcr_;
    case 5: {
        poll(now);
        uint8_t v = 0x60 | (fifo_.empty() ? 0 : 1) | overrun_;
        overrun_ = 0;
        return v;
    }
    case 6: {
        poll(now);
        uint8_t v = msr_;
        msr_ &= 0xF0;
        return v;
    }
    default: return scr_;
    }
}

void Modem::write(int reg, uint8_t v, uint64_t now) {
    idle_until(now);
    bool dlab = lcr_ & 0x80;
    switch (reg & 7) {
    case 0:
        if (dlab) dll_ = v;
        else {
            from_host(v, now);
            thre_ = true;                         // sent at once, so the register is empty again
        }
        break;
    case 1:
        if (dlab) dlm_ = v;
        else {
            if ((v & 2) && !(ier_ & 2)) thre_ = true;   // enabling it with the register empty interrupts
            ier_ = v & 0x0F;
        }
        break;
    case 2:
        fcr_ = v;
        if (v & 2) fifo_.clear();
        break;
    case 3: lcr_ = v; break;
    case 4: mcr_ = v; break;
    default: scr_ = v; break;
    }
}
