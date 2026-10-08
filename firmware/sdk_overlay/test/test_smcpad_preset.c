/* Host unit tests for board/smcpad_preset.c.
 *   gcc -Wall -Wextra -Werror -std=c11 -fsanitize=address,undefined -g -I board -I sinco \
 *       test/test_smcpad_preset.c -o /tmp/t && /tmp/t [SPB.bin]
 * With the Midi Suite factory file SPB.bin as argument, smcpad_preset_factory() is also
 * compared byte for byte with its slot 1.
 */
#define SMCPAD_HOST_TEST 1
#include <stdio.h>
#include <string.h>
#include "../board/smcpad_scale.c"
#include "../board/smcpad_preset.c"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* ---- MIDI capture ------------------------------------------------------------------ */
static u8  out[4096];
static u32 out_len;
static int gchanged;

static void cap_midi(void *ctx, const u8 *m, u32 n)
{
    (void)ctx;
    if (out_len + n <= sizeof(out)) {
        memcpy(out + out_len, m, n);
        out_len += n;
    }
}
static void cap_globals(void *ctx) { (void)ctx; gchanged++; }
static u32 mcp_bytes;                                     /* bytes that went through midi_mcp */
static void cap_mcp(void *ctx, const u8 *m, u32 n)
{
    mcp_bytes += n;
    cap_midi(ctx, m, n);
}
static int schanged;
static void cap_scale(void *ctx) { (void)ctx; schanged++; }
static const struct smcpad_preset_io io = {NULL, cap_midi, cap_mcp, cap_globals, cap_scale};

static int out_is(const u8 *exp, u32 n)
{
    int ok = out_len == n && !memcmp(out, exp, n);
    if (!ok) {
        printf("  got %u B:", (unsigned)out_len);
        for (u32 i = 0; i < out_len; i++) printf(" %02x", out[i]);
        printf("\n  exp %u B:", (unsigned)n);
        for (u32 i = 0; i < n; i++) printf(" %02x", exp[i]);
        printf("\n");
    }
    out_len = 0;
    return ok;
}
#define OUT(...) do { static const u8 e_[] = {__VA_ARGS__}; CHECK(out_is(e_, sizeof(e_))); } while (0)
#define NO_OUT() do { CHECK(out_len == 0); out_len = 0; } while (0)

static u8 presets[SMCPAD_PRESETS_SIZE];
static u8 globals[SMCPAD_GLOBALS_SIZE];
static struct smcpad_preset P;

static void setup(void)
{
    for (int i = 0; i < SMCPAD_PRESET_COUNT; i++) {
        smcpad_preset_factory(presets + i * SMCPAD_PRESET_SIZE);
    }
    static const u8 gdef[12] = {120, 0, 50, 4, 0, 0, 2, 0, 1, 1, 0, 0};   /* stock defaults */
    memcpy(globals, gdef, sizeof(globals));
    smcpad_preset_init(&P, &io, presets, globals);
    out_len = 0;
    gchanged = 0;
}

static u8 *pad_rec(int preset, int rec)
{
    return presets + preset * SMCPAD_PRESET_SIZE + SMCPAD_PRESET_OFF_PAD + rec * SMCPAD_PRESET_PAD_LEN;
}
static u8 *btn_rec(int preset, int b)
{
    return presets + preset * SMCPAD_PRESET_SIZE + SMCPAD_PRESET_OFF_BTN + b * SMCPAD_PRESET_BTN_LEN;
}
static u8 *knob_rec(int preset, int k)
{
    return presets + preset * SMCPAD_PRESET_SIZE + SMCPAD_PRESET_OFF_KNOB + k * SMCPAD_PRESET_KNOB_LEN;
}

/* ---- tests ------------------------------------------------------------------------- */
static void test_factory(const char *spb_path)
{
    static u8 f[SMCPAD_PRESET_SIZE];
    smcpad_preset_factory(f);
    /* records copied from SPB.bin slot 1 (Midi Suite assets/bin) */
    static const u8 btn0[23] = {0x02, 0x00, 0x19, 0x00, 0x7f, 0x06, 0xf0, 0x7f, 0x7f, 0x06, 0x01, 0xf7,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff};
    static const u8 knob15[6] = {0x00, 0x02, 0x00, 0x2d, 0x00, 0x7f};
    static const u8 pad0[9] = {0x00, 0x09, 0x04, 0x00, 0x7f, 0x96, 0xc8, 0xf0, 0xff};
    static const u8 pad47[9] = {0x00, 0x09, 0x33, 0x00, 0x7f, 0xf0, 0x00, 0xf0, 0xff};
    static const u8 pad111[9] = {0x00, 0x09, 0x73, 0x00, 0x7f, 0x00, 0xf0, 0xf0, 0xff};
    static const u8 pad127[9] = {0x00, 0x09, 0x43, 0x00, 0x7f, 0x78, 0x78, 0xf0, 0xff};
    CHECK(!memcmp(f, btn0, 23));
    CHECK(!memcmp(f + SMCPAD_PRESET_OFF_KNOB + 15 * 6, knob15, 6));
    CHECK(!memcmp(f + SMCPAD_PRESET_OFF_PAD + 0 * 26, pad0, 9));
    CHECK(!memcmp(f + SMCPAD_PRESET_OFF_PAD + 47 * 26, pad47, 9));
    CHECK(!memcmp(f + SMCPAD_PRESET_OFF_PAD + 111 * 26, pad111, 9));
    CHECK(!memcmp(f + SMCPAD_PRESET_OFF_PAD + 127 * 26, pad127, 9));
    CHECK(SMCPAD_PRESET_OFF_PAD + 128 * 26 == SMCPAD_PRESET_SIZE);

    if (spb_path) {
        static u8 spb[SMCPAD_PRESETS_SIZE + 1];
        FILE *fp = fopen(spb_path, "rb");
        CHECK(fp != NULL);
        if (fp) {
            size_t n = fread(spb, 1, sizeof(spb), fp);
            fclose(fp);
            CHECK(n == SMCPAD_PRESETS_SIZE);
            CHECK(!memcmp(spb, f, SMCPAD_PRESET_SIZE));
            printf("  factory preset == SPB.bin slot 1\n");
        }
    }
}

