/* Host unit tests for midi/ble_midi.c.
 *   gcc -Wall -Wextra -Werror -std=c11 -I sinco -I midi test/test_ble_midi.c -o /tmp/t && /tmp/t
 * Vectors follow the Apple "MIDI over Bluetooth LE" packet rules (header/timestamp bytes,
 * running status, real-time inside SysEx, SysEx continuation packets).
 */
#define SMCPAD_HOST_TEST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../midi/ble_midi.c"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* collected decoder output: messages concatenated, lengths recorded */
static u8 out[4096];
static u32 out_len, out_msgs, out_sx_bytes;
static u8 sx_stream[1024];
static void cb(void *ctx, const u8 *m, u32 n)
{
    (void)ctx;
    memcpy(out + out_len, m, n);
    out_len += n;
    out_msgs++;
}
static void sxb(void *ctx, u8 b)
{
    (void)ctx;
    if (out_sx_bytes < sizeof(sx_stream)) {
        sx_stream[out_sx_bytes] = b;
    }
    out_sx_bytes++;
}
static void reset_out(void)
{
    out_len = out_msgs = out_sx_bytes = 0;
}
static struct ble_midi_dec dec;
static void decode(const u8 *p, u32 n)
{
    ble_midi_dec_packet(&dec, p, n);
}

static void test_encoder_basic(void)
{
    struct ble_midi_enc e;
    u8 pkt[BLE_MIDI_PKT_MAX];
    ble_midi_enc_init(&e, 20);
    const u8 on[3] = {0x90, 0x3C, 0x64}, off[3] = {0x80, 0x3C, 0x00};
    /* ts 0x1234 & 0x1FFF = 4660: header 0x80 | 36 = 0xA4, timestamp 0x80 | 52 = 0xB4 */
    CHECK(ble_midi_enc_add(&e, 0x1234, on, 3) == 3);
    CHECK(ble_midi_enc_add(&e, 0x1235, off, 3) == 3);
    const u8 want[] = {0xA4, 0xB4, 0x90, 0x3C, 0x64, 0xB5, 0x80, 0x3C, 0x00};
    CHECK(ble_midi_enc_take(&e, pkt) == sizeof(want) && memcmp(pkt, want, sizeof(want)) == 0);
    CHECK(ble_midi_enc_take(&e, pkt) == 0);

    /* >= 128 ms jump -> new packet; full packet -> 0 */
    CHECK(ble_midi_enc_add(&e, 0x0000, on, 3) == 3);
    CHECK(ble_midi_enc_add(&e, 0x0080, on, 3) == 0);
    for (int i = 0; i < 3; i++) {
        CHECK(ble_midi_enc_add(&e, 0x0001, on, 3) == 3);   /* 1 + 4*4 = 17 */
    }
    CHECK(ble_midi_enc_add(&e, 0x0001, on, 3) == 0);       /* 21 > 20 */
    const u8 pc[2] = {0xC0, 5};
    CHECK(ble_midi_enc_add(&e, 0x0001, pc, 2) == 2);       /* exactly 20 */
    CHECK(ble_midi_enc_take(&e, pkt) == 20);
    /* malformed messages rejected */
    CHECK(ble_midi_enc_add(&e, 0, on, 2) == 0 && ble_midi_enc_add(&e, 0, pc + 1, 1) == 0);
    const u8 clk = 0xF8;
    CHECK(ble_midi_enc_add(&e, 0, &clk, 1) == 1);
    CHECK(ble_midi_enc_take(&e, pkt) == 3 && pkt[2] == 0xF8);
    /* 7-bit timestampLow wraps across a 128 ms boundary within the same packet (< 128 ms step) */
    CHECK(ble_midi_enc_add(&e, 117, &clk, 1) == 1);
    CHECK(ble_midi_enc_add(&e, 138, &clk, 1) == 1);        /* 21 ms later: hi 0 -> 1, lo 117 -> 10 */
    const u8 wrap_want[] = {0x80, 0x80 | 117, 0xF8, 0x80 | 10, 0xF8};
    CHECK(ble_midi_enc_take(&e, pkt) == sizeof(wrap_want) && memcmp(pkt, wrap_want, sizeof(wrap_want)) == 0);
}

