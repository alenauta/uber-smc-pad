/* Host unit tests for sinco/sinco_proto.c (see README.md).
 *   gcc -Wall -Wextra -Werror -std=c11 -I sinco test/test_sinco_proto.c -o /tmp/t && /tmp/t
 * Golden vectors: Midi Suite worked examples (docs/midisuite_protocol.md 2.3)
 * and the live 0x11 reply of a stock pad (SMC-PAD_005).
 */
#define SMCPAD_HOST_TEST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sinco/sinco_proto.c"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define CHECK_MEM(a, b, n) CHECK(memcmp((a), (b), (n)) == 0)

static const u8 Q_PKT[] = {0x00, 0x59, 0x11, 0x00, 0x00, 0x00, 0xFF};
static const u8 Q_USB[] = {0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7};
static const u8 RG_PKT[] = {0x00, 0x59, 0x23, 0x08, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0xEF};
static const u8 RG_USB[] = {0xF0, 0x00, 0x32, 0x0D, 0x41, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
                            0x40, 0x01, 0x00, 0x00, 0x6F, 0x01, 0xF7};
static const u8 SV_PKT[] = {0x00, 0x59, 0x22, 0x08, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFA};
static const u8 SV_USB[] = {0xF0, 0x00, 0x32, 0x09, 0x41, 0x00, 0x00, 0x40, 0x02, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x7A, 0x01, 0xF7};
static const u8 ERR_PKT[] = {0x00, 0x59, 0x00, 0x01, 0x00, 0x00, 0x01, 0xFE};
static const u8 WOK_PKT[] = {0x00, 0x59, 0x00, 0x01, 0x00, 0x00, 0x00, 0xFF};   /* stock SMC-PAD_005 write ack */

static u8 reply[SINCO_MAX_PKT];

static void live_name_reply(u8 *out)     /* 00 59 11 1b 00 00 "SMC-PAD_005" 00.. 26 */
{
    memset(out, 0, 34);
    out[0] = 0x00; out[1] = 0x59; out[2] = 0x11; out[3] = 0x1b;
    memcpy(out + 6, "SMC-PAD_005", 11);
    out[33] = 0x26;
}

static void test_build_verify(void)
{
    u8 b[64];
    CHECK(sinco_build(0x11, NULL, 0, b) == sizeof(Q_PKT));
    CHECK_MEM(b, Q_PKT, sizeof(Q_PKT));
    const u8 rd[8] = {4, 0, 0, 0, 0, 12, 0, 0};
    CHECK(sinco_build(0x23, rd, 8, b) == sizeof(RG_PKT));
    CHECK_MEM(b, RG_PKT, sizeof(RG_PKT));
    const u8 sv[8] = {5, 0, 0, 0, 0, 0, 0, 0};
    CHECK(sinco_build(0x22, sv, 8, b) == sizeof(SV_PKT));
    CHECK_MEM(b, SV_PKT, sizeof(SV_PKT));
    CHECK(sinco_build_error(1, b) == sizeof(ERR_PKT));
    CHECK_MEM(b, ERR_PKT, sizeof(ERR_PKT));

    u8 cmd; const u8 *p; u32 len;
    CHECK(sinco_verify(RG_PKT, sizeof(RG_PKT), &cmd, &p, &len) && cmd == 0x23 && len == 8 && p[0] == 4);
    u8 bad[sizeof(RG_PKT)];
    memcpy(bad, RG_PKT, sizeof(bad));
    bad[sizeof(bad) - 1] ^= 1;
    CHECK(!sinco_verify(bad, sizeof(bad), &cmd, &p, &len));             /* checksum */
    CHECK(!sinco_verify(RG_PKT, sizeof(RG_PKT) - 1, &cmd, &p, &len));   /* short */
    bad[sizeof(bad) - 1] ^= 1;
    bad[1] = 0x58;
    CHECK(!sinco_verify(bad, sizeof(bad), &cmd, &p, &len));             /* header */
    u8 live[34];
    live_name_reply(live);
    CHECK(sinco_verify(live, 34, &cmd, &p, &len) && cmd == 0x11 && len == 27);
}

