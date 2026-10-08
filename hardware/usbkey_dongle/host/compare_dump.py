#!/usr/bin/env python3
"""Compare a physical UBOOT SPI-flash dump of the SMC-PAD with a BLE XIP dump.

Background (see docs/recovery.md, section "Comparing a UBOOT dump with the BLE dump"):
  * jl-uboot-tool reads the SPI flash RAW, as stored on the chip ("physical" bytes).
  * A BLE backup (read type 0) is read through the running firmware's XIP mapping:
      0x00000-0x00FFF  flash header / JLFS entries: physical = ENC(0xFFFF) 16-bit LFSR
                       keystream XOR, restarted every 0x200 bytes
      0x01000-SFC_END  app code area: physical = JieLi SFC 16-bit LFSR keystream XOR
                       (32-byte blocks, seed = chipkey ^ ((off - base) >> 2)); pass the
                       16-bit chipkey reported by jl-uboot-tool ("Chip key: 0x....") via --chipkey
      SFC_END-end      VM / BTIF / EXIF / USRFLASH / USRTRIM / key_mac: plain

Modes (Python 3 stdlib only):
  compare_dump.py UBOOT_DUMP.bin --chipkey 0xXXXX [--ble flash_dump_1.bin] [--out-ble-equiv X.bin]
      convert the physical dump to BLE-equivalent and diff it against the BLE dump
  compare_dump.py --to-physical BLE_DUMP.bin OUT.bin --chipkey 0xXXXX
      predict a physical image from a BLE dump (FALLBACK restore image only; the
      UBOOT dump itself is always the preferred restore source)
  compare_dump.py --selftest
      synthetic round-trip and wrong-key sensitivity self-tests (no device files needed)
"""
import argparse
import hashlib
import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
BLE_DEFAULT = os.path.join(ROOT, 'firmware', 'backup', 'flash_dump_1.bin')
FLASH_SIZE = 0x80000
HDR_END = 0x1000
SFC_BASE = 0x1000
SFC_END = 0x26000

# Header/CRC descriptor ranges where a UFW flash.bin and a runtime BLE read legitimately differ.
KNOWN_DIFF = [
    (0x01000, 0x01004),
    (0x010E0, 0x010E2),
    (0x010ED, 0x010EE),
    (0x01100, 0x01102),
    (0x0110D, 0x0110E),
    (0x25FE0, 0x26000),
]


def enc_lfsr(buf, off, size, key=0xFFFF):
    for i in range(size):
        buf[off + i] ^= key & 0xFF
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xFFFF
    return key


def sfc_lfsr(buf, off, size, base, key, blocksize=32):
    for i in range(0, size, blocksize):
        enc_lfsr(buf, off + i, min(size - i, blocksize), key ^ ((off + i - base) >> 2))


def convert(img, chipkey, sfc_end=SFC_END, header='enc512'):
    """Symmetric physical <-> BLE-equivalent XOR transform."""
    b = bytearray(img)
    if header == 'enc512':
        for off in range(0, min(HDR_END, len(b)), 0x200):
            enc_lfsr(b, off, min(0x200, len(b) - off), 0xFFFF)
    end = min(sfc_end, len(b))
    if end > SFC_BASE:
        sfc_lfsr(b, SFC_BASE, end - SFC_BASE, SFC_BASE, chipkey)
    return bytes(b)


def sha(b):
    return hashlib.sha256(b).hexdigest()


def ranges(a, b, lo=0, hi=None):
    hi = min(len(a), len(b)) if hi is None else hi
    out, cur = [], None
    for i in range(lo, hi):
        if a[i] != b[i]:
            if cur is None:
                cur = i
        elif cur is not None:
            out.append((cur, i))
            cur = None
    if cur is not None:
        out.append((cur, hi))
    return out


def known(s, e):
    return any(ks <= s and e <= ke for ks, ke in KNOWN_DIFF)


def region(off):
    if off < HDR_END:
        return 'header (ENC-512)'
    if off < SFC_END:
        return 'app (SFC)'
    for lo, name in (
        (0x7F000, 'key_mac'),
        (0x7E000, 'USRTRIM'),
        (0x77000, 'USRFLASH'),
        (0x76000, 'EXIF'),
        (0x75000, 'BTIF'),
        (0x26000, 'VM'),
    ):
        if off >= lo:
            return name
    return '?'


def report(diffs, limit=40):
    tot = sum(e - s for s, e in diffs)
    unk = [(s, e) for s, e in diffs if not known(s, e)]
    print(
        '  %d differing range(s), %d byte(s); %d range(s) / %d byte(s) NOT in the known list'
        % (len(diffs), tot, len(unk), sum(e - s for s, e in unk))
    )
    for s, e in diffs[:limit]:
        print(
            '    0x%05X-0x%05X %5d B  %-20s %s'
            % (s, e, e - s, region(s), 'known' if known(s, e) else 'UNEXPECTED')
        )
    if len(diffs) > limit:
        print('    ... %d more' % (len(diffs) - limit))
    return unk


