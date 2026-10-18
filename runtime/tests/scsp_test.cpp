// The SCSP's DSP: its reads and writes of sound RAM, and what it keeps from one step to the next.
#include "saturn.h"
#include "sound.h"
#include "state.h"
#include "test.h"

namespace {

struct Step {
    uint32_t tra, twt, twa, xsel, ysel, ira, iwt, iwa, table, mwt, mrd, ewt, ewa, adrl, frcl, shift, yrl, negb,
        zero, bsel, nofl, coef, masa, adreb, nxadr;
};

void put(int st, const Step& s) {
    const uint32_t w[4] = {
        s.tra << 8 | s.twt << 7 | s.twa,
        s.xsel << 15 | s.ysel << 13 | s.ira << 6 | s.iwt << 5 | s.iwa,
        s.table << 15 | s.mwt << 14 | s.mrd << 13 | s.ewt << 12 | s.ewa << 8 | s.adrl << 7 | s.frcl << 6 | s.shift << 4 |
            s.yrl << 3 | s.negb << 2 | s.zero << 1 | s.bsel,
        s.coef << 9 | s.nofl << 8 | s.masa << 2 | s.adreb << 1 | s.nxadr};
    for (int k = 0; k < 4; ++k) scsp_write(0x800 + st * 8 + 2 * k, w[k], 2);
}

void temp(int i, uint32_t v) { scsp_write(0xC00 + 4 * i, v, 4); }
void mems(int i, uint32_t v) { scsp_write(0xE00 + 4 * i, v, 4); }
uint32_t mems(int i) { return scsp_read(0xE00 + 4 * i, 4); }
uint32_t efreg(int i) { return scsp_read(0xEC0 + 2 * i, 2); }
uint32_t ram_word(uint32_t word) { return (uint32_t)g_sound_ram[2 * word] << 8 | g_sound_ram[2 * word + 1]; }
void set_ram_word(uint32_t word, uint16_t v) { g_sound_ram[2 * word] = (uint8_t)(v >> 8), g_sound_ram[2 * word + 1] = (uint8_t)v; }

void sample() {
    int16_t out[2];
    scsp_sample(0, 0, out);
}

// A step of 0s sets the accumulator to TEMP 0 (FRC is 0), so a write after one stores 0x1234.
void start(uint32_t temp0 = 0x123400) {
    scsp_reset();
    for (uint32_t w : {0x0u, 0xFFu, 0x100u, 0x101u, 0x200u, 0x10FFu, 0x1100u, 0x1FFFu}) set_ram_word(w, 0);
    temp(0, temp0);
    scsp_write(0x780, 0x100, 2);                // MADRS 0
}

void samples(int n) {
    while (n--) sample();
}

// Slot 0 keyed on, looping, with an instant attack, keyed off a sample later, then `n` samples on: its
// envelope as the monitor reads it, 0 loudest
uint32_t released(int rr, int krs, int oct, int n) {
    scsp_reset();
    scsp_write(0x06, 0xFFFF, 2);                // LEA
    scsp_write(0x08, 0x001F, 2);                // AR 31
    scsp_write(0x0A, (uint32_t)(krs << 10 | rr), 2);
    scsp_write(0x10, (uint32_t)((oct & 15) << 11), 2);
    scsp_write(0x00, 0x1820, 2);                // KYONEX, KB, a loop
    sample();
    scsp_write(0x00, 0x1020, 2);
    samples(n);
    scsp_write(0x408, 0, 2);                    // the monitor on slot 0
    return scsp_read(0x408, 2) & 0x1F;
}

}  // namespace

TEST(a_write_asked_for_on_an_even_step_reaches_sound_ram) {
    start();
    put(2, {.table = 1, .mwt = 1, .nofl = 1});
    sample();
    CHECK_EQ(ram_word(0x100), 0x1234);
}

TEST(a_word_read_reaches_mems_through_the_iwt_two_steps_on_not_the_next) {
    start();
    set_ram_word(0x100, 0x5678);
    put(0, {.table = 1, .mrd = 1, .nofl = 1});
    put(1, {.iwt = 1, .iwa = 1});
    put(2, {.iwt = 1, .iwa = 2});
    sample();
    CHECK_EQ(mems(1), 0);
    CHECK_EQ(mems(2), 0x567800);
}

TEST(a_nofl_read_of_a_negative_word_is_sign_extended) {
    start();
    set_ram_word(0x100, 0x8000);
    put(0, {.table = 1, .mrd = 1, .nofl = 1});
    put(2, {.iwt = 1, .iwa = 1});
    sample();
    CHECK_EQ(mems(1), 0xFF800000u);
}

TEST(a_write_asked_for_by_the_programs_last_instruction_lands_in_the_same_sample) {
    start();
    put(1, {.table = 1, .mwt = 1, .nofl = 1});
    sample();
    CHECK_EQ(ram_word(0x100), 0x1234);
}