static void test_sysex_golden(void)
{
    u8 b[64];
    CHECK(sinco_sysex_encode(Q_PKT, sizeof(Q_PKT), b, sizeof(b)) == sizeof(Q_USB));
    CHECK_MEM(b, Q_USB, sizeof(Q_USB));
    CHECK(sinco_sysex_encode(RG_PKT, sizeof(RG_PKT), b, sizeof(b)) == sizeof(RG_USB));
    CHECK_MEM(b, RG_USB, sizeof(RG_USB));
    CHECK(sinco_sysex_encode(SV_PKT, sizeof(SV_PKT), b, sizeof(b)) == sizeof(SV_USB));
    CHECK_MEM(b, SV_USB, sizeof(SV_USB));
    CHECK(sinco_sysex_encode(SV_PKT, sizeof(SV_PKT), b, sizeof(SV_USB) - 1) == 0);
    CHECK(SINCO_SYSEX_LEN(sizeof(RG_PKT)) == sizeof(RG_USB));

    static struct sinco_sx sx;
    sinco_sx_reset(&sx);
    u32 got = 0;
    for (u32 i = 0; i < sizeof(RG_USB); i++) {
        u32 n = sinco_sx_byte(&sx, RG_USB[i]);
        got = n ? n : got;
    }
    CHECK(got == sizeof(RG_PKT));
    CHECK_MEM(sx.rx.buf, RG_PKT, sizeof(RG_PKT));

    /* real-time interleaved in the frame is ignored */
    sinco_sx_reset(&sx);
    got = 0;
    for (u32 i = 0; i < sizeof(Q_USB); i++) {
        if (i == 4) {
            CHECK(sinco_sx_byte(&sx, 0xF8) == 0);
        }
        u32 n = sinco_sx_byte(&sx, Q_USB[i]);
        got = n ? n : got;
    }
    CHECK(got == sizeof(Q_PKT) && memcmp(sx.rx.buf, Q_PKT, sizeof(Q_PKT)) == 0);

    /* the diagnostic SysEx is not a Sinco frame */
    const u8 diag[] = {0xF0, 0x7D, 0x53, 0x50, 0x01, 0xF7};
    sinco_sx_reset(&sx);
    for (u32 i = 0; i < sizeof(diag); i++) {
        CHECK(sinco_sx_byte(&sx, diag[i]) == 0);
    }
    /* a frame cut short by F7 is dropped, and the next one decodes */
    sinco_sx_reset(&sx);
    for (u32 i = 0; i < 8; i++) {
        CHECK(sinco_sx_byte(&sx, RG_USB[i]) == 0);
    }
    CHECK(sinco_sx_byte(&sx, 0xF7) == 0 && sx.rx.n == 0);
    got = 0;
    for (u32 i = 0; i < sizeof(Q_USB); i++) {
        u32 n = sinco_sx_byte(&sx, Q_USB[i]);
        got = n ? n : got;
    }
    CHECK(got == sizeof(Q_PKT));
}

static void test_sysex_roundtrip(void)
{
    static u8 pkt[SINCO_MAX_PKT], frame[SINCO_SYSEX_LEN(SINCO_MAX_PKT)];
    static struct sinco_sx sx;
    srand(1);
    for (u32 plen = 0; plen <= SINCO_MAX_PAYLOAD; plen += (plen < 40 ? 1 : 97)) {
        u8 payload[SINCO_MAX_PAYLOAD];
        for (u32 i = 0; i < plen; i++) {
            payload[i] = (u8)rand();
        }
        u32 n = sinco_build(0x23, payload, plen, pkt);
        u32 f = sinco_sysex_encode(pkt, n, frame, sizeof(frame));
        CHECK(f == SINCO_SYSEX_LEN(n));
        for (u32 i = 1; i + 1 < f; i++) {
            CHECK(frame[i] < 0x80);
        }
        sinco_sx_reset(&sx);
        u32 got = 0;
        for (u32 i = 0; i < f; i++) {
            u32 r = sinco_sx_byte(&sx, frame[i]);
            got = r ? r : got;
        }
        CHECK(got == n && memcmp(sx.rx.buf, pkt, n) == 0);
    }
}

