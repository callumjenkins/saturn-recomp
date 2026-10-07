// saturn-recomp runtime — --agent FD: another program plays the run through a
// socket it passed down, one text command a line.
//
// The run pauses at VBlank-IN 1, and again wherever a step ends, and says so
// with "vblank N". While paused it takes commands, each answered with one line:
//   step N            run N VBlanks; the answer is the next "vblank"
//   pad [P.]BUTTONS   pad P (1 if not given) holds BUTTONS from this VBlank
//                     on, as --input's "N:P.BUTTONS" would; no BUTTONS lets go
//   read ADDR LEN     LEN bytes from ADDR (hex), as the CPU reads them: "data HEX"
//   write ADDR HEX    those bytes to ADDR, as the CPU writes them: "ok"
//   frame             the field that has just ended: "frame W H", then W*H*3
//                     bytes of RGB, top row first
//   quit              end the run
// A command that does not parse answers "error WHAT". A closed socket ends the
// run too. A pause takes no time on the Saturn's clock, so the same presses at
// the same VBlanks play the same as an --input script.
#include "saturn.h"
#include "video.h"
#include <sstream>
#include <string>
#if !defined(_WIN32)
#include <sys/socket.h>
#include <unistd.h>
#endif
#if !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0                      // macOS: a write to a closed socket raises SIGPIPE instead
#endif

static uint64_t g_until = 1;              // the VBlank the run pauses at next
static std::string g_in;

#if defined(_WIN32)
static bool read_line(std::string&) { sat_fatal("--agent: not available on Windows"); }
static void send(const void*, size_t) { sat_fatal("--agent: not available on Windows"); }
#else
static bool read_line(std::string& line) {
    for (;;) {
        size_t nl = g_in.find('\n');
        if (nl != std::string::npos) {
            line = g_in.substr(0, nl);
            g_in.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        char buf[4096];
        ssize_t n = ::read(g_cfg.agent_fd, buf, sizeof buf);
        if (n <= 0) return false;
        g_in.append(buf, (size_t)n);
    }
}

static void send(const void* data, size_t n) {
    const char* p = static_cast<const char*>(data);
    while (n) {
        ssize_t k = ::send(g_cfg.agent_fd, p, n, MSG_NOSIGNAL);
        if (k <= 0) sat_stop("the agent closed its socket");
        p += k;
        n -= (size_t)k;
    }
}
#endif

static void say(const std::string& line) { send((line + "\n").data(), line.size() + 1); }

static void read_memory(std::istringstream& args) {
    uint32_t addr = 0, len = 0;
    if (!(args >> std::hex >> addr >> std::dec >> len) || len > 0x100000) { say("error read ADDR LEN, LEN up to 1M"); return; }
    static const char kHex[] = "0123456789ABCDEF";
    std::string out = "data ";
    for (uint32_t i = 0; i < len; ++i) {
        uint32_t b = ld8(addr + i);
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    say(out);
}

static void write_memory(std::istringstream& args) {
    uint32_t addr = 0;
    std::string hex;
    if (!(args >> std::hex >> addr >> hex) || hex.size() % 2) { say("error write ADDR HEX"); return; }
    for (size_t i = 0; i < hex.size(); i += 2) st8(addr + (uint32_t)(i / 2), (uint32_t)std::stoul(hex.substr(i, 2), nullptr, 16));
    say("ok");
}

static void send_frame() {
    static Frame f;
    vdp2_compose(f);
    say("frame " + std::to_string(f.w) + " " + std::to_string(f.h));
    std::string rgb;
    rgb.reserve(f.px.size() * 3);
    for (uint32_t p : f.px) { rgb += (char)(p >> 16); rgb += (char)(p >> 8); rgb += (char)p; }
    send(rgb.data(), rgb.size());
}

void agent_poll() {
    if (g_cfg.agent_fd < 0 || sat_vblanks() < g_until) return;
    say("vblank " + std::to_string(sat_vblanks()));
    std::string line;
    while (read_line(line)) {
        std::istringstream args(line);
        std::string cmd;
        args >> cmd;
        if (cmd == "step") {
            uint64_t n = 0;
            if (!(args >> n) || !n) { say("error step N, N at least 1"); continue; }
            g_until = sat_vblanks() + n;
            return;
        }
        if (cmd == "pad") {
            std::string spec, error;
            args >> spec;
            if (smpc_press(spec, error)) say("ok");
            else say("error " + error);
        } else if (cmd == "read") read_memory(args);
        else if (cmd == "write") write_memory(args);
        else if (cmd == "frame") send_frame();
        else if (cmd == "quit") break;
        else say("error no command " + cmd);
    }
    sat_stop("the agent ended it");
}