TEST(adrs_moves_an_address_back_when_its_12_bits_are_negative) {
    start();
    mems(0, 0xFF0000);                          // -65536, so ADRL takes 0xFFF
    put(0, {.adrl = 1});
    put(1, {.table = 1, .mwt = 1, .nofl = 1, .adreb = 1});
    sample();
    CHECK_EQ(ram_word(0xFF), 0x1234);
    CHECK_EQ(ram_word(0x10FF), 0);
}

TEST(two_writes_to_one_efreg_in_a_sample_leave_the_second) {
    start();
    temp(1, 0x000100);
    put(1, {.tra = 1, .ewt = 1});               // writes 0x1234 and takes TEMP 1 into the accumulator
    put(2, {.ewt = 1});
    sample();
    CHECK_EQ(efreg(0), 0x0001);
}

TEST(inputs_read_mems_before_the_same_steps_iwt_writes_it) {
    start();
    set_ram_word(0x100, 0x7F00);
    scsp_write(0x700, 0x7FF8, 2);               // COEF 0: 4095 of 4096
    put(0, {.table = 1, .mrd = 1, .nofl = 1});
    put(2, {.xsel = 1, .ysel = 1, .iwt = 1, .iwa = 0, .zero = 1});
    put(3, {.ewt = 1});
    sample();
    CHECK_EQ(efreg(0), 0);
    CHECK_EQ(mems(0), 0x7F0000);
}

TEST(an_ira_past_the_external_inputs_keeps_the_last_input) {
    start();
    mems(0, 0x123400);
    scsp_write(0x700, 0x7FF8, 2);
    put(1, {.xsel = 1, .ysel = 1, .ira = 0x32, .zero = 1});
    put(2, {.ewt = 1});
    sample();
    CHECK_EQ(efreg(0), 0x1232);                 // 0x123400 * 4095 >> 12 >> 8
}

TEST(a_dump_keeps_an_access_the_last_step_asked_for) {
    start();
    put(127, {.table = 1, .mwt = 1, .nofl = 1});
    sample();
    CHECK_EQ(ram_word(0x100), 0);               // done at the next sample's first step
    State chip, dsp;
    scsp_state(chip);
    scspdsp_state(dsp);
    scsp_reset();
    State chip_in, dsp_in;
    chip_in.loading = dsp_in.loading = true;
    chip_in.data = chip.data;
    dsp_in.data = dsp.data;
    scsp_state(chip_in);
    scspdsp_state(dsp_in);
    sample();
    CHECK_EQ(ram_word(0x100), 0x1234);
}

TEST(the_words_after_madrs_leave_it_alone_and_read_0) {
    scsp_reset();
    scsp_write(0x780, 0x4772, 2);
    scsp_write(0x7C0, 0, 2);
    CHECK_EQ(scsp_read(0x780, 2), 0x4772);
    scsp_write(0x7FE, 0x1234, 2);
    CHECK_EQ(scsp_read(0x7FE, 2), 0);
}

TEST(a_step_that_reads_and_writes_writes_where_the_next_step_points) {
    start();
    scsp_write(0x782, 0x200, 2);                // MADRS 1
    put(1, {.table = 1, .mwt = 1, .mrd = 1, .nofl = 1});
    put(2, {.table = 1, .masa = 1});            // the read is done here, the write a step on
    sample();
    CHECK_EQ(ram_word(0x200), 0x1234);
    CHECK_EQ(ram_word(0x100), 0);
}

TEST(rbp_bit_6_puts_the_ring_past_the_end_of_sound_ram_where_reads_are_0) {
    start();
    set_ram_word(0x100, 0x5678);
    scsp_write(0x402, 0x40, 2);
    put(0, {.table = 1, .mrd = 1, .nofl = 1});
    put(2, {.iwt = 1, .iwa = 1});
    sample();
    CHECK_EQ(mems(1), 0);
}

TEST(a_write_past_the_end_of_sound_ram_is_lost) {
    start();
    scsp_write(0x402, 0x40, 2);
    put(2, {.table = 1, .mwt = 1, .nofl = 1});
    sample();
    CHECK_EQ(ram_word(0x100), 0);
}

TEST(a_float_read_unpacks_into_mems) {
    start();
    set_ram_word(0x100, 0x9234);
    put(0, {.table = 1, .mrd = 1});
    put(2, {.iwt = 1, .iwa = 1});
    sample();
    CHECK_EQ(mems(1), 0xFFE46800u);
}

TEST(a_float_write_packs) {
    start();
    put(2, {.table = 1, .mwt = 1});
    sample();
    CHECK_EQ(ram_word(0x100), 0x111A);
}

TEST(the_ring_address_moves_back_a_word_each_sample_and_wraps_at_rbl) {
    start();
    scsp_write(0x780, 0, 2);
    temp(0x7F, 0x567800);                       // TEMP 0 as the second sample reads it
    put(2, {.mwt = 1, .nofl = 1});
    samples(2);
    CHECK_EQ(ram_word(0), 0x1234);
    CHECK_EQ(ram_word(0x1FFF), 0x5678);
}