static void test_reassembler(void)
{
    static struct sinco_rx rx;
    u8 live[34];
    live_name_reply(live);
    memset(&rx, 0, sizeof(rx));

    /* junk, a false start, then the packet in BLE-sized chunks of 20 */
    const u8 junk[] = {0x12, 0x00, 0x00, 0x34, 0x00};
    u32 t = 1000, got = 0;
    sinco_rx_chunk_start(&rx, t);
    for (u32 i = 0; i < sizeof(junk); i++) {
        CHECK(sinco_rx_byte(&rx, junk[i]) == 0);
    }
    for (u32 off = 0; off < 34; off += 20) {
        sinco_rx_chunk_start(&rx, t += 30);
        for (u32 i = off; i < off + 20 && i < 34; i++) {
            u32 n = sinco_rx_byte(&rx, live[i]);
            if (n) {
                CHECK(i == 33);
                got = n;
            }
        }
    }
    CHECK(got == 34 && memcmp(rx.buf, live, 34) == 0);

    /* back-to-back packets */
    got = 0;
    for (u32 k = 0; k < 2; k++) {
        for (u32 i = 0; i < sizeof(Q_PKT); i++) {
            if (sinco_rx_byte(&rx, Q_PKT[i])) {
                got++;
            }
        }
    }
    CHECK(got == 2);

    /* timeout drops a partial packet */
    sinco_rx_chunk_start(&rx, t);
    for (u32 i = 0; i < 10; i++) {
        sinco_rx_byte(&rx, RG_PKT[i]);
    }
    u32 d0 = rx.dropped;
    sinco_rx_chunk_start(&rx, t + SINCO_RX_TIMEOUT_MS + 1);
    CHECK(rx.dropped == d0 + 1 && rx.n == 0);
    got = 0;
    for (u32 i = 0; i < sizeof(RG_PKT); i++) {
        got = sinco_rx_byte(&rx, RG_PKT[i]) ? 1 : got;
    }
    CHECK(got);

    /* oversize length field */
    const u8 big[] = {0x00, 0x59, 0x22, 0xFF, 0xFF, 0x00};
    d0 = rx.dropped;
    for (u32 i = 0; i < sizeof(big); i++) {
        CHECK(sinco_rx_byte(&rx, big[i]) == 0);
    }
    CHECK(rx.dropped == d0 + 1 && rx.n == 0);
}

/* ---- dispatcher with a fake store ---- */
static u8 mem4[12] = {120, 0, 50, 4, 0, 0, 2, 0, 1, 1, 0, 0};
static u8 midi_seen[16];
static u32 midi_seen_len, writes, commits;
static int f_read(void *ctx, u8 type, u32 addr, u8 *dst, u32 len)
{
    (void)ctx;
    if (type != 4 || addr + len > sizeof(mem4)) {
        return -1;
    }
    memcpy(dst, mem4 + addr, len);
    return 0;
}
static int f_write(void *ctx, u8 type, u32 addr, const u8 *src, u32 len)
{
    (void)ctx;
    if (type == 5 && len == 0 && addr == 0) {
        commits++;
        return 0;
    }
    if (type != 4 || addr + len > sizeof(mem4)) {
        return -1;
    }
    memcpy(mem4 + addr, src, len);
    writes++;
    return 0;
}
static void f_midi(void *ctx, const u8 *b, u32 n)
{
    (void)ctx;
    memcpy(midi_seen, b, n);
    midi_seen_len = n;
}
static const struct sinco_ops OPS = {NULL, "SMC-PAD_005", f_read, f_write, f_midi, NULL};