static void test_helpers(void)
{
    /* value helper 0x1E0FE62 */
    CHECK(smcpad_preset_scale(0x7f, 0, 127) == 127);
    CHECK(smcpad_preset_scale(0, 0, 127) == 0);
    CHECK(smcpad_preset_scale(0x7f, 100, 20) == 20);
    CHECK(smcpad_preset_scale(0, 100, 20) == 100);
    CHECK(smcpad_preset_scale(63, 0, 127) == 63);
    CHECK(smcpad_preset_scale(0x7f, 5, 5) == 5);
    /* curves: 0 linear, 1 soft, 2 hard, 3 fixed */
    CHECK(smcpad_preset_curve(100, 0) == 100);
    CHECK(smcpad_preset_curve(20, 1) == 35 && smcpad_preset_curve(64, 1) == 77);
    CHECK(smcpad_preset_curve(20, 2) == 12 && smcpad_preset_curve(64, 2) == 48);
    CHECK(smcpad_preset_curve(1, 3) == 127);
    CHECK(smcpad_preset_curve(0, 0) == 1);
    for (int c = 0; c < 3; c++) {
        CHECK(smcpad_preset_curve(1, (u8)c) >= 1 && smcpad_preset_curve(127, (u8)c) == 127);
        for (int v = 2; v <= 127; v++) {
            CHECK(smcpad_preset_curve((u8)v, (u8)c) >= smcpad_preset_curve((u8)(v - 1), (u8)c));
        }
    }
    for (int v = 1; v <= 127; v++) {        /* soft above linear above hard */
        CHECK(smcpad_preset_curve((u8)v, 1) >= v && smcpad_preset_curve((u8)v, 2) <= v);
    }
    /* speed table */
    CHECK(smcpad_preset_speed(1, 4) == 1);
    CHECK(smcpad_preset_speed(3, 2) == 6);
    CHECK(smcpad_preset_speed(-3, 2) == -6);
    CHECK(smcpad_preset_speed(9, 4) == 45);
    CHECK(smcpad_preset_speed(9, 0) == 9);
    CHECK(smcpad_preset_speed(2, 99) == 4);
    CHECK(smcpad_preset_speed(0, 3) == 0);
    /* record index */
    u8 g[12] = {0};
    g[GLOB_OCTAVE] = 2;
    CHECK(smcpad_preset_pad_record(g, 0) == 32);
    CHECK(smcpad_preset_pad_record(g, 15) == 47);
    g[GLOB_TRANSPOSE] = (u8)(s8)-5;
    CHECK(smcpad_preset_pad_record(g, 0) == 27);
    g[GLOB_OCTAVE] = 0;
    CHECK(smcpad_preset_pad_record(g, 0) == 0xFF);        /* below 0: refused */
    g[GLOB_OCTAVE] = 7; g[GLOB_TRANSPOSE] = 16;
    CHECK(smcpad_preset_pad_record(g, 15) == 0xFF);       /* above 127: refused */
    g[GLOB_BANK] = 1;
    CHECK(smcpad_preset_pad_record(g, 3) == 115);
    CHECK(smcpad_preset_pad_record(g, 16) == 0xFF);
}

static void test_pads_note_at(void)
{
    setup();
    /* factory: pad 0 -> record 32 = note 36 ch10, curve 1 (soft): v7 100 -> (112 + 100 + 1) / 2 */
    smcpad_preset_pad(&P, 0, 1, 100);
    OUT(0x99, 36, 106);
    smcpad_preset_pressure(&P, 0, 64);
    OUT(0xD9, 77);                                         /* soft: (90 + 64 + 1) / 2 */
    smcpad_preset_pressure(&P, 0, 64);
    NO_OUT();                                              /* unchanged */
    smcpad_preset_pad(&P, 5, 1, 127);
    OUT(0x99, 41, 127);
    smcpad_preset_pad(&P, 0, 0, 0);
    OUT(0x89, 36, 0x40);                                   /* pad 5 still held: no AT 0 */
    smcpad_preset_pad(&P, 5, 0, 0);
    OUT(0xD9, 0, 0x89, 41, 0x40);                          /* AT 0 before the last Note Off */
    smcpad_preset_pad(&P, 5, 0, 0);
    NO_OUT();                                              /* double release */

    /* aftertouch off in globals */
    globals[GLOB_AFTERTOUCH] = 0;
    smcpad_preset_pad(&P, 1, 1, 10);
    out_len = 0;
    smcpad_preset_pressure(&P, 1, 100);
    NO_OUT();
    smcpad_preset_pad(&P, 1, 0, 0);
    OUT(0x89, 37, 0x40);

    /* velocity range [100, 110], curve 0 */
    globals[GLOB_PAD_CURVE] = 0;
    pad_rec(0, 33)[3] = 100; pad_rec(0, 33)[4] = 110;
    smcpad_preset_pad(&P, 1, 1, 127);
    OUT(0x99, 37, 110);
    smcpad_preset_pad(&P, 1, 0, 0);
    out_len = 0;

    /* release uses the record captured at press, even if octave changes in between */
    smcpad_preset_pad(&P, 2, 1, 50);
    OUT(0x99, 38, 50);
    globals[GLOB_OCTAVE] = 3;
    smcpad_preset_pad(&P, 2, 0, 0);
    OUT(0x89, 38, 0x40);
    smcpad_preset_pad(&P, 2, 1, 50);
    OUT(0x99, 54, 50);                                     /* record 50 = note 54 */
    smcpad_preset_pad(&P, 2, 0, 0);
    out_len = 0;

    /* bank B */
    globals[GLOB_BANK] = 1;
    smcpad_preset_pad(&P, 0, 1, 50);
    OUT(0x99, 52, 50);
    smcpad_preset_pad(&P, 0, 0, 0);
    out_len = 0;
}