TEST(rbp_moves_the_ring_by_4096_words) {
    start();
    scsp_write(0x402, 1, 2);
    put(2, {.table = 1, .mwt = 1, .nofl = 1});
    sample();
    CHECK_EQ(ram_word(0x1100), 0x1234);
}

TEST(nxadr_adds_one_to_the_address) {
    start();
    put(2, {.table = 1, .mwt = 1, .nofl = 1, .nxadr = 1});
    sample();
    CHECK_EQ(ram_word(0x101), 0x1234);
}

TEST(a_table_address_wraps_at_16_bits) {
    start();
    set_ram_word(0, 0);
    scsp_write(0x780, 0xFFFF, 2);
    put(2, {.table = 1, .mwt = 1, .nofl = 1, .nxadr = 1});
    sample();
    CHECK_EQ(ram_word(0), 0x1234);
}

TEST(the_accumulator_wraps_at_26_bits_before_the_shifter_clamps) {
    start(0x7FFFFF);
    scsp_write(0x700, 0x7FF8, 2);
    for (int st = 1; st <= 5; ++st) put(st, {.ysel = 1, .bsel = 1});   // adds TEMP 0 x 4095/4096 each
    put(6, {.ewt = 1});
    sample();
    CHECK_EQ(efreg(0), 0x8000);
}

TEST(the_accumulator_lasts_into_the_next_sample) {
    start();
    put(0, {.ewt = 1});
    sample();
    CHECK_EQ(efreg(0), 0);
    sample();
    CHECK_EQ(efreg(0), 0x1234);
}

TEST(frc_lasts_into_the_next_sample) {
    start();
    temp(0x7F, 0x123400);
    put(0, {.zero = 1});                        // TEMP 0 x FRC
    put(1, {.ewt = 1});
    put(2, {.frcl = 1});                        // FRC = 0x123400 >> 11, 0x246
    samples(2);
    CHECK_EQ(efreg(0), 0x296);                  // 0x123400 x 0x246 >> 12 >> 8
}

TEST(adrs_lasts_into_the_next_sample) {
    start();
    mems(0, 0x010000);
    put(0, {.table = 1, .mwt = 1, .nofl = 1, .adreb = 1});
    put(2, {.adrl = 1});
    samples(2);
    CHECK_EQ(ram_word(0x101), 0x1234);
}

TEST(inputs_last_into_the_next_sample) {
    start();
    mems(0, 0x123400);
    scsp_write(0x700, 0x7FF8, 2);
    put(0, {.xsel = 1, .ysel = 1, .ira = 0x32, .zero = 1});
    put(1, {.ewt = 1});
    sample();
    CHECK_EQ(efreg(0), 0);
    sample();
    CHECK_EQ(efreg(0), 0x1232);
}

TEST(the_y_register_lasts_into_the_next_sample) {
    start();
    temp(0x7F, 0x123400);
    mems(0, 0x123400);
    put(0, {.ysel = 3, .zero = 1});             // TEMP 0 x Y's bits 4-15, 0x340
    put(1, {.ewt = 1, .yrl = 1});
    sample();
    CHECK_EQ(efreg(0), 0);
    sample();
    CHECK_EQ(efreg(0), 0x3B2);                  // 0x123400 x 0x340 >> 12 >> 8
}

TEST(efreg_keeps_its_value_through_a_sample_that_does_not_write_it) {
    start();
    put(1, {.ewt = 1});
    sample();
    put(1, {});
    put(2, {.tra = 0x10});
    sample();
    CHECK_EQ(efreg(0), 0x1234);
}

TEST(madrs_31_is_the_word_at_0x7be) {
    scsp_reset();
    scsp_write(0x7BE, 0x4321, 2);
    CHECK_EQ(scsp_read(0x7BE, 2), 0x4321);
}

TEST(the_ring_moves_while_the_program_is_empty) {
    start();
    scsp_write(0x780, 0, 2);
    temp(0x7F, 0x123400);
    sample();
    put(2, {.mwt = 1, .nofl = 1});
    sample();
    CHECK_EQ(ram_word(0x1FFF), 0x1234);
}

TEST(reset_drops_an_access_the_last_step_asked_for) {
    start();
    put(127, {.table = 1, .mwt = 1, .nofl = 1});
    sample();
    start();
    put(0, {.tra = 0x10});
    sample();
    CHECK_EQ(ram_word(0x100), 0);
}

// A full release, 1023 steps, takes 920 ms at rate 15 and 3.6 ms at 31: after 4410 samples at 15 the
// envelope is 111 steps down, which the monitor shows as 3, and 170 samples at 31 end it (31).
TEST(a_negative_octave_slows_no_rate) {
    CHECK_EQ(released(15, 0, -3, 4410), 3);
}

TEST(key_rate_scaling_adds_krs_and_the_octave_to_the_rate) {
    CHECK_EQ(released(12, 2, 1, 4410), 3);
}

TEST(krs_15_scales_nothing) {
    CHECK_EQ(released(15, 15, 7, 4410), 3);
}

TEST(a_scaled_rate_stops_at_31) {
    CHECK_EQ(released(20, 14, 7, 170), 31);
}