static void test_dispatch(void)
{
    u8 live[34], pkt[64];
    live_name_reply(live);
    CHECK(sinco_handle(Q_PKT, sizeof(Q_PKT), reply, &OPS) == 34);
    CHECK_MEM(reply, live, 34);

    /* read globals: reply echoes type/addr/len, then the data (len + 8) */
    u32 n = sinco_handle(RG_PKT, sizeof(RG_PKT), reply, &OPS);
    CHECK(n == 6 + 8 + 12 + 1);
    u8 cmd; const u8 *p; u32 len;
    CHECK(sinco_verify(reply, n, &cmd, &p, &len) && cmd == 0x23 && len == 20);
    CHECK_MEM(p, RG_PKT + 6, 8);
    CHECK_MEM(p + 8, mem4, 12);

    /* in-place: request and reply in the same buffer */
    memcpy(reply, RG_PKT, sizeof(RG_PKT));
    CHECK(sinco_handle(reply, sizeof(RG_PKT), reply, &OPS) == n);
    CHECK(sinco_verify(reply, n, &cmd, &p, &len) && p[0] == 4 && p[8] == 120);

    /* write active preset = 4 */
    const u8 wr[9] = {4, 10, 0, 0, 0, 1, 0, 0, 4};
    u32 m = sinco_build(0x22, wr, 9, pkt);
    CHECK(sinco_handle(pkt, m, reply, &OPS) == sizeof(WOK_PKT));
    CHECK_MEM(reply, WOK_PKT, sizeof(WOK_PKT));
    CHECK(mem4[10] == 4 && writes == 1);

    /* Save (zero-length type-5 write) */
    CHECK(sinco_handle(SV_PKT, sizeof(SV_PKT), reply, &OPS) == sizeof(WOK_PKT) && commits == 1);

    /* errors: bad length field, out of range, erase, aux, unknown, bad checksum, too big */
    const u8 wbad[9] = {4, 10, 0, 0, 0, 2, 0, 0, 4};
    m = sinco_build(0x22, wbad, 9, pkt);
    CHECK(sinco_handle(pkt, m, reply, &OPS) == 8 && memcmp(reply, ERR_PKT, 8) == 0);
    const u8 rbad[8] = {4, 8, 0, 0, 0, 8, 0, 0};
    m = sinco_build(0x23, rbad, 8, pkt);
    CHECK(sinco_handle(pkt, m, reply, &OPS) == 8 && memcmp(reply, ERR_PKT, 8) == 0);
    const u8 er[5] = {5, 0, 0, 0, 0};
    m = sinco_build(0x21, er, 5, pkt);
    CHECK(sinco_handle(pkt, m, reply, &OPS) == 8 && memcmp(reply, ERR_PKT, 8) == 0);
    m = sinco_build(0x24, er, 5, pkt);
    CHECK(sinco_handle(pkt, m, reply, &OPS) == 8 && memcmp(reply, ERR_PKT, 8) == 0);
    m = sinco_build(0x7E, NULL, 0, pkt);
    CHECK(sinco_handle(pkt, m, reply, &OPS) == 8 && memcmp(reply, ERR_PKT, 8) == 0);
    memcpy(pkt, RG_PKT, sizeof(RG_PKT));
    pkt[sizeof(RG_PKT) - 1]++;
    CHECK(sinco_handle(pkt, sizeof(RG_PKT), reply, &OPS) == 8 && memcmp(reply, ERR_PKT, 8) == 0);
    const u8 rbig[8] = {4, 0, 0, 0, 0, (SINCO_MAX_DATA + 1) & 0xFF, (SINCO_MAX_DATA + 1) >> 8, 0};
    m = sinco_build(0x23, rbig, 8, pkt);
    CHECK(sinco_handle(pkt, m, reply, &OPS) == 8 && memcmp(reply, ERR_PKT, 8) == 0);

    /* 0x15: MIDI injection, acked like a write (stock) */
    const u8 mi[3] = {0x90, 0x24, 0x7F};
    m = sinco_build(0x15, mi, 3, pkt);
    CHECK(sinco_handle(pkt, m, reply, &OPS) == sizeof(WOK_PKT) && midi_seen_len == 3 && memcmp(midi_seen, mi, 3) == 0);
    CHECK_MEM(reply, WOK_PKT, sizeof(WOK_PKT));

    /* 0x12: 13-byte status, zeros without a callback */
    m = sinco_build(0x12, NULL, 0, pkt);
    n = sinco_handle(pkt, m, reply, &OPS);
    CHECK(n == 6 + 13 + 1 && reply[2] == 0x12 && reply[19] == 0xFF);

    /* long names are cut to 27 bytes */
    struct sinco_ops o2 = OPS;
    o2.name_ver = "0123456789012345678901234567890123";
    CHECK(sinco_handle(Q_PKT, sizeof(Q_PKT), reply, &o2) == 34 && reply[6 + 26] == '6');
}

int main(void)
{
    test_build_verify();
    test_sysex_golden();
    test_sysex_roundtrip();
    test_reassembler();
    test_dispatch();
    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_sinco_proto: all tests passed\n");
    return 0;
}
