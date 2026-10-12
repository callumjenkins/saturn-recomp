// saturn-recomp runtime — a NetLink modem: the 16550 UART the software sees at 0x25895001 (a register
// every 4 bytes), and the Hayes command set of the modem behind it.
#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

// What the modem's phone line reaches. A line carries one call at a time.
struct ModemLine {
    virtual ~ModemLine() = default;
    virtual bool dial(const std::string& number) = 0;   // true once the far end is ringing
    virtual bool answered() = 0;                        // the far end has picked up the call dialled
    virtual bool ringing() = 0;                         // a call is waiting to be answered
    virtual bool answer() = 0;                          // true once the waiting call is connected
    virtual void hang_up() = 0;
    virtual bool connected() = 0;                       // false once the other end has gone
    virtual void send(uint8_t b) = 0;
    virtual bool receive(uint8_t& b) = 0;
    virtual bool leased() { return false; }             // the modem goes online whenever the line can, unasked
};

// A line with nothing on it: a dial gets no carrier and no call comes in.
std::unique_ptr<ModemLine> modem_no_line();

// `spec` is "dial=HOST:PORT", "listen=PORT", or both with a comma: every number dialled calls HOST:PORT,
// and a connection to PORT rings. Empty on a spec it cannot use, with the reason in `error`.
// With ",up" the line is a leased line: the modem is online from power-on, dialling at once or answering
// the first connection to come.
std::unique_ptr<ModemLine> modem_tcp_line(const std::string& spec, std::string& error);

class Modem {
public:
    explicit Modem(std::unique_ptr<ModemLine> line);
    uint8_t read(int reg, uint64_t now);          // reg 0-7; now in ns of machine time
    void write(int reg, uint8_t v, uint64_t now);
    bool online() const { return online_; }
    bool interrupting(uint64_t now);              // the UART's interrupt output: OUT2 set and an interrupt pending
    void bring_up(uint64_t now);                  // a leased line: dial or answer now, and go online without a word
    void dialled(bool started, uint64_t now);     // after ATD: wait for an answer, or no carrier at once

private:
    void poll(uint64_t now);
    void idle_until(uint64_t now);
    void from_host(uint8_t b, uint64_t now);      // a byte the software sent through the UART
    void command(const std::string& line, uint64_t now);
    void result(const char* text);
    void connect(uint64_t now);
    void drop();
    uint64_t byte_ns() const;
    uint8_t pending() const;                      // IIR's interrupt bits for the highest one pending, 0x01 for none

    std::unique_ptr<ModemLine> line_;
    uint8_t ier_ = 0, lcr_ = 0, mcr_ = 0, scr_ = 0, fcr_ = 0, dll_ = 12, dlm_ = 0;
    uint8_t msr_ = 0x30, overrun_ = 0;            // CTS and DSR: the modem is always ready
    bool thre_ = false;                           // the transmit register has emptied since IIR last said so
    std::deque<uint8_t> fifo_;                    // the UART's receive FIFO, which the software reads
    std::deque<uint8_t> to_host_;                 // what the modem has to say, or the line's data
    uint64_t next_rx_ = 0;                        // when the next byte may reach the FIFO

    uint64_t next_try_ = 0;                       // a leased line's next try at its call
    uint64_t calling_until_ = 0;                  // a call dialled waits for its answer until then; 0: none
    int wait_for_carrier_ = 50;                   // S7: seconds a dialled call waits to be answered
    bool online_ = false;                         // a call is up
    bool data_ = false;                           // ... and bytes go down it, rather than to the command parser
    bool echo_ = true, verbose_ = true, quiet_ = false;
    int auto_answer_ = 0, rings_ = 0;
    uint64_t next_ring_ = 0;
    std::string cmd_, last_cmd_;
    int pluses_ = 0;
    uint64_t last_tx_ = 0, escape_at_ = 0;
};

// The modem the runtime's address map uses, or none (the default: no NetLink is fitted).
extern std::unique_ptr<Modem> g_modem;
