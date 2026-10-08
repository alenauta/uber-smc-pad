#!/usr/bin/env python3
"""Check a logic-analyzer capture of the USB_KEY dongle (Python 3 stdlib only).

Input: a VCD file, e.g. PulseView "File > Export > Value Change Dump", or
`sigrok-cli -d fx2lafw -c samplerate=4m --time 2s -C D0=dp,D1=dm -O vcd -o cap.vcd`,
or `host/pio_model.py --vcd cap.vcd` (used by --selftest). Probe at the PAD connector
(after R1/R2), with the 15k dummy load or the chip simulator attached.

What is checked (see ../README.md, "Bench test plan"):
  key  : D+ rising edges grouped into packets; every complete packet decodes to
         0x16EF MSB first (D- sampled at the D+ rising edge); bit period, clock-high
         time, D- setup/hold around the rising edge, packet repeat
  ack  : after a packet, D+ AND D- both low for ACK_MIN..ACK_MAX us (ROM: ~2 ms)
  sof  : falling edges on D+ after the ACK spaced 1000 us (+-SOF_TOL us) apart
         (only with config.SOF_EMULATION = True, or the chip simulator logging)

Usage:
  check_capture.py cap.vcd [--dp NAME] [--dm NAME] [--expect key,ack,sof] [--bit-us 20]
  check_capture.py --selftest      # runs pio_model.py --vcd into a temp file and checks it
Exit code 0 = all requested checks passed.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

KEY = 0x16EF
ACK_MIN_US, ACK_MAX_US = 500, 5000
SOF_PERIOD_US, SOF_TOL_US = 1000, 5
UNITS = {'s': 1e6, 'ms': 1e3, 'us': 1.0, 'ns': 1e-3, 'ps': 1e-6, 'fs': 1e-9}


def parse_vcd(path, want):
    """Return {name: [(t_us, value), ...]} for the 1-bit signals named in `want`."""
    scale, ids, sig, t = 1.0, {}, {}, 0.0
    text = open(path, encoding='ascii', errors='replace').read()
    m = re.search(r'\$timescale\s+(\d+)\s*(s|ms|us|ns|ps|fs)\s+\$end', text)
    if m:
        scale = int(m.group(1)) * UNITS[m.group(2)]
    for m in re.finditer(r'\$var\s+\S+\s+1\s+(\S+)\s+(\S+)(?:\s+\[\d+\])?\s+\$end', text):
        if m.group(2) in want:
            ids[m.group(1)] = m.group(2)
    missing = set(want) - set(ids.values())
    if missing:
        names = re.findall(r'\$var\s+\S+\s+\d+\s+\S+\s+(\S+)', text)
        raise SystemExit('signal(s) %s not in %s; available: %s'
                         % (sorted(missing), path, ', '.join(names)))
    body = text.split('$enddefinitions', 1)[1].split('$end', 1)[1]
    for tok in body.split():
        if tok[0] == '#':
            t = int(tok[1:]) * scale
        elif tok[0] in '01xXzZ' and tok[1:] in ids:
            v = 1 if tok[0] == '1' else 0
            s = sig.setdefault(ids[tok[1:]], [])
            if not s or s[-1][1] != v:
                s.append((t, v))
    return sig


def level_at(trace, t):
    """Value of a signal at time t (last change <= t)."""
    lo, hi = 0, len(trace) - 1
    if not trace or t < trace[0][0]:
        return None
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if trace[mid][0] <= t:
            lo = mid
        else:
            hi = mid - 1
    return trace[lo][1]


def edges(trace, rising):
    return [t for (t, v), (_, pv) in zip(trace[1:], trace[:-1]) if v == (1 if rising else 0)
            and pv != v]


class Checker:
    def __init__(self):
        self.fails = []

    def ok(self, cond, what):
        print('  [%s] %s' % ('PASS' if cond else 'FAIL', what))
        if not cond:
            self.fails.append(what)


def check_key(ck, dp, dm, bit_us):
    print('key packets (D+ = clock, D- = data, MSB first, latched on the D+ rising edge)')
    rises, falls, dmch = edges(dp, True), edges(dp, False), [t for t, _ in dm[1:]]
    packets, cur = [], []
    for t in rises:
        if cur and t - cur[-1] > 1.5 * bit_us:
            packets.append(cur)
            cur = []
        cur.append(t)
    if cur:
        packets.append(cur)
    leadins = 0
    for k, p in enumerate(packets):              # see pio_model.decode(): harmless lead-in edge
        if len(p) == 17 and p[1] - p[0] < bit_us:
            packets[k] = p[1:]
            leadins += 1
    full = [p for p in packets if len(p) == 16]
    vals = {}
    for p in full:
        v = 0
        for t in p:
            v = (v << 1) | (level_at(dm, t + 0.01) or 0)
        vals[v] = vals.get(v, 0) + 1
    odd = [len(p) for p in packets if len(p) != 16]
    ck.ok(len(full) >= 3, '%d complete 16-bit packets (+%d other groups %s, %d lead-in edge(s))'
          % (len(full), len(odd), odd[:5], leadins))
    if not full:
        return []
    ck.ok(set(vals) == {KEY}, 'decoded values %s (expect 0x%04X)'
          % (', '.join('0x%04X x%d' % kv for kv in sorted(vals.items())), KEY))
    per = [b - a for p in full for a, b in zip(p, p[1:])]
    ck.ok(all(abs(x - bit_us) <= 0.1 * bit_us for x in per),
          'bit period %.2f..%.2f us (expect %d +-10%%)' % (min(per), max(per), bit_us))
    dm_rises = edges(dm, True)

    def acked(f):
        """True if the D+ falling edge at f is the start of the chip's ACK (both lines
        then stay low for >= ~0.8 * ACK_MIN_US): that last bit is cut short on purpose."""
        late = f + 0.8 * ACK_MIN_US
        if level_at(dm, f + 0.01) != 0:
            return False
        nxt_dp = next((x for x in rises if x > f), None)
        nxt_dm = next((x for x in dm_rises if x > f), None)
        return all(x is None or x > late for x in (nxt_dp, nxt_dm))

    highs, cut = [], set()
    for p in full:
        for t in p:
            f = next((x for x in falls if x > t), None)
            if f is not None and f - t < 1.5 * bit_us:
                if acked(f):
                    cut.add(t)
                    continue
                highs.append(f - t)
    if cut:
        print('  info: %d bit(s) cut short by an ACK (excluded from high/hold timing)' % len(cut))
    if highs:
        ck.ok(min(highs) >= 0.35 * bit_us, 'clock high time %.2f..%.2f us (expect ~%g)'
              % (min(highs), max(highs), bit_us / 2))
    setup = hold = 1e9
    for p in full:
        for t in p:
            before = [c for c in dmch if c <= t]
            after = [c for c in dmch if c > t]
            if before:
                setup = min(setup, t - before[-1])
            if after and t not in cut:
                hold = min(hold, after[0] - t)
    ck.ok(setup >= 0.25 * bit_us and hold >= 0.25 * bit_us,
          'D- setup %.2f us / hold %.2f us around D+ rising edges (need >= %.1f)'
          % (setup, hold, 0.25 * bit_us))
    if len(full) > 1:
        rep = [b[0] - a[0] for a, b in zip(full, full[1:])]
        print('  info: packet repeat %.0f..%.0f us' % (min(rep), max(rep)))
    return full


def find_acks(dp, dm, after):
    """Intervals where D+ and D- are both low, starting after time `after`."""
    times = sorted({t for t, _ in dp} | {t for t, _ in dm})
    out, start = [], None
    for t in times:
        both_low = level_at(dp, t) == 0 and level_at(dm, t) == 0
        if both_low and start is None:
            start = t
        elif not both_low and start is not None:
            if start >= after:
                out.append((start, t - start))
            start = None
    return out


def check_ack(ck, dp, dm, full):
    print('ACK (chip drives D+ and D- low ~2 ms right after a matching packet)')
    after = full[0][0] if full else 0
    lows = [(t, d) for t, d in find_acks(dp, dm, after) if ACK_MIN_US <= d <= ACK_MAX_US]
    ck.ok(len(lows) >= 1, '%d ACK-like low(s): %s' % (len(lows), ', '.join(
        '%.3f ms long at t=%.3f ms' % (d / 1e3, t / 1e3) for t, d in lows[:3])))
    if lows and full:
        last = max((p[-1] for p in full if p[-1] <= lows[0][0]), default=None)
        if last is not None:
            ck.ok(lows[0][0] - last < 200, 'ACK starts %.1f us after the last key edge (< 200)'
                  % (lows[0][0] - last))
        later = [p for p in full if p[0] > lows[0][0] + lows[0][1]]
        ck.ok(not later, 'no key packets after the ACK (%d found)' % len(later))
    return lows


def check_sof(ck, dp, lows):
    print('SOF pulses after the ACK (falling edges on D+, 1.000 ms apart)')
    t0 = lows[0][0] + lows[0][1] if lows else 0
    f = [t for t in edges(dp, False) if t > t0]
    iv = [b - a for a, b in zip(f, f[1:])]
    good = [x for x in iv if abs(x - SOF_PERIOD_US) <= SOF_TOL_US]
    ck.ok(len(good) >= 4, '%d of %d falling-edge intervals within %d+-%d us (ROM needs 4 in a row)'
          % (len(good), len(iv), SOF_PERIOD_US, SOF_TOL_US))
    if iv:
        print('  info: first intervals (us): %s' % ', '.join('%.1f' % x for x in iv[:12]))


def analyse(path, dpn, dmn, expect, bit_us):
    sig = parse_vcd(path, {dpn, dmn})
    dp, dm = sig.get(dpn, []), sig.get(dmn, [])
    print('%s: %s %d transitions, %s %d transitions, span %.3f ms'
          % (path, dpn, len(dp), dmn, len(dm),
             (max(dp[-1][0], dm[-1][0]) - min(dp[0][0], dm[0][0])) / 1e3 if dp and dm else 0))
    ck = Checker()
    full = check_key(ck, dp, dm, bit_us) if 'key' in expect or 'ack' in expect else []
    lows = check_ack(ck, dp, dm, full) if 'ack' in expect else []
    if 'sof' in expect:
        check_sof(ck, dp, lows)
    print('%s: %d check(s) failed' % ('ALL PASSED' if not ck.fails else 'FAILED', len(ck.fails)))
    return 0 if not ck.fails else 1


def selftest():
    model = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pio_model.py')
    with tempfile.TemporaryDirectory() as d:
        vcd = os.path.join(d, 'normal.vcd')
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
        r = subprocess.run([sys.executable, model, '--vcd', vcd], env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode != 0 or not os.path.exists(vcd):
            print(r.stdout[-2000:])
            print('SELFTEST FAILED: pio_model.py did not produce a VCD')
            return 1
        print('selftest: pio_model.py "normal" scenario (VBUS switched by K2)')
        return analyse(vcd, 'dp', 'dm', {'key', 'ack'}, 20)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('vcd', nargs='?')
    ap.add_argument('--dp', default='dp', help='VCD name of the D+ channel (default %(default)s)')
    ap.add_argument('--dm', default='dm', help='VCD name of the D- channel (default %(default)s)')
    ap.add_argument('--expect', default='key', help='comma list of key,ack,sof (default key)')
    ap.add_argument('--bit-us', type=float, default=20, help='config.BIT_US (default 20)')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.vcd:
        ap.error('need a VCD file (or --selftest)')
    return analyse(a.vcd, a.dp, a.dm, set(a.expect.split(',')), a.bit_us)


if __name__ == '__main__':
    sys.exit(main())