def do_compare(a):
    if a.chipkey is None:
        sys.exit('error: --chipkey 0xXXXX (from jl-uboot-tool output) is required')
    phys = open(a.dump, 'rb').read()
    ble = open(a.ble, 'rb').read()
    print('UBOOT dump : %s  %d B  sha256 %s' % (a.dump, len(phys), sha(phys)))
    print('BLE dump   : %s  %d B  sha256 %s' % (a.ble, len(ble), sha(ble)))
    if len(phys) != FLASH_SIZE:
        print('WARNING: expected 0x%X bytes (512 KiB flash), got 0x%X' % (FLASH_SIZE, len(phys)))
    if phys == ble:
        print('NOTE: dump is byte-identical to the BLE dump -> it is NOT a physical dump?')
    eq = convert(phys, a.chipkey, a.sfc_end, a.header_mode)
    if a.out_ble_equiv:
        open(a.out_ble_equiv, 'wb').write(eq)
        print('wrote BLE-equivalent image %s (sha256 %s)' % (a.out_ble_equiv, sha(eq)))
    n = min(HDR_END, len(phys), len(ble))
    h1 = sum(e - s for s, e in ranges(convert(phys[:n], a.chipkey, header='enc512'), ble, 0, n))
    h2 = sum(e - s for s, e in ranges(phys, ble, 0, n))
    print(
        'header 0x0-0x%X: %d B differ under H1 (ENC-512), %d B under H2 (plain); using --header-mode %s'
        % (n, h1, h2, a.header_mode)
    )
    if (h2 < h1) != (a.header_mode == 'plain'):
        print(
            '  HINT: the other header mode fits better; re-run with --header-mode %s'
            % ('plain' if h2 < h1 else 'enc512')
        )
    print('BLE-equivalent vs BLE dump:')
    unk = report(ranges(eq, ble))
    if eq == ble:
        print('RESULT: IDENTICAL (sha256 %s)' % sha(eq))
        return 0
    if not unk:
        print('RESULT: MATCH except the known header/CRC bytes')
        return 0
    app_unk = [r for r in unk if r[0] < a.sfc_end]
    if app_unk:
        print(
            'RESULT: MISMATCH in header/app area. Verify --chipkey (UBOOT "Chip key" output) '
            'and --sfc-end before concluding anything.'
        )
    else:
        print(
            'RESULT: header/app match; differences only in the plain data areas '
            '(VM etc. change at runtime)'
        )
    return 1


def do_to_physical(a):
    if a.chipkey is None:
        sys.exit('error: --chipkey 0xXXXX (from jl-uboot-tool output) is required')
    ble = open(a.to_physical[0], 'rb').read()
    phys = convert(ble, a.chipkey, a.sfc_end, a.header_mode)
    open(a.to_physical[1], 'wb').write(phys)
    print('wrote predicted physical image %s (sha256 %s)' % (a.to_physical[1], sha(phys)))
    print(
        'NOTE: the %d header/CRC bytes (%s) may differ in a synthetic prediction; '
        'always prefer a direct UBOOT dump as your restore source.'
        % (sum(e - s for s, e in KNOWN_DIFF), ', '.join('0x%X-0x%X' % r for r in KNOWN_DIFF))
    )
    return 0


def do_selftest():
    fails = []

    def ok(c, what):
        print('  [%s] %s' % ('PASS' if c else 'FAIL', what))
        if not c:
            fails.append(what)

    # Synthetic 512 KiB test image (no external files or device keys needed)
    sample = bytes((i * 73 + (i >> 8) * 19) & 0xFF for i in range(FLASH_SIZE))
    test_key = 0x1234
    phys = convert(sample, chipkey=test_key)
    ok(convert(phys, chipkey=test_key) == sample, 'convert() is an involution (XIP -> physical -> XIP)')
    ok(phys[SFC_END:] == sample[SFC_END:], 'plain data regions >= SFC_END are untouched')
    wrong = convert(phys, chipkey=0xFFFF)
    diff_bytes = sum(e - s for s, e in ranges(wrong, sample))
    ok(diff_bytes > 100000, 'mismatched --chipkey is detected (%d bytes differ)' % diff_bytes)
    print('SELFTEST %s' % ('FAILED' if fails else 'PASSED'))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dump', nargs='?', help='physical dump from jl-uboot-tool (read 0 0x80000)')
    ap.add_argument('--ble', default=BLE_DEFAULT, help='BLE dump (default: %(default)s)')
    ap.add_argument('--chipkey', type=lambda s: int(s, 0), default=None, help='16-bit chipkey reported by UBOOT (e.g. 0x1234)')
    ap.add_argument('--sfc-end', type=lambda s: int(s, 0), default=SFC_END)
    ap.add_argument(
        '--header-mode',
        choices=('enc512', 'plain'),
        default='enc512',
        help='0x0-0xFFF: physical = ENC-512(BLE) (default) or physical = BLE as-is',
    )
    ap.add_argument('--out-ble-equiv')
    ap.add_argument('--to-physical', nargs=2, metavar=('BLE_DUMP', 'OUT'))
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return do_selftest()
    if a.to_physical:
        return do_to_physical(a)
    if not a.dump:
        ap.error('give a UBOOT dump, --to-physical or --selftest')
    return do_compare(a)


if __name__ == '__main__':
    sys.exit(main())