static void test_pads_other_types(void)
{
    setup();
    u8 *r = pad_rec(0, 32);                                /* pad 0 */
    /* CC toggle: first press = byte 4, then byte 3, no release message */
    r[0] = PAD_T_CC_TOGGLE; r[1] = 0; r[2] = 4; r[3] = 0; r[4] = 127;
    smcpad_preset_pad(&P, 0, 1, 90);  OUT(0xB0, 4, 127);
    smcpad_preset_pad(&P, 0, 0, 0);   NO_OUT();
    smcpad_preset_pad(&P, 0, 1, 90);  OUT(0xB0, 4, 0);
    smcpad_preset_pad(&P, 0, 0, 0);   NO_OUT();
    /* momentary: press = byte 4 ("Down"), release = byte 3 ("Up") */
    r[0] = PAD_T_CC_MOMENTARY; r[1] = 2; r[2] = 46; r[3] = 10; r[4] = 99;
    smcpad_preset_pad(&P, 0, 1, 90);  OUT(0xB2, 46, 99);
    smcpad_preset_pad(&P, 0, 0, 0);   OUT(0xB2, 46, 10);
    /* program: byte 2 = LSB (CC32), byte 3 = MSB (CC0), byte 4 = program */
    r[0] = PAD_T_PROGRAM; r[1] = 3; r[2] = 5; r[3] = 6; r[4] = 7;
    smcpad_preset_pad(&P, 0, 1, 90);  OUT(0xB3, 0x00, 6, 0xB3, 0x20, 5, 0xC3, 7);
    smcpad_preset_pad(&P, 0, 0, 0);   NO_OUT();
    /* MCP: ch1 whatever byte 1 says, 7F / 00, through the MCP output */
    r[0] = PAD_T_MCP; r[1] = 9; r[2] = 0x18;
    mcp_bytes = 0;
    smcpad_preset_pad(&P, 0, 1, 10);  OUT(0x90, 0x18, 0x7f);
    smcpad_preset_pad(&P, 0, 0, 0);   OUT(0x90, 0x18, 0x00);
    CHECK(mcp_bytes == 6);
    /* custom SysEx: sent raw when well formed */
    r[0] = PAD_T_SYSEX; r[9] = 6;
    static const u8 sx[6] = {0xF0, 0x7F, 0x7F, 0x06, 0x02, 0xF7};
    memcpy(r + 10, sx, 6);
    smcpad_preset_pad(&P, 0, 1, 10);  OUT(0xF0, 0x7F, 0x7F, 0x06, 0x02, 0xF7);
    smcpad_preset_pad(&P, 0, 0, 0);   NO_OUT();
    r[14] = 0x82;                                          /* status byte inside: refused */
    smcpad_preset_pad(&P, 0, 1, 10);  NO_OUT();
    smcpad_preset_pad(&P, 0, 0, 0);
    r[14] = 0x02; r[9] = 17;                               /* too long: refused */
    smcpad_preset_pad(&P, 0, 1, 10);  NO_OUT();
    smcpad_preset_pad(&P, 0, 0, 0);
    /* Comb-MCP and unknown types: nothing (deferred) */
    r[0] = PAD_T_COMB_MCP;
    smcpad_preset_pad(&P, 0, 1, 10);  NO_OUT();
    smcpad_preset_pad(&P, 0, 0, 0);   NO_OUT();
    r[0] = 0x42;
    smcpad_preset_pad(&P, 0, 1, 10);  NO_OUT();
    smcpad_preset_pad(&P, 0, 0, 0);   NO_OUT();
    /* out-of-range pad and record */
    smcpad_preset_pad(&P, 16, 1, 10); NO_OUT();
    globals[GLOB_OCTAVE] = 0; globals[GLOB_TRANSPOSE] = (u8)(s8)-1;
    smcpad_preset_pad(&P, 0, 1, 10);  NO_OUT();
}