static void test_encoder_sysex_split(void)
{
    struct ble_midi_enc e;
    u8 pkt[BLE_MIDI_PKT_MAX];
    u8 sx[14] = {0xF0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0xF7};
    ble_midi_enc_init(&e, 10);
    u32 c = ble_midi_enc_add(&e, 5, sx, sizeof(sx));
    CHECK(c == 8);                                  /* hdr ts F0 + 7 data = 10 */
    CHECK(ble_midi_enc_take(&e, pkt) == 10 && pkt[0] == 0x80 && pkt[1] == 0x85 && pkt[2] == 0xF0 && pkt[9] == 7);
    CHECK(ble_midi_enc_add(&e, 6, sx + c, sizeof(sx) - c) == sizeof(sx) - c);
    const u8 want2[] = {0x80, 8, 9, 10, 11, 12, 0x86, 0xF7};
    CHECK(ble_midi_enc_take(&e, pkt) == sizeof(want2) && memcmp(pkt, want2, sizeof(want2)) == 0);
    CHECK(e.sx_cont == 0);

    /* F7 that does not fit with its timestamp moves to the next packet */
    u8 sx2[9] = {0xF0, 1, 2, 3, 4, 5, 6, 7, 0xF7};
    c = ble_midi_enc_add(&e, 0, sx2, sizeof(sx2));
    CHECK(c == 8 && e.sx_cont);
    CHECK(ble_midi_enc_take(&e, pkt) == 10);
    CHECK(ble_midi_enc_add(&e, 0, sx2 + 8, 1) == 1);
    CHECK(ble_midi_enc_take(&e, pkt) == 3 && pkt[1] == 0x80 && pkt[2] == 0xF7);
    /* a SysEx does not start in a nearly full packet */
    const u8 on[3] = {0x90, 1, 2};
    ble_midi_enc_add(&e, 0, on, 3);
    ble_midi_enc_add(&e, 0, on, 3);                 /* len 9 */
    CHECK(ble_midi_enc_add(&e, 0, sx2, sizeof(sx2)) == 0);
}

static void test_decoder_spec_cases(void)
{
    ble_midi_dec_init(&dec, cb, sxb, NULL);
    reset_out();
    /* running status without timestamps */
    const u8 p1[] = {0x80, 0x80, 0x90, 0x3C, 0x40, 0x3E, 0x40};
    decode(p1, sizeof(p1));
    const u8 w1[] = {0x90, 0x3C, 0x40, 0x90, 0x3E, 0x40};
    CHECK(out_msgs == 2 && out_len == 6 && memcmp(out, w1, 6) == 0);
    /* running status with a timestamp, then a new status with timestamp */
    reset_out();
    const u8 p2[] = {0x80, 0x81, 0x90, 0x3C, 0x40, 0x82, 0x3E, 0x40, 0x83, 0x80, 0x3C, 0x00};
    decode(p2, sizeof(p2));
    const u8 w2[] = {0x90, 0x3C, 0x40, 0x90, 0x3E, 0x40, 0x80, 0x3C, 0x00};
    CHECK(out_msgs == 3 && out_len == 9 && memcmp(out, w2, 9) == 0);
    /* timestamp bytes that look like real-time (0xF8..0xFF) are still timestamps */
    reset_out();
    const u8 p3[] = {0xBF, 0xFF, 0xB0, 0x07, 0x64, 0xF8, 0xC0, 0x05};
    decode(p3, sizeof(p3));
    const u8 w3[] = {0xB0, 0x07, 0x64, 0xC0, 0x05};
    CHECK(out_msgs == 2 && out_len == 5 && memcmp(out, w3, 5) == 0);
    /* real-time inside a SysEx and inside a channel message */
    reset_out();
    const u8 p4[] = {0x80, 0x80, 0xF0, 0x01, 0x02, 0x81, 0xF8, 0x03, 0x82, 0xF7,
                     0x83, 0x90, 0x3C, 0x84, 0xFE, 0x40};
    decode(p4, sizeof(p4));
    const u8 w4[] = {0xF8, 0xF0, 0x01, 0x02, 0x03, 0xF7, 0xFE, 0x90, 0x3C, 0x40};
    CHECK(out_msgs == 4 && out_len == sizeof(w4) && memcmp(out, w4, sizeof(w4)) == 0);
    CHECK(out_sx_bytes == 5 && memcmp(sx_stream, w4 + 1, 5) == 0);
    /* SysEx across packets (continuation has no timestamp before the data) */
    reset_out();
    const u8 p5a[] = {0x80, 0x80, 0xF0, 0x7D, 0x01};
    const u8 p5b[] = {0x80, 0x02, 0x03};
    const u8 p5c[] = {0x81, 0x04, 0x85, 0xF7};
    decode(p5a, sizeof(p5a));
    decode(p5b, sizeof(p5b));
    CHECK(out_msgs == 0);
    decode(p5c, sizeof(p5c));
    const u8 w5[] = {0xF0, 0x7D, 0x01, 0x02, 0x03, 0x04, 0xF7};
    CHECK(out_msgs == 1 && out_len == 7 && memcmp(out, w5, 7) == 0);
    /* malformed: no header bit, too short, stray data, incomplete message at packet end */
    reset_out();
    u32 d0 = dec.dropped;
    const u8 bad1[] = {0x00, 0x80, 0x90, 0x3C, 0x40};
    decode(bad1, sizeof(bad1));
    decode(bad1 + 1, 1);
    CHECK(dec.dropped == d0 + 2 && out_msgs == 0);
    ble_midi_dec_init(&dec, cb, NULL, NULL);
    const u8 stray[] = {0x80, 0x12, 0x34};
    decode(stray, sizeof(stray));
    const u8 cut[] = {0x80, 0x80, 0x90, 0x3C};
    decode(cut, sizeof(cut));
    const u8 next[] = {0x80, 0x80, 0xC0, 0x01};
    decode(next, sizeof(next));
    CHECK(out_msgs == 1 && out[0] == 0xC0 && out[1] == 0x01);
    /* SysEx aborted by a status byte, and an oversize SysEx */
    reset_out();
    d0 = dec.dropped;
    const u8 ab[] = {0x80, 0x80, 0xF0, 0x01, 0x81, 0x90, 0x3C, 0x40};
    decode(ab, sizeof(ab));
    CHECK(out_msgs == 1 && out[0] == 0x90 && dec.dropped == d0 + 1);
    reset_out();
    u8 big[BLE_MIDI_PKT_MAX];
    big[0] = 0x80; big[1] = 0x80; big[2] = 0xF0;
    memset(big + 3, 0x11, sizeof(big) - 3);
    for (int k = 0; k < 3; k++) {
        decode(big, sizeof(big));
        big[1] = 0x11;                              /* continuation packets after the first */
        big[2] = 0x11;
    }
    const u8 end[] = {0x80, 0x80, 0xF7};
    d0 = dec.dropped;
    decode(end, sizeof(end));
    CHECK(out_msgs == 0 && dec.dropped == d0 + 1);
}

