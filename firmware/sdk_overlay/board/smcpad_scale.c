#include "smcpad_scale.h"

#define M(a) (1u << (a))
static const u16 scale_masks[SCALE_COUNT] = {
    0x0FFF,                                                 /* chromatic */
    M(0) | M(2) | M(4) | M(5) | M(7) | M(9) | M(11),        /* major */
    M(0) | M(2) | M(3) | M(5) | M(7) | M(8) | M(10),        /* natural minor */
    M(0) | M(2) | M(3) | M(5) | M(7) | M(9) | M(10),        /* dorian */
    M(0) | M(1) | M(3) | M(5) | M(7) | M(8) | M(10),        /* phrygian */
    M(0) | M(2) | M(4) | M(6) | M(7) | M(9) | M(11),        /* lydian */
    M(0) | M(2) | M(4) | M(5) | M(7) | M(9) | M(10),        /* mixolydian */
    M(0) | M(1) | M(3) | M(5) | M(6) | M(8) | M(10),        /* locrian */
    M(0) | M(2) | M(3) | M(5) | M(7) | M(8) | M(11),        /* harmonic minor */
    M(0) | M(2) | M(4) | M(7) | M(9),                       /* pentatonic major */
    M(0) | M(3) | M(5) | M(7) | M(10),                      /* pentatonic minor */
    M(0) | M(3) | M(5) | M(6) | M(7) | M(10),               /* blues */
};
#undef M

u16 smcpad_scale_mask(u8 scale)
{
    return scale < SCALE_COUNT ? scale_masks[scale] : scale_masks[SCALE_CHROMATIC];
}

u8 smcpad_scale_in(u8 note, u8 root, u8 scale)
{
    u8 deg = (u8)((note + 12 - (root % 12)) % 12);
    return (u8)((smcpad_scale_mask(scale) >> deg) & 1);
}

u8 smcpad_scale_size(u8 scale)
{
    u16 m = smcpad_scale_mask(scale);
    u8 n = 0;
    for (u8 i = 0; i < 12; i++) {
        n = (u8)(n + ((m >> i) & 1));
    }
    return n;
}

u8 smcpad_scale_note(u8 base, s8 off, u8 root, u8 scale)
{
    int n = base, dir = off < 0 ? -1 : 1, k = off < 0 ? -off : off;
    while (k-- > 0) {
        do {
            n += dir;
        } while (n > 0 && n < 127 && !smcpad_scale_in((u8)n, root, scale));
        if (n <= 0 || n >= 127) {
            break;
        }
    }
    return (u8)(n < 0 ? 0 : (n > 127 ? 127 : n));
}