static void test_buttons(void)
{
    setup();
    /* factory: hw buttons 3..7 = CC Push 25..29, 127 on press / 0 on release; Shift ^ 64 = 89..93 */
    smcpad_preset_button(&P, 3, 1);   OUT(0xB0, 25, 127);
    smcpad_preset_button(&P, 3, 0);   OUT(0xB0, 25, 0);
    smcpad_preset_button(&P, 7, 1);   OUT(0xB0, 29, 127);
    smcpad_preset_button(&P, 7, 0);   OUT(0xB0, 29, 0);
    smcpad_preset_button(&P, SMCPAD_HWBTN_SHIFT, 1);
    smcpad_preset_button(&P, 7, 1);   OUT(0xB0, 93, 127);  /* Shift + Rec = CC 93 */
    smcpad_preset_button(&P, SMCPAD_HWBTN_SHIFT, 0);       /* releasing Shift first keeps CC 93 */
    smcpad_preset_button(&P, 7, 0);   OUT(0xB0, 93, 0);
    /* system buttons 0, 8, 9: no MIDI */
    smcpad_preset_button(&P, 0, 1);   NO_OUT();
    smcpad_preset_button(&P, 8, 1);   NO_OUT();
    smcpad_preset_button(&P, 8, 0);   NO_OUT();
    smcpad_preset_button(&P, 9, 0);   NO_OUT();
    smcpad_preset_button(&P, 12, 1);  NO_OUT();

    u8 *b = btn_rec(0, 1);                                 /* hw button 4 */
    b[0] = BTN_T_CC_SINGLE; b[1] = 1; b[2] = 20; b[3] = 33; b[4] = 44;
    smcpad_preset_button(&P, 4, 1);   OUT(0xB1, 20, 33);   /* Single = byte 3 */
    smcpad_preset_button(&P, 4, 0);   NO_OUT();
    b[0] = BTN_T_CC_TOGGLE;
    smcpad_preset_button(&P, 4, 1);   OUT(0xB1, 20, 44);
    CHECK(smcpad_preset_button_leds(&P, 0) & (1u << 4));   /* toggle state lights the LED */
    smcpad_preset_button(&P, 4, 0);   NO_OUT();
    smcpad_preset_button(&P, SMCPAD_HWBTN_SHIFT, 1);       /* Shift toggle is independent (CC 84) */
    CHECK(!(smcpad_preset_button_leds(&P, 0) & (1u << 4)));
    smcpad_preset_button(&P, 4, 1);   OUT(0xB1, 84, 44);
    CHECK(smcpad_preset_button_leds(&P, 0) & (1u << 4));
    smcpad_preset_button(&P, 4, 0);
    smcpad_preset_button(&P, 4, 1);   OUT(0xB1, 84, 33);
    smcpad_preset_button(&P, 4, 0);
    smcpad_preset_button(&P, SMCPAD_HWBTN_SHIFT, 0);
    smcpad_preset_button(&P, 4, 1);   OUT(0xB1, 20, 33);   /* unshifted was still on -> now off */
    CHECK(!(smcpad_preset_button_leds(&P, 0) & (1u << 4)));
    smcpad_preset_button(&P, 4, 0);
    b[0] = BTN_T_PROGRAM; b[2] = 1; b[3] = 2; b[4] = 3;
    smcpad_preset_button(&P, 4, 1);   OUT(0xB1, 0x00, 2, 0xB1, 0x20, 1, 0xC1, 3);
    smcpad_preset_button(&P, 4, 0);   NO_OUT();
    b[0] = BTN_T_SYSEX;                                    /* factory MMC field: F0 7F 7F 06 02 F7 */
    smcpad_preset_button(&P, 4, 1);   OUT(0xF0, 0x7F, 0x7F, 0x06, 0x02, 0xF7);
    smcpad_preset_button(&P, 4, 0);   NO_OUT();
    b[0] = BTN_T_NOTE; b[1] = 5; b[2] = 94;
    mcp_bytes = 0;
    smcpad_preset_button(&P, 4, 1);   OUT(0x90, 94, 0x7f); /* always ch1 */
    smcpad_preset_button(&P, 4, 0);   OUT(0x90, 94, 0x00);
    CHECK(mcp_bytes == 6);

    /* pad bank: toggle in presets 1..4 */
    CHECK(globals[GLOB_BANK] == 0);
    smcpad_preset_button(&P, SMCPAD_HWBTN_PAD_BANK, 1);
    CHECK(globals[GLOB_BANK] == 1 && gchanged == 1);
    CHECK(smcpad_preset_button_leds(&P, 0) & (1u << SMCPAD_HWBTN_PAD_BANK));
    smcpad_preset_button(&P, SMCPAD_HWBTN_PAD_BANK, 0);
    CHECK(globals[GLOB_BANK] == 1 && gchanged == 1);
    smcpad_preset_button(&P, SMCPAD_HWBTN_PAD_BANK, 1);
    CHECK(globals[GLOB_BANK] == 0 && gchanged == 2);
    smcpad_preset_button(&P, SMCPAD_HWBTN_PAD_BANK, 0);
    /* momentary in presets 5..8 (stock 0x1E11182) */
    globals[GLOB_PRESET] = 4;
    smcpad_preset_button(&P, SMCPAD_HWBTN_PAD_BANK, 1);
    CHECK(globals[GLOB_BANK] == 1);
    smcpad_preset_button(&P, SMCPAD_HWBTN_PAD_BANK, 0);
    CHECK(globals[GLOB_BANK] == 0);
    NO_OUT();

    /* knob bank */
    CHECK(P.knob_bank == 0);
    smcpad_preset_button(&P, SMCPAD_HWBTN_KNOB_BANK, 1);
    smcpad_preset_button(&P, SMCPAD_HWBTN_KNOB_BANK, 0);
    CHECK(P.knob_bank == 1);
    CHECK(smcpad_preset_button_leds(&P, 0) & (1u << SMCPAD_HWBTN_KNOB_BANK));
    CHECK(smcpad_preset_button_leds(&P, 1u << 9) & (1u << 9));   /* held buttons lit */
    NO_OUT();
}