static void test_roundtrip(void)
{
    static u8 msgs[2000], pkt[BLE_MIDI_PKT_MAX];
    struct ble_midi_enc e;
    srand(3);
    for (u32 mtu_payload = 5; mtu_payload <= 128; mtu_payload += 41) {
        u32 n = 0;
        while (n < sizeof(msgs) - 300) {
            u32 kind = (u32)rand() % 10;
            if (kind < 6) {
                u8 st = (u8)(0x80 | ((rand() % 7) << 4) | (rand() & 15));
                msgs[n++] = st;
                for (u32 i = 1; i < ble_midi_msg_len(st); i++) {
                    msgs[n++] = (u8)(rand() & 0x7F);
                }
            } else if (kind < 8) {
                static const u8 rt[] = {0xF8, 0xFA, 0xFB, 0xFC, 0xFE, 0xFF};
                msgs[n++] = rt[(u32)rand() % sizeof(rt)];
            } else {
                u32 l = (u32)rand() % 250;
                msgs[n++] = 0xF0;
                for (u32 i = 0; i < l; i++) {
                    msgs[n++] = (u8)(rand() & 0x7F);
                }
                msgs[n++] = 0xF7;
            }
        }
        ble_midi_enc_init(&e, (u16)mtu_payload);
        ble_midi_dec_init(&dec, cb, NULL, NULL);
        reset_out();
        u16 ts = 0;
        for (u32 i = 0; i < n;) {
            u32 l = msgs[i] == 0xF0 ? (u32)((u8 *)memchr(msgs + i, 0xF7, n - i) - (msgs + i)) + 1
                                     : ble_midi_msg_len(msgs[i]);
            u32 done = 0;
            ts = (u16)(ts + rand() % 40);
            while (done < l) {
                u32 c = ble_midi_enc_add(&e, ts, msgs + i + done, l - done);
                done += c;
                if (done < l) {
                    u32 pl = ble_midi_enc_take(&e, pkt);
                    CHECK(pl > 0 && pl <= mtu_payload);
                    decode(pkt, pl);
                }
            }
            i += l;
        }
        u32 pl = ble_midi_enc_take(&e, pkt);
        if (pl) {
            decode(pkt, pl);
        }
        CHECK(out_len == n && memcmp(out, msgs, n) == 0 && dec.dropped == 0);
    }
}

int main(void)
{
    test_encoder_basic();
    test_encoder_sysex_split();
    test_decoder_spec_cases();
    test_roundtrip();
    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_ble_midi: all tests passed\n");
    return 0;
}
