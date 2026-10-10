/* Scales for the sequencer (pure C, host-tested). A scale is a 12-bit mask of semitones above
 * the root (bit 0 = root). Offsets are counted in scale steps from a base note; offset 0 is
 * always the base note itself, so drum tracks (all offsets 0) are never changed by the scale. */
#ifndef SMCPAD_SCALE_H
#define SMCPAD_SCALE_H

#ifdef SMCPAD_HOST_TEST
#include <stdint.h>
typedef uint8_t  u8;
typedef int8_t   s8;
typedef uint16_t u16;
#else
#include "typedef.h"
#endif

enum { SCALE_CHROMATIC = 0, SCALE_MAJOR, SCALE_MINOR, SCALE_DORIAN, SCALE_PHRYGIAN, SCALE_LYDIAN,
       SCALE_MIXOLYDIAN, SCALE_LOCRIAN, SCALE_HARM_MINOR, SCALE_PENTA_MAJOR, SCALE_PENTA_MINOR,
       SCALE_BLUES, SCALE_COUNT };

u16 smcpad_scale_mask(u8 scale);                          /* semitones above the root */
u8  smcpad_scale_in(u8 note, u8 root, u8 scale);         /* 1 if note is in the scale */
u8  smcpad_scale_note(u8 base, s8 off, u8 root, u8 scale);/* base moved by off scale steps */
u8  smcpad_scale_size(u8 scale);                         /* notes per octave */

#endif