static void test_knobs(void)
{
    setup();
    /* factory knob 0: CC30 ch1 0..127, speed 2. +1 step -> 1 */
    smcpad_preset_knob(&P, 0, 1);
    NO_OUT();                                              /* sent from the tick */
    smcpad_preset_tick(&P, 0);
    OUT(0xB0, 30, 1);
    /* +3 steps at speed 2 = +6: values 2..7, all sent */
    smcpad_preset_knob(&P, 0, 3);
    smcpad_preset_tick(&P, 10);
    OUT(0xB0, 30, 2, 0xB0, 30, 3, 0xB0, 30, 4, 0xB0, 30, 5, 0xB0, 30, 6, 0xB0, 30, 7);
    /* at most 8 values per tick: +20 at speed 2 = +80 -> 87, first tick sends 8..15 */
    smcpad_preset_knob(&P, 0, 20);
    smcpad_preset_tick(&P, 20);
    CHECK(out_len == 8 * 3 && out[2] == 8 && out[23] == 15);
    out_len = 0;
    smcpad_preset_tick(&P, 25);
    CHECK(out_len == 8 * 3 && out[2] == 16 && out[23] == 23);
    out_len = 0;
    /* clamp at 0: from 3, walk 2, 1, 0 */
    P.knob_target[0] = P.knob_sent[0] = 3;
    smcpad_preset_knob(&P, 0, -20);
    smcpad_preset_tick(&P, 30);
    OUT(0xB0, 30, 2, 0xB0, 30, 1, 0xB0, 30, 0);
    smcpad_preset_tick(&P, 35);
    NO_OUT();
    /* bank 2 -> record 8 = CC38 */
    P.knob_bank = 1;
    smcpad_preset_knob(&P, 0, 1);
    smcpad_preset_tick(&P, 40);
    OUT(0xB0, 38, 1);
    P.knob_bank = 0;

    /* range [64, 70]: the first move jumps straight into the range */
    u8 *k = knob_rec(0, 1);
    k[0] = KNOB_T_CC; k[1] = 0; k[2] = 3; k[3] = 7; k[4] = 64; k[5] = 70;
    smcpad_preset_knob(&P, 1, 1);
    smcpad_preset_tick(&P, 50);
    OUT(0xB3, 7, 64);
    smcpad_preset_knob(&P, 1, 30);
    smcpad_preset_tick(&P, 60);
    CHECK(out_len == 6 * 3 && out[out_len - 1] == 70);
    out_len = 0;

    /* aftertouch type */
    k[0] = KNOB_T_AT;
    smcpad_preset_knob(&P, 1, -1);
    smcpad_preset_tick(&P, 70);
    OUT(0xD3, 69);

    /* CW: Right = byte 4 per step, Left = byte 5 */
    k[0] = KNOB_T_CW; k[4] = 1; k[5] = 127;
    smcpad_preset_knob(&P, 1, 2);
    OUT(0xB3, 7, 1, 0xB3, 7, 1);
    smcpad_preset_knob(&P, 1, -1);
    OUT(0xB3, 7, 127);
    smcpad_preset_tick(&P, 80);
    NO_OUT();

    /* Mackie fader: touch 90 68+ch 7F on ch1, En 00 v, release after the timeout */
    k[0] = KNOB_T_FADER; k[1] = 0; k[2] = 2; k[4] = 0; k[5] = 127;
    P.knob_target[1] = P.knob_sent[1] = 10;
    mcp_bytes = 0;
    smcpad_preset_knob(&P, 1, 1);
    smcpad_preset_tick(&P, 1000);
    OUT(0x90, 0x6A, 0x7f, 0xE2, 0, 11);
    CHECK(mcp_bytes == 6);
    smcpad_preset_knob(&P, 1, 1);
    smcpad_preset_tick(&P, 1500);
    OUT(0xE2, 0, 12);                                      /* still touched */
    smcpad_preset_tick(&P, 1500 + SMCPAD_FADER_RELEASE_MS - 1);
    NO_OUT();
    smcpad_preset_tick(&P, 1500 + SMCPAD_FADER_RELEASE_MS);
    OUT(0x90, 0x6A, 0x00);
    smcpad_preset_tick(&P, 5000);
    NO_OUT();
    smcpad_preset_knob(&P, 9, 1);                          /* out of range knob */
    smcpad_preset_tick(&P, 5000);
    NO_OUT();
}

static void test_feedback_leds(void)
{
    setup();
    /* host CC feedback sets the knob value: no jump on the next move */
    u8 m1[3] = {0xB0, 30, 100};
    smcpad_preset_midi_in(&P, m1, 3);
    smcpad_preset_knob(&P, 0, 1);
    smcpad_preset_tick(&P, 0);
    OUT(0xB0, 30, 101);
    u8 m2[3] = {0xB1, 30, 5};                              /* other channel: ignored */
    smcpad_preset_midi_in(&P, m2, 3);
    CHECK(P.knob_target[0] == 101);
    /* late echo from DAW while turning / within SMCPAD_KNOB_ECHO_MS is ignored; accepted after */
    u8 echo[3] = {0xB0, 30, 50};
    smcpad_preset_midi_in(&P, echo, 3);
    CHECK(P.knob_target[0] == 101 && P.knob_sent[0] == 101);
    smcpad_preset_tick(&P, SMCPAD_KNOB_ECHO_MS - 1);
    smcpad_preset_midi_in(&P, echo, 3);
    CHECK(P.knob_target[0] == 101);
    smcpad_preset_tick(&P, SMCPAD_KNOB_ECHO_MS);
    smcpad_preset_midi_in(&P, echo, 3);
    CHECK(P.knob_target[0] == 50 && P.knob_sent[0] == 50);
    /* range clamp + AT / Fader (raw & BLE-wrapped) host feedback */
    u8 *k1 = knob_rec(0, 1);
    k1[0] = KNOB_T_CC; k1[2] = 0; k1[3] = 31; k1[4] = 20; k1[5] = 80;
    u8 m_hi[3] = {0xB0, 31, 120};
    smcpad_preset_midi_in(&P, m_hi, 3);
    CHECK(P.knob_target[1] == 80 && P.knob_sent[1] == 80);
    k1[0] = KNOB_T_AT; k1[2] = 2;
    u8 m_at[2] = {0xD2, 65};
    smcpad_preset_midi_in(&P, m_at, 2);
    CHECK(P.knob_target[1] == 65 && P.knob_sent[1] == 65);
    k1[0] = KNOB_T_FADER; k1[2] = 3;
    u8 m_fd[7] = {0xF0, 0x35, 0x59, 0xE3, 0x00, 72, 0xF7};
    smcpad_preset_midi_in(&P, m_fd, 7);
    CHECK(P.knob_target[1] == 72 && P.knob_sent[1] == 72);

    /* pad Led feedback */
    pad_rec(0, 32)[8] = 60;                                /* pad 0 (record 32) Led = note 60 */
    u32 idle = smcpad_preset_pad_rgb(&P, 0);
    CHECK(idle == ((0xf0 >> 3) << 16 | (0xf0 >> 3)));      /* factory f000f0, dimmed */
    P.led_dirty = 0;
    u8 on[3] = {0x90, 60, 127};
    smcpad_preset_midi_in(&P, on, 3);
    CHECK(P.pad_led[32] == 127 && P.led_dirty);
    CHECK(smcpad_preset_pad_rgb(&P, 0) == 0xf000f0);
    u8 half[3] = {0x95, 60, 63};                           /* any channel */
    smcpad_preset_midi_in(&P, half, 3);
    CHECK(smcpad_preset_pad_rgb(&P, 0) == (0x78u << 16 | 0x78u));
    u8 off[3] = {0x80, 60, 0};
    smcpad_preset_midi_in(&P, off, 3);
    CHECK(P.pad_led[32] == 0 && smcpad_preset_pad_rgb(&P, 0) == idle);
    /* held pad = full colour */
    smcpad_preset_pad(&P, 0, 1, 100);
    out_len = 0;
    CHECK(smcpad_preset_pad_rgb(&P, 0) == 0xf000f0);
    smcpad_preset_pad(&P, 0, 0, 0);
    out_len = 0;
    /* black record: held shows white-ish */
    pad_rec(0, 33)[5] = pad_rec(0, 33)[6] = pad_rec(0, 33)[7] = 0;
    CHECK(smcpad_preset_pad_rgb(&P, 1) == 0);
    smcpad_preset_pad(&P, 1, 1, 100);
    out_len = 0;
    CHECK(smcpad_preset_pad_rgb(&P, 1) == 0x606060);
    smcpad_preset_pad(&P, 1, 0, 0);
    out_len = 0;

    /* button Led (byte 22) + clear all */
    btn_rec(0, 2)[22] = 94;
    u8 b_on[3] = {0x90, 94, 127};
    smcpad_preset_midi_in(&P, b_on, 3);
    CHECK(P.btn_led[2] == 127);
    CHECK(smcpad_preset_button_leds(&P, 0) & (1u << (SMCPAD_HWBTN_REC0 + 2)));
    smcpad_preset_midi_in(&P, on, 3);
    u8 clr[3] = {0x9F, 0x7F, 0x7F};
    smcpad_preset_midi_in(&P, clr, 3);
    CHECK(P.btn_led[2] == 0 && P.pad_led[32] == 0);
    CHECK(!(smcpad_preset_button_leds(&P, 0) & (1u << (SMCPAD_HWBTN_REC0 + 2))));
    /* 0xFF Led never matches, short messages are ignored */
    u8 two[2] = {0xD0, 5};
    smcpad_preset_midi_in(&P, two, 2);
    NO_OUT();
    /* out-of-range record: pad dark */
    globals[GLOB_OCTAVE] = 0; globals[GLOB_TRANSPOSE] = (u8)(s8)-1;
    CHECK(smcpad_preset_pad_rgb(&P, 0) == 0);
}

/* Shift + pad: matches stock SMC-PAD_005 behaviour */
static void test_shift(void)
{
    setup();
    u8 *g = globals;
    smcpad_preset_button(&P, SMCPAD_HWBTN_SHIFT, 1);
    smcpad_preset_pad(&P, 1, 1, 100);                     /* pad 2 = DAW preset */
    CHECK(g[GLOB_PRESET] == 1 && gchanged == 1);
    NO_OUT();
    smcpad_preset_pad(&P, 1, 0, 0);
    NO_OUT();
    smcpad_preset_pad(&P, 0, 1, 100);                     /* pad 1 = Performance */
    smcpad_preset_pad(&P, 0, 0, 0);
    CHECK(g[GLOB_PRESET] == 0);
    smcpad_preset_pad(&P, 11, 1, 100);                    /* pad 12 = curve 3 */
    smcpad_preset_pad(&P, 11, 0, 0);
    CHECK(g[GLOB_PAD_CURVE] == 3);
    NO_OUT();

    /* octave +: 2 -> 6, then capped */
    for (int i = 0; i < 5; i++) {
        smcpad_preset_pad(&P, 15, 1, 100);
        smcpad_preset_pad(&P, 15, 0, 0);
    }
    CHECK(g[GLOB_OCTAVE] == 6 && g[GLOB_TRANSPOSE] == 0);
    smcpad_preset_pad(&P, 13, 1, 100);                    /* octave 6: no transpose up from 0 */
    smcpad_preset_pad(&P, 13, 0, 0);
    CHECK(g[GLOB_TRANSPOSE] == 0);
    for (int i = 0; i < 3; i++) {                         /* transpose -3 */
        smcpad_preset_pad(&P, 12, 1, 100);
        smcpad_preset_pad(&P, 12, 0, 0);
    }
    CHECK((s8)g[GLOB_TRANSPOSE] == -3);
    for (int i = 0; i < 7; i++) {                         /* octave down to 0 (capped): transpose reset */
        smcpad_preset_pad(&P, 14, 1, 100);
        smcpad_preset_pad(&P, 14, 0, 0);
    }
    CHECK(g[GLOB_OCTAVE] == 0 && g[GLOB_TRANSPOSE] == 0);
    smcpad_preset_pad(&P, 12, 1, 100);                    /* octave 0: no transpose below 0 */
    smcpad_preset_pad(&P, 12, 0, 0);
    CHECK(g[GLOB_TRANSPOSE] == 0);
    smcpad_preset_pad(&P, 13, 1, 100);
    smcpad_preset_pad(&P, 13, 0, 0);
    CHECK(g[GLOB_TRANSPOSE] == 1);
    g[GLOB_OCTAVE] = 2; g[GLOB_TRANSPOSE] = 16;           /* +16 is the top */
    smcpad_preset_pad(&P, 13, 1, 100);
    smcpad_preset_pad(&P, 13, 0, 0);
    CHECK(g[GLOB_TRANSPOSE] == 16);
    g[GLOB_TRANSPOSE] = (u8)(s8)-16;                      /* -16 is the bottom */
    smcpad_preset_pad(&P, 12, 1, 100);
    smcpad_preset_pad(&P, 12, 0, 0);
    CHECK((s8)g[GLOB_TRANSPOSE] == -16);
    g[GLOB_TRANSPOSE] = 0;
    NO_OUT();

    /* Shift released before the pad: still no Note Off */
    smcpad_preset_pad(&P, 0, 1, 100);
    smcpad_preset_button(&P, SMCPAD_HWBTN_SHIFT, 0);
    smcpad_preset_pad(&P, 0, 0, 0);
    NO_OUT();
    CHECK(P.shift_pads == 0);
    /* a note held before Shift ends normally */
    smcpad_preset_pad(&P, 0, 1, 100);
    CHECK(out_len == 3 && out[0] == 0x99 && out[1] == 36);
    out_len = 0;
    smcpad_preset_button(&P, SMCPAD_HWBTN_SHIFT, 1);
    smcpad_preset_pad(&P, 0, 0, 0);
    CHECK(out_len >= 3 && out[out_len - 3] == 0x89 && out[out_len - 2] == 36);
    out_len = 0;
    smcpad_preset_button(&P, SMCPAD_HWBTN_SHIFT, 0);

    /* Shift view colours */
    g[GLOB_PRESET] = 0; g[GLOB_PAD_CURVE] = 1; g[GLOB_OCTAVE] = 2; g[GLOB_TRANSPOSE] = 0;
    u32 normal = smcpad_preset_pad_rgb(&P, 0);
    smcpad_preset_button(&P, SMCPAD_HWBTN_SHIFT, 1);
    CHECK(smcpad_preset_pad_rgb(&P, 0) == 0xFF6000);                 /* Performance, selected */
    CHECK(smcpad_preset_pad_rgb(&P, 1) == 0x00141F);                 /* DAW 00A0FF / 8 */
    pad_rec(2, 32)[5] = 0x10; pad_rec(2, 32)[6] = 0x80; pad_rec(2, 32)[7] = 0x40;
    CHECK(smcpad_preset_pad_rgb(&P, 2) == 0x021008);                 /* user: own pad-1 colour / 8 */
    pad_rec(3, 32)[5] = pad_rec(3, 32)[6] = pad_rec(3, 32)[7] = 0;
    CHECK(smcpad_preset_pad_rgb(&P, 3) == 0x10001F);                 /* no colour: purple / 8 */
    CHECK(smcpad_preset_pad_rgb(&P, 9) == 0x00FF30);                 /* curve 1 selected */
    CHECK(smcpad_preset_pad_rgb(&P, 8) != 0 && smcpad_preset_pad_rgb(&P, 8) < smcpad_preset_pad_rgb(&P, 11));
    u32 dim_tr = smcpad_preset_pad_rgb(&P, 13);
    CHECK(dim_tr == smcpad_preset_pad_rgb(&P, 12) && dim_tr != 0);   /* default: both dim */
    g[GLOB_TRANSPOSE] = 8;
    CHECK(smcpad_preset_pad_rgb(&P, 13) > dim_tr && smcpad_preset_pad_rgb(&P, 12) == dim_tr);
    g[GLOB_TRANSPOSE] = 16;
    CHECK(smcpad_preset_pad_rgb(&P, 13) == 0);                       /* limit: off */
    g[GLOB_TRANSPOSE] = 0; g[GLOB_OCTAVE] = 6;
    CHECK(smcpad_preset_pad_rgb(&P, 15) == 0 && smcpad_preset_pad_rgb(&P, 14) != 0);
    CHECK(smcpad_preset_pad_rgb(&P, 13) == 0);                       /* octave 6: no transpose up */
    g[GLOB_OCTAVE] = 2;
    smcpad_preset_button(&P, SMCPAD_HWBTN_SHIFT, 0);
    CHECK(smcpad_preset_pad_rgb(&P, 0) == normal);                   /* back to the preset colours */

    /* Shift + Pad 15 + Pad 16: default octave (stock manual) */
    g[GLOB_OCTAVE] = 5; g[GLOB_TRANSPOSE] = 3;
    smcpad_preset_button(&P, SMCPAD_HWBTN_SHIFT, 1);
    smcpad_preset_pad(&P, 14, 1, 100);                               /* octave -1 -> 4 */
    smcpad_preset_pad(&P, 15, 1, 100);                               /* + pad 16: reset */
    CHECK(g[GLOB_OCTAVE] == 2 && g[GLOB_TRANSPOSE] == 0);
    smcpad_preset_pad(&P, 15, 0, 0);
    smcpad_preset_pad(&P, 14, 0, 0);
    smcpad_preset_button(&P, SMCPAD_HWBTN_SHIFT, 0);
    NO_OUT();
    /* pad note helper (sequencer defaults) */
    u8 ch = 0;
    CHECK(smcpad_preset_pad_note(&P, 0, &ch) == 36 && ch == 9);
}

/* global scale (shared with the sequencer): Shift + K3 / K4, Note pads remapped from pad 1 */
static void test_global_scale(void)
{
    setup();
    schanged = 0;
    pad_rec(0, 34)[2] = 99;                                /* pad 3: an odd note in the preset */
    smcpad_preset_pad(&P, 2, 1, 127);                      /* scale 0 = the preset's own notes */
    smcpad_preset_pad(&P, 2, 0, 0);
    OUT(0x99, 99, 0x7F, 0x89, 99, 0x40);
    /* Shift + K4: follow preset -> chromatic (pad 1), pick display while Shift stays held */
    smcpad_preset_button(&P, SMCPAD_HWBTN_SHIFT, 1);
    smcpad_preset_knob(&P, 3, 2);                          /* one per report */
    CHECK(P.scale == 1 && schanged == 1 && P.scale_view == 2);
    CHECK(smcpad_preset_pad_rgb(&P, 0) == 0x00FFC0 && smcpad_preset_pad_rgb(&P, 1) == rgb_scale(0x00FFC0, 1, 12));
    CHECK(smcpad_preset_pad_rgb(&P, 11) != 0 && smcpad_preset_pad_rgb(&P, 12) == 0);
    smcpad_preset_knob(&P, 2, -1);                         /* K3 root: clamped at follow = no pad lit */
    CHECK(P.root == 0 && P.scale_view == 1 && smcpad_preset_pad_rgb(&P, 0) == 0 && smcpad_preset_pad_rgb(&P, 1) == 0);
    smcpad_preset_knob(&P, 2, 1);                          /* C = pad 1 */
    CHECK(P.root == 1 && smcpad_preset_pad_rgb(&P, 0) == 0x00FFC0 && smcpad_preset_pad_rgb(&P, 12) == 0);
    smcpad_preset_knob(&P, 2, -1);
    CHECK(P.root == 0);
    smcpad_preset_knob(&P, 0, 1);                          /* other knobs: still their CC */
    CHECK(P.knob_target[0] != 0 && P.scale == 1);
    smcpad_preset_button(&P, SMCPAD_HWBTN_SHIFT, 0);
    CHECK(P.scale_view == 0);
    smcpad_preset_tick(&P, 1000);
    out_len = 0;
    /* chromatic: pad n = pad 1's note + n (36 + 2) */
    smcpad_preset_pad(&P, 2, 1, 127);
    OUT(0x99, 38, 0x7F);
    /* major from C: pad 3 = E; the release ends the note it started even if the scale changed */
    P.scale = SCALE_MAJOR + 1;
    smcpad_preset_pad(&P, 2, 0, 0);
    OUT(0x89, 38, 0x40);
    smcpad_preset_pad(&P, 2, 1, 127);
    P.scale = 0;
    smcpad_preset_pad(&P, 2, 0, 0);
    OUT(0x99, 40, 0x7F, 0x89, 40, 0x40);
    P.scale = SCALE_MAJOR + 1;
    u8 ch = 0;
    CHECK(smcpad_preset_scaled_note(&P, 7, &ch) == 48 && ch == 9);   /* C one octave up */
    /* root pads brighter (pads 1 and 8 = C: root follows the preset = pad 1's note) */
    CHECK(smcpad_preset_pad_rgb(&P, 7) == smcpad_preset_pad_rgb(&P, 0));
    CHECK(smcpad_preset_pad_rgb(&P, 7) > smcpad_preset_pad_rgb(&P, 6));
    /* transpose moves in scale steps from pad 1's note: +1 = D on pad 1 */
    globals[GLOB_TRANSPOSE] = 1;
    CHECK(smcpad_preset_scaled_note(&P, 0, NULL) == 38 && smcpad_preset_scaled_note(&P, 6, NULL) == 48);
    globals[GLOB_TRANSPOSE] = 0;
    /* root D: pad 1 stays the base note, the scale is D major */
    P.root = 3;
    CHECK(smcpad_preset_scaled_note(&P, 0, NULL) == 36 && smcpad_preset_scaled_note(&P, 1, NULL) == 37);
    /* not a Note pad: untouched */
    pad_rec(0, 35)[0] = PAD_T_CC_MOMENTARY;
    CHECK(smcpad_preset_scaled_note(&P, 3, NULL) == 0xFF);
    /* pad 1 not a Note pad: no base, own notes */
    pad_rec(0, 32)[0] = PAD_T_CC_MOMENTARY;
    CHECK(smcpad_preset_scaled_note(&P, 2, NULL) == 99);
    CHECK(smcpad_preset_default_root(&P) == 0);            /* not a Note pad: C */
    pad_rec(0, 32)[0] = PAD_T_NOTE;
    /* sequencer defaults: octave 2, no transpose, whatever the Shift menu says */
    globals[GLOB_OCTAVE] = 4;
    globals[GLOB_TRANSPOSE] = 1;
    u8 dc = 0;
    CHECK(smcpad_preset_default_note(&P, 1, &dc) == 37 && dc == 9 && smcpad_preset_default_root(&P) == 0);
    CHECK(smcpad_preset_own_root(&P) == (u8)((4 * 16 + 4) % 12));   /* normal mode: pad 1 at octave 4 */
}

int main(int argc, char **argv)
{
    test_factory(argc > 1 ? argv[1] : NULL);
    test_helpers();
    test_pads_note_at();
    test_pads_other_types();
    test_buttons();
    test_knobs();
    test_feedback_leds();
    test_shift();
    test_global_scale();
    printf(failures ? "test_smcpad_preset: %d FAILED\n" : "test_smcpad_preset: ALL PASSED\n", failures);
    return failures != 0;
}
