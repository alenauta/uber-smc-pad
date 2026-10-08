#!/usr/bin/env python3
"""On-paper test of the dongle firmware (CPython 3, stdlib only, nothing to install).

Loads ../firmware/usbkey.py with fake `rp2` / `machine` / `time` modules, assembles
the REAL `key_tx` and `sof_pulses` PIO programs, and runs the REAL Python code
(usbkey.run(), waveform(), sof_test(), AckLogic) against:

  * a small cycle-accurate PIO interpreter (1 SM cycle = 1 us, i.e. BIT_US = 20),
  * a resistive model of the wiring (R1/R2 1k series, R3/R4 2k2 pull-ups, optional
    R5 330R SOF path, relays K1/K2 with 5 ms operate time),
  * a behavioural model of the BR25 mask-ROM key receiver, inferred from the ROM
    listing (cpu/br25/tools/rom.lst: main, usb_key_isr, set_cap_timeout,
    check_usb_sof, tmr3_isr) and kagaimiq/jielie isp/usb/usb-key.md:
      power-on -> BOOT_US -> key window WINDOW_US (~10 ms) during which every D+
      rising edge shifts D- into a 16-bit register (init 0xFFFF); == 0x16EF ->
      drive D+ and D- low for 2 ms (ACK) -> D+ pull-up (1.5k) on, calibrate on
      four 1 ms SOF periods -> UBOOT. No key in the window -> normal firmware.

Checks (exit code 1 if any fails):
  1. electrical levels (why 2k2 pull-ups, why the SOF path needs R5)
  2. waveform: 0x16EF MSB first on D-, latched on D+ rising edges, 20 us bits,
     data setup/hold >= 9 us around every rising edge
  3. end-to-end scenarios through usbkey.run(): VBUS-switched pad, slow Python,
     ISR-latency sweep, chip never powered, nothing attached, battery-powered pad
     (with/without the user toggling its power switch), no VBUS relay fitted,
     long boot delay, SOF emulation with/without R5
  4. sof_pulses: period 1000 us, low time 2 us
  --vcd FILE writes the 'normal' scenario (PulseView / GTKWave).

Usage:  python3 pio_model.py [-v] [--vcd /path/usbkey.vcd]
Model limits: the ROM behaviour is INFERRED from the public BR25 mask-ROM listing (window length, ISR
latency, thresholds, SOF tolerance are assumptions); the electrical model is
resistive only (no capacitance, no ringing); MicroPython call latency is a guess
(PY_US per fake call). It proves the firmware logic and timing are self-consistent,
not that the real chip behaves like the model.
"""
import argparse
import contextlib
import io
import os
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.normpath(os.path.join(HERE, '..', 'firmware'))

# --------------------------------------------------------------------------------
# Electrical constants
# --------------------------------------------------------------------------------
VDD = 3.3
R_SERIES = 1000.0       # R1/R2: Pico line pin -> pad line
R_PU = 2200.0           # R3/R4: pull-up pin -> pad line
R_SOF = 330.0           # R5: optional SOF pin -> pad D+
R_CHIP_PD = 15000.0     # BR25 pull-downs while the ROM listens (usb-key.md)
R_CHIP_DRV = 30.0       # chip output driver (ACK)
R_CHIP_DPU = 1500.0     # USB FS D+ pull-up enabled by the ROM after the ACK
R_HOST_PD = 15000.0     # host/hub downstream pull-downs
R_PICO_INT_PU = 50000.0 # RP2040 internal pull-up (datasheet: 50..80k)
V_CLAMP, R_CLAMP = 0.6, 100.0   # unpowered chip: ESD diode to a 0 V rail (approx.)
PICO_VIH, PICO_VIL = 2.0, 0.8   # RP2040 datasheet, IOVDD = 3.3 V
CHIP_VIH, CHIP_VIL = 2.0, 0.8   # ASSUMED (no BR25 electrical datasheet available)
FIFO_DEPTH = 4
RELAY_US = 5000
SOF_TOL_US = 20         # ASSUMED tolerance for the ROM's SOF period check
SOF_GIVEUP_US = 3 * 1_200_000

CUR = None              # the running Sim


def par(*branches):
    """Thevenin node voltage of (V, R) branches; None if floating."""
    g = sum(1.0 / r for _, r in branches)
    if not g:
        return None
    return sum(v / r for v, r in branches) / g


# --------------------------------------------------------------------------------
# Fake rp2 assembler (the subset of MicroPython's rp2.asm_pio used here)
# --------------------------------------------------------------------------------
class Ins:
    def __init__(self, op, *args):
        self.op, self.args, self.sidev, self.dly = op, args, None, 0

    def side(self, v):
        self.sidev = v
        return self

    def __getitem__(self, d):
        self.dly = d
        return self


class Prog:
    def __init__(self, kw):
        self.kw = kw
        self.ins, self.labels, self.wrap_target, self.wrap = [], {}, 0, None

    def builders(self):
        P = self

        def mk(op):
            def f(*a):
                i = Ins(op, *a)
                P.ins.append(i)
                return i
            return f
        g = {n: mk(n) for n in ('pull', 'push', 'mov', 'set', 'out', 'in_', 'jmp', 'nop',
                                'wait', 'irq')}

        def label(name):
            P.labels[name] = len(P.ins)

        def wrap_target():
            P.wrap_target = len(P.ins)

        def wrap():
            P.wrap = len(P.ins) - 1
        g.update(label=label, wrap_target=wrap_target, wrap=wrap, invert=lambda v: ('inv', v))
        for t in ('pins', 'pindirs', 'x', 'y', 'osr', 'isr', 'null', 'pc', 'x_dec', 'y_dec',
                  'not_x', 'not_y', 'pin', 'gpio', 'not_osre', 'x_not_y', 'block', 'noblock'):
            g[t] = t
        return g


class FakePIO:
    IN_LOW, IN_HIGH, OUT_LOW, OUT_HIGH = 0, 1, 2, 3
    SHIFT_LEFT, SHIFT_RIGHT = 0, 1


def asm_pio(**kw):
    def dec(f):
        p = Prog(kw)
        g = dict(f.__globals__)
        g.update(p.builders())
        types.FunctionType(f.__code__, g)()
        if p.wrap is None:
            p.wrap = len(p.ins) - 1
        return p
    return dec


# --------------------------------------------------------------------------------
# PIO state machine interpreter
# --------------------------------------------------------------------------------
class SM:
    def __init__(self, sim, prog, set_base=None, out_base=None, side_base=None,
                 in_base=None, jmp_pin=None):
        self.sim, self.p = sim, prog
        kw = prog.kw
        si = kw.get('set_init', ())
        self.set_count = len(si) if isinstance(si, tuple) else 1
        self.side_base = side_base if 'sideset_init' in kw else None
        self.set_base, self.out_base = set_base, out_base
        self.in_base, self.jmp_pin = in_base, jmp_pin
        assert kw.get('out_shiftdir', 0) == FakePIO.SHIFT_LEFT
        assert kw.get('in_shiftdir', 0) == FakePIO.SHIFT_LEFT
        self.pc = 0
        self.x = self.y = self.isr = self.osr = 0
        self.osr_cnt = 32
        self.delay = 0
        self.tx, self.rx = [], []
        self.active = False
        # MicroPython applies *_init to the pins when the SM is constructed
        for init, base in (('set_init', set_base), ('out_init', out_base),
                           ('sideset_init', self.side_base)):
            v = kw.get(init)
            if v is None or base is None:
                continue
            for k, s in enumerate(v if isinstance(v, tuple) else (v,)):
                p = sim.pin(base + k)
                p[0] = 1 if s in (FakePIO.IN_HIGH, FakePIO.OUT_HIGH) else 0
                p[1] = 1 if s in (FakePIO.OUT_LOW, FakePIO.OUT_HIGH) else 0

    def _src(self, s):
        if isinstance(s, tuple) and s[0] == 'inv':
            return (~self._src(s[1])) & 0xFFFFFFFF
        if s == 'pins':
            return self.sim.read_pins(self.in_base, 32)
        return {'x': self.x, 'y': self.y, 'isr': self.isr, 'osr': self.osr, 'null': 0}[s]

    def _write(self, base, n, v, idx):
        for k in range(n):
            self.sim.pin(base + k)[idx] = (v >> k) & 1

    def step(self):
        if not self.active:
            return
        if self.delay:
            self.delay -= 1
            return
        i = self.p.ins[self.pc]
        op, a = i.op, i.args
        nxt = self.p.wrap_target if self.pc == self.p.wrap else self.pc + 1
        stall = False
        if op == 'pull':
            blk = (a[0] if a else 'block') == 'block'
            if self.tx:
                self.osr, self.osr_cnt = self.tx.pop(0), 0
            elif blk:
                stall = True
            else:
                self.osr, self.osr_cnt = self.x, 0
        elif op == 'push':
            blk = (a[0] if a else 'block') == 'block'
            if len(self.rx) < FIFO_DEPTH:
                self.rx.append(self.isr)
                self.isr = 0
            elif blk:
                stall = True
            else:
                self.isr = 0                    # data lost
        elif op == 'mov':
            dst, src = a
            v = self._src(src)
            if dst == 'osr':
                self.osr, self.osr_cnt = v, 0
            else:
                setattr(self, dst, v)
        elif op == 'set':
            dst, v = a
            if dst == 'pins':
                self._write(self.set_base, self.set_count, v, 0)
            elif dst == 'pindirs':
                self._write(self.set_base, self.set_count, v, 1)
            else:
                setattr(self, dst, v)
        elif op == 'out':
            dst, n = a
            v = self.osr >> (32 - n)
            self.osr = (self.osr << n) & 0xFFFFFFFF
            self.osr_cnt += n
            assert dst == 'pins'
            self._write(self.out_base, n, v, 0)
        elif op == 'in_':
            src, n = a
            assert src == 'pins'
            v = self.sim.read_pins(self.in_base, n)
            self.isr = ((self.isr << n) | v) & 0xFFFFFFFF
        elif op == 'jmp':
            cond, tgt = (None, a[0]) if len(a) == 1 else a
            if cond is None:
                take = True
            elif cond == 'not_x':
                take = self.x == 0
            elif cond == 'not_y':
                take = self.y == 0
            elif cond == 'x_dec':
                take = self.x != 0
                self.x = (self.x - 1) & 0xFFFFFFFF
            elif cond == 'y_dec':
                take = self.y != 0
                self.y = (self.y - 1) & 0xFFFFFFFF
            elif cond == 'pin':
                take = self.sim.read_pins(self.jmp_pin, 1) == 1
            elif cond == 'x_not_y':
                take = self.x != self.y
            else:
                raise NotImplementedError(cond)
            if take:
                nxt = self.p.labels[tgt]
        elif op == 'nop':
            pass
        else:
            raise NotImplementedError(op)
        # side-set acts even on a stalled instruction and wins over OUT/SET on the same pin
        if i.sidev is not None and self.side_base is not None:
            self.sim.pin(self.side_base)[0] = i.sidev & 1
        if stall:
            return
        self.pc = nxt
        self.delay = i.dly


# --------------------------------------------------------------------------------
# BR25 mask-ROM model
# --------------------------------------------------------------------------------
class Chip:
    def __init__(self, sim, sc):
        self.sim, self.sc = sim, sc
        self.state = 'off'
        self.t_next = None
        self.pending = []
        self.dp_prev = 0
        self.reg = 0xFFFF
        self.bits = 0
        self.edges = []
        self.cal_by = None
        self.history = []

    def go(self, st):
        self.state = st
        self.history.append((self.sim.t, st))

    def powered(self):
        sc, sim = self.sc, self.sim
        if sc.never_powered:
            return False
        p = sc.vbus_powered and sim.vbus_out()
        if sc.battery is not None:
            p = p or sc.battery_on(sim.t)
        return p

    def loads(self, line):
        st = self.state
        if st == 'off':
            return [(V_CLAMP, R_CLAMP)]
        if st == 'ack':
            return [(0.0, R_CHIP_DRV)]
        if st in ('sof', 'uboot', 'app'):
            return [(VDD, R_CHIP_DPU)] if line == 0 else []
        return [(0.0, R_CHIP_PD)]                 # boot / window

    def next_event(self):
        t = self.sim.t
        c = [self.sc.next_power_event(t)]
        if self.state in ('boot', 'window', 'ack'):
            c.append(self.t_next)
        if self.pending:
            c.append(self.pending[0])
        if self.state == 'sof':
            c.append(self.sof_t0 + SOF_GIVEUP_US)
            if self.sim.k1 == 0 and self.sc.host:
                c.append((t // 1000 + 1) * 1000)
        return min(x for x in c if x is not None and x > t) if any(
            x is not None and x > t for x in c) else None

    def _sof_edge(self, t):
        self.edges.append(t)
        e = self.edges[-5:]
        if len(e) == 5 and all(abs((e[k + 1] - e[k]) - 1000) <= SOF_TOL_US for k in range(4)):
            self.cal_by = 'pico' if self.sim.k1 else 'host'
            self.go('uboot')

    def update(self, dp, dm):
        t, sc = self.sim.t, self.sc
        if not self.powered():
            if self.state != 'off':
                self.go('off')
                self.pending = []
            self.dp_prev = dp
            return
        st = self.state
        if st == 'off':
            self.reg, self.bits = 0xFFFF, 0
            self.go('boot')
            self.t_next = t + sc.boot_us
        elif st == 'boot':
            if sc.receiver and t >= self.t_next:
                self.go('window')
                self.t_next = t + sc.window_us
        elif st == 'window':
            if dp and not self.dp_prev:
                self.pending.append(t + sc.isr_lat_us)
            while self.pending and t >= self.pending[0]:
                self.pending.pop(0)
                self.reg = ((self.reg << 1) | dm) & 0xFFFF
                self.bits += 1
                if self.reg == 0x16EF:
                    self.go('ack')
                    self.t_next = t + sc.ack_us
                    self.pending = []
                    break
            if self.state == 'window' and t >= self.t_next:
                self.go('app')
                self.pending = []
        elif st == 'ack':
            if t >= self.t_next:
                self.go('sof')
                self.sof_t0 = t
                self.edges = []
        elif st == 'sof':
            if self.sim.k1 == 0 and sc.host:
                if t % 1000 == 0:
                    self._sof_edge(t)       # the host's SOFs (ideal, quiet bus)
            elif self.dp_prev and not dp:
                self._sof_edge(t)
            if self.state == 'sof' and t - self.sof_t0 > SOF_GIVEUP_US:
                self.go('app')
        self.dp_prev = dp


# --------------------------------------------------------------------------------
# Scenario + simulator
# --------------------------------------------------------------------------------
class Scenario:
    def __init__(self, name, expect, chip=True, vbus_powered=True, never_powered=False,
                 battery=None, user_toggle=False, user_plug=False, receiver=True,
                 boot_us=2000, window_us=10000, isr_lat_us=2, ack_us=2000, host=True,
                 py_us=5, cfg=None, expect_cal=None):
        self.__dict__.update(locals())
        del self.__dict__['self']
        self.power_events = []          # [(t, on)] for the battery / manual-plug supply

    def battery_on(self, t):
        on = self.battery
        for te, v in self.power_events:
            if t >= te:
                on = v
        return on

    def next_power_event(self, t):
        f = [te for te, _ in self.power_events if te > t]
        return min(f) if f else None

    def on_log(self, sim, msg):
        if self.user_toggle and 'power switch OFF->ON' in msg:
            self.power_events += [(sim.t + 30_000, False), (sim.t + 80_000, True)]
        if self.user_plug and 'POWER THE PAD NOW' in msg:
            self.power_events += [(sim.t + 50_000, True)]


class Sim:
    def __init__(self, sc, C, record=False):
        self.sc, self.C = sc, C
        self.t = 0
        self.gpio = {}
        self.sms = {}
        self.k1 = self.k2 = 0
        self.relay_q = []
        self.chip = Chip(self, sc) if sc.chip else None
        self.lv = {'dp': 0, 'dm': 0}           # chip-side logic (hysteresis)
        self.pv = {}                           # Pico-side logic per pin (hysteresis)
        self.undef_pico = 0
        self.record = record
        self.trace = []
        self.sof_dir = []
        self.max_backpower_ma = 0.0
        self.drove_while_handed = 0

    # ---- pins -------------------------------------------------------------------
    def pin(self, n):
        return self.gpio.setdefault(n, [0, 0])

    def vbus_out(self):
        return self.k2 == 0                    # K2 NC contact passes VBUS

    def pico_branches(self, line):
        C = self.C
        b = []
        p = self.pin(C.PIN_BASE + line)
        if p[1]:
            b.append((VDD * p[0], R_SERIES))
        q = self.pin(C.PIN_BASE + 2 + line)
        if q[1]:
            b.append((VDD * q[0], R_PU))
        if line == 0 and getattr(C, 'PIN_SOF', None) is not None:
            s = self.pin(C.PIN_SOF)
            if s[1]:
                b.append((VDD * s[0], R_SOF))
        return b

    def node(self, line, pico_side=False):
        b = []
        if self.k1 or pico_side:
            b += self.pico_branches(line)
        if pico_side and not self.k1:
            return par(*b)
        if not self.k1 and self.sc.host:
            b.append((0.0, R_HOST_PD))
        if self.chip:
            b += self.chip.loads(line)
        return par(*b)

    def read_pins(self, base, n):
        v = 0
        for k in range(n):
            v |= self.read_pin(base + k) << k
        return v

    def read_pin(self, n):
        C = self.C
        p = self.pin(n)
        if p[1]:
            return p[0]
        if n in (C.PIN_BASE, C.PIN_BASE + 1):
            V = self.node(n - C.PIN_BASE, pico_side=True)
            prev = self.pv.get(n, 0)
            if V is None:
                return 0
            if V >= PICO_VIH:
                lv = 1
            elif V <= PICO_VIL:
                lv = 0
            else:
                self.undef_pico += 1
                lv = prev
            self.pv[n] = lv
            return lv
        return 0

    def gpio_in(self):
        v = 0
        for n in list(self.gpio) + [self.C.PIN_BASE, self.C.PIN_BASE + 1]:
            if isinstance(n, int):
                v |= self.read_pin(n) << n
        return v

    def set_relay(self, which, val):
        self.relay_q.append((self.t + RELAY_US, which, val))

    # ---- time -------------------------------------------------------------------
    def _chip_level(self, key, V):
        if V is None:
            lv = 0
        elif V >= CHIP_VIH:
            lv = 1
        elif V <= CHIP_VIL:
            lv = 0
        else:
            lv = self.lv[key]
        self.lv[key] = lv
        return lv

    def cycle(self):
        for t_, w, v in [r for r in self.relay_q if r[0] <= self.t]:
            setattr(self, w, v)
        self.relay_q = [r for r in self.relay_q if r[0] > self.t]
        for sm in self.sms.values():
            sm.step()
        Vp, Vm = self.node(0), self.node(1)
        dp, dm = self._chip_level('dp', Vp), self._chip_level('dm', Vm)
        if self.chip:
            self.chip.update(dp, dm)
            if self.chip.state == 'off' and self.k1:
                for line in (0, 1):
                    for v, r in self.pico_branches(line):
                        if v > V_CLAMP:
                            self.max_backpower_ma = max(self.max_backpower_ma,
                                                        (v - V_CLAMP) / (r + R_CLAMP) * 1e3)
        if not self.k1:
            if any(self.pin(self.C.PIN_BASE + k)[1] for k in range(4)):
                self.drove_while_handed += 1
        sp = getattr(self.C, 'PIN_SOF', None)
        sp = self.C.PIN_BASE if sp is None else sp
        d = self.pin(sp)[1]
        if not self.sof_dir or self.sof_dir[-1][1] != d:
            self.sof_dir.append((self.t, d))
        if self.record:
            st = (dp, dm, self.k1, int(self.vbus_out()),
                  int(bool(self.chip and self.chip.state not in ('off',))),
                  int(bool(self.chip and self.chip.state == 'ack')))
            if not self.trace or self.trace[-1][1] != st:
                self.trace.append((self.t, st))
        self.t += 1

    def advance(self, n):
        end = self.t + int(n)
        while self.t < end:
            busy = any(sm.active for sm in self.sms.values())
            self.cycle()
            if busy or self.t >= end:
                continue
            cands = [end] + [r[0] for r in self.relay_q]
            if self.chip:
                ne = self.chip.next_event()
                if ne is not None:
                    cands.append(ne)
            nxt = min(c for c in cands if c >= self.t)
            if nxt > self.t:
                self.t = nxt


# --------------------------------------------------------------------------------
# Fake MicroPython modules bound to the running Sim
# --------------------------------------------------------------------------------
class FakePin:
    IN, OUT, OPEN_DRAIN = 0, 1, 2
    PULL_UP, PULL_DOWN = 1, 2
    IRQ_FALLING, IRQ_RISING = 1, 2

    def __init__(self, id, mode=None, pull=None, value=None):
        self.id = id
        if isinstance(id, str) or mode is None:
            return
        p = CUR.pin(id)
        if mode == FakePin.IN:
            p[1] = 0
        else:
            p[1] = 1
            if value is not None:
                p[0] = 1 if value else 0
        self._hook()

    def _hook(self):
        C = CUR.C
        if self.id == C.PIN_K1:
            CUR.set_relay('k1', CUR.pin(self.id)[0] if CUR.pin(self.id)[1] else 0)
        if self.id == C.PIN_K2:
            CUR.set_relay('k2', CUR.pin(self.id)[0] if CUR.pin(self.id)[1] else 0)

    def value(self, v=None):
        CUR.advance(CUR.sc.py_us)
        if isinstance(self.id, str):
            return 0
        if v is None:
            return CUR.read_pin(self.id)
        CUR.pin(self.id)[0] = 1 if v else 0
        self._hook()


class FakeMem32:
    def __getitem__(self, addr):
        assert addr == 0xD0000004
        CUR.advance(1)
        return CUR.gpio_in()


class FakeSMHandle:
    def __init__(self, id, prog, freq=None, set_base=None, out_base=None, sideset_base=None,
                 in_base=None, jmp_pin=None, **kw):
        assert freq == 1_000_000, "model resolution is 1 us per SM cycle (BIT_US = 20)"
        ident = lambda p: None if p is None else p.id
        self.sm = SM(CUR, prog, ident(set_base), ident(out_base), ident(sideset_base),
                     ident(in_base), ident(jmp_pin))
        CUR.sms[id] = self.sm

    def active(self, v=None):
        CUR.advance(CUR.sc.py_us)
        if v is None:
            return int(self.sm.active)
        self.sm.active = bool(v)

    def put(self, v):
        CUR.advance(CUR.sc.py_us)
        while len(self.sm.tx) >= FIFO_DEPTH:
            CUR.advance(1)
        self.sm.tx.append(v & 0xFFFFFFFF)

    def get(self):
        CUR.advance(CUR.sc.py_us)
        while not self.sm.rx:
            CUR.advance(1)
        return self.sm.rx.pop(0)

    def rx_fifo(self):
        CUR.advance(CUR.sc.py_us)
        return len(self.sm.rx)


def _fake_time():
    t = types.ModuleType('time')
    t.ticks_us = lambda: (CUR.advance(1), CUR.t)[1]
    t.ticks_ms = lambda: (CUR.advance(1), CUR.t // 1000)[1]
    t.ticks_diff = lambda a, b: a - b
    t.sleep_ms = lambda n: CUR.advance(n * 1000)
    t.sleep_us = lambda n: CUR.advance(n)
    return t


def load_firmware():
    rp2 = types.ModuleType('rp2')
    rp2.asm_pio, rp2.PIO = asm_pio, FakePIO
    rp2.StateMachine = FakeSMHandle
    machine = types.ModuleType('machine')
    machine.Pin, machine.mem32 = FakePin, FakeMem32()
    sys.modules['rp2'], sys.modules['machine'] = rp2, machine
    sys.path.insert(0, FW)
    import config  # noqa: E402
    import usbkey  # noqa: E402
    usbkey.time = _fake_time()
    return usbkey, config


# --------------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------------
FAILS = []


def check(cond, what):
    print('  [%s] %s' % ('PASS' if cond else 'FAIL', what))
    if not cond:
        FAILS.append(what)


def electrical():
    print('1. Electrical levels (resistive model)')
    rows = [
        ('idle, chip listening: 2k2 pull-up vs 15k pull-down',
         par((VDD, R_PU), (0, R_CHIP_PD)), 'high'),
        ('same + host 15k in parallel (passive tap, no K1)',
         par((VDD, R_PU), (0, R_CHIP_PD), (0, R_HOST_PD)), 'high'),
        ('Pico internal pull-up (~50k) vs 15k  -> why R3/R4 are needed',
         par((VDD, R_PICO_INT_PU), (0, R_CHIP_PD)), 'low'),
        ('ACK: chip drives low vs 2k2', par((VDD, R_PU), (0, R_CHIP_DRV)), 'low'),
        ('unpowered chip (ESD clamp) vs 2k2', par((VDD, R_PU), (V_CLAMP, R_CLAMP)), 'low'),
        ('SOF pulse via R1 1k vs ROM D+ pull-up 1.5k',
         par((0, R_SERIES), (VDD, R_CHIP_DPU)), 'undefined'),
        ('SOF pulse via R5 330R vs ROM D+ pull-up 1.5k',
         par((0, R_SOF), (VDD, R_CHIP_DPU)), 'low'),
    ]
    for name, v, exp in rows:
        got = 'high' if v >= CHIP_VIH else 'low' if v <= CHIP_VIL else 'undefined'
        check(got == exp, '%-62s %.2f V -> %s' % (name, v, got))
    i = (VDD - V_CLAMP) / (R_SERIES + R_CLAMP) * 1e3
    print('  note: key into an UNPOWERED chip back-feeds <= %.1f mA per line via R1/R2 '
          '(vendor dongle does the same; keep R1/R2 >= 1k)' % i)
    i2 = VDD / (R_SERIES + R_CHIP_DRV) * 1e3
    print('  note: ACK contention (Pico drives high, chip drives low) <= %.1f mA for '
          '<= 10 us per line' % i2)


def decode(trace):
    """Decode 16-bit packets from chip-side (t, (dp, dm, ...)) transitions."""
    ev = [(t, s[0], s[1]) for t, s in trace]
    rises, dm_changes = [], []
    for k in range(1, len(ev)):
        if ev[k][1] and not ev[k - 1][1]:
            rises.append((ev[k][0], ev[k][2]))
        if ev[k][2] != ev[k - 1][2]:
            dm_changes.append(ev[k][0])
    falls = [ev[k][0] for k in range(1, len(ev)) if not ev[k][1] and ev[k - 1][1]]
    packets, cur = [], []
    for k, (t, b) in enumerate(rises):
        if cur and t - cur[-1][0] > 30:
            packets.append(cur)
            cur = []
        cur.append((t, b))
    if cur:
        packets.append(cur)
    # Lead-in: if the clock line was LOW before a packet (released, nothing pulling it
    # up, e.g. the very first packet), driving it high creates one extra rising edge
    # 2 us before the first data bit. The ROM shifts one junk bit; the 16 data bits
    # that follow are complete, so the register still matches. Strip it here.
    leadins = 0
    for k, p in enumerate(packets):
        if len(p) == 17 and p[1][0] - p[0][0] < 20:
            packets[k] = p[1:]
            leadins += 1
    sel = [e for p in packets if len(p) == 16 for e in p]
    setup = hold = 10 ** 9
    for t, _ in sel:
        before = [c for c in dm_changes if c <= t]
        after = [c for c in dm_changes if c > t]
        if before:
            setup = min(setup, t - before[-1])
        if after:
            hold = min(hold, after[0] - t)
    periods = []
    for p in packets:
        if len(p) == 16:
            periods += [p[k + 1][0] - p[k][0] for k in range(15)]
    highs = []
    for t, _ in sel:
        f = [x for x in falls if x > t]
        if f and f[0] - t <= 30:
            highs.append(f[0] - t)
    return packets, setup, hold, periods, highs, leadins


def run_waveform(usbkey, C):
    global CUR
    print('2. Waveform (usbkey.waveform(), chip powered but not answering)')
    sc = Scenario('waveform', None, receiver=False)
    CUR = Sim(sc, C, record=True)
    CUR.k2 = 0
    with contextlib.redirect_stdout(io.StringIO()):
        usbkey.waveform(seconds=0.012)
    packets, setup, hold, periods, highs, leadins = decode(
        [(t, s) for t, s in CUR.trace if s[2] == 1])
    full = [p for p in packets if len(p) == 16]
    vals = set()
    for p in full:
        v = 0
        for _, b in p:
            v = (v << 1) | b
        vals.add(v)
    partial = [k for k, p in enumerate(packets) if len(p) != 16]
    check(len(full) >= 10 and partial in ([], [len(packets) - 1]),
          '%d complete 16-bit packets in 12 ms (+%d truncated by stop())'
          % (len(full), len(partial)))
    print('  info: %d lead-in edge(s) (line was low before the first packet; harmless)'
          % leadins)
    check(vals == {0x16EF}, 'every packet decodes to %s (expect 0x16EF, MSB first)'
          % sorted('0x%04X' % v for v in vals))
    check(set(periods) == {20}, 'bit period %s us (expect 20)' % sorted(set(periods)))
    check(set(highs) == {10}, 'clock high time %s us (expect 10)' % sorted(set(highs)))
    check(setup >= 9 and hold >= 9,
          'D- setup %d us / hold %d us around D+ rising edges (need >= 9)' % (setup, hold))
    gaps = [full[k + 1][0][0] - full[k][-1][0] for k in range(len(full) - 1)]
    print('  info: packet repeat %d us, inter-packet gap %d..%d us (sense phase)'
          % (full[1][0][0] - full[0][0][0], min(gaps), max(gaps)))
    check(CUR.undef_pico == 0, 'no Pico input sample in the undefined 0.8..2.0 V band')


def run_scenario(usbkey, C, sc, verbose=False, vcd=None):
    global CUR
    defaults = dict(AUTO_ACK_TIMEOUT_MS=30, MANUAL_WINDOW_S=0.2, RETRIES=2,
                    VBUS_OFF_MS=1500, HANDOFF_QUIET_MS=50, SOF_EMULATION=False,
                    PIN_SOF=None, SOF_PULSES=12, PIN_K2=7)
    saved = {k: getattr(C, k) for k in defaults}
    for k, v in dict(defaults, **(sc.cfg or {})).items():
        setattr(C, k, v)
    CUR = Sim(sc, C, record=vcd is not None)
    orig_log = usbkey._log

    def hooked(*a):
        msg = ' '.join(str(x) for x in a)
        out.write('[%9.3f ms] %s\n' % (CUR.t / 1000, msg))
        sc.on_log(CUR, msg)
    out = io.StringIO()
    usbkey._log = hooked
    try:
        with contextlib.redirect_stdout(out):
            ok = usbkey.run()
        CUR.advance(20_000)                  # let the host side settle
    finally:
        usbkey._log = orig_log
        for k, v in saved.items():
            setattr(C, k, v)
    chip = CUR.chip
    final = chip.state if chip else 'absent'
    res = 'ACK' if ok else 'FAIL'
    detail = 'final chip state=%s' % final
    if chip and chip.cal_by:
        detail += ', SOF calibration by %s' % chip.cal_by
    good = res == sc.expect
    if sc.expect == 'ACK':
        good = good and final == 'uboot'
    else:
        good = good and final != 'uboot' and not (chip and any(s == 'ack' for _, s in chip.history)
                                                   and not ok)
    if sc.expect_cal:
        good = good and chip.cal_by == sc.expect_cal
    good = good and CUR.drove_while_handed == 0 and CUR.undef_pico == 0
    check(good, '%-44s -> %-4s (%s)' % (sc.name, res, detail))
    if verbose or not good:
        for line in out.getvalue().splitlines():
            print('      ' + line)
        if chip:
            print('      chip history:', ', '.join('%.3fms:%s' % (t / 1000, s)
                                                  for t, s in chip.history))
        print('      undefined Pico samples=%d, Pico drove lines after handoff=%d cycles'
              % (CUR.undef_pico, CUR.drove_while_handed))
    if vcd:
        write_vcd(vcd, CUR.trace)
        print('  wrote %s' % vcd)
    return CUR


def run_scenarios(usbkey, C, verbose, vcd):
    print('3. End-to-end scenarios through usbkey.run() (model timeouts shortened)')
    S = Scenario
    sim = run_scenario(usbkey, C, S('normal: VBUS switched by K2', 'ACK'), verbose, vcd)
    lows = [t for t, s in sim.chip.history if s == 'ack']
    if lows:
        on = [t for t, s in sim.chip.history if s == 'boot']
        print('  info: ACK %.2f ms after the last power-on' % ((lows[0] - on[-1]) / 1000))
    print('  info: max back-feed into the unpowered chip %.1f mA/line' % sim.max_backpower_ma)
    run_scenario(usbkey, C, S('slow MicroPython (100 us per call)', 'ACK', py_us=100), verbose)
    for lat in (1, 5, 9, 11, 15):
        exp = 'ACK' if lat <= 10 else 'FAIL'
        run_scenario(usbkey, C, S('ROM ISR latency %d us (data valid 10 us)' % lat, exp,
                                  isr_lat_us=lat), verbose)
    run_scenario(usbkey, C, S('long ROM boot delay (50 ms)', 'ACK', boot_us=50_000), verbose)
    run_scenario(usbkey, C, S('short key window (2 ms)', 'ACK', window_us=2000), verbose)
    run_scenario(usbkey, C, S('pad never powered (no false ACK)', 'FAIL', never_powered=True),
                 verbose)
    run_scenario(usbkey, C, S('nothing attached', 'FAIL', chip=False), verbose)
    run_scenario(usbkey, C, S('battery pad, already on, no user action', 'FAIL',
                              battery=True, vbus_powered=False), verbose)
    run_scenario(usbkey, C, S('battery pad, user toggles power switch', 'ACK',
                              battery=True, vbus_powered=False, user_toggle=True), verbose)
    run_scenario(usbkey, C, S('no K2 fitted, user plugs pad in', 'ACK', vbus_powered=False,
                              battery=False, user_plug=True, cfg=dict(PIN_K2=None)), verbose)
    run_scenario(usbkey, C, S('SOF emulation via R1 (1k): not seen by chip', 'ACK',
                              expect_cal='host', cfg=dict(SOF_EMULATION=True)), verbose)
    run_scenario(usbkey, C, S('SOF emulation via R5 (330R, PIN_SOF=8)', 'ACK',
                              expect_cal='pico', host=False,
                              cfg=dict(SOF_EMULATION=True, PIN_SOF=8)), verbose)


def run_sof(usbkey, C):
    global CUR
    print('4. sof_pulses timing (usbkey.sof_test(12))')
    CUR = Sim(Scenario('sof', None, chip=False), C)
    with contextlib.redirect_stdout(io.StringIO()):
        usbkey.sof_test(12)
    falls = [t for t, d in CUR.sof_dir if d == 1]
    rises = [t for t, d in CUR.sof_dir if d == 0 and t > 0]
    per = {falls[k + 1] - falls[k] for k in range(len(falls) - 1)}
    low = {r - f for f, r in zip(falls, rises[-len(falls):])}
    check(len(falls) == 12, '%d pulses' % len(falls))
    check(per == {1000}, 'period %s us' % sorted(per))
    check(low == {2}, 'low time %s us' % sorted(low))


def write_vcd(path, trace):
    names = ['dp', 'dm', 'k1_lines_to_pico', 'vbus_to_pad', 'chip_powered', 'chip_ack']
    ids = '!"#$%&'
    with open(path, 'w') as f:
        f.write('$timescale 1us $end\n$scope module usbkey $end\n')
        for i, n in zip(ids, names):
            f.write('$var wire 1 %s %s $end\n' % (i, n))
        f.write('$upscope $end\n$enddefinitions $end\n')
        prev = None
        for t, st in trace:
            f.write('#%d\n' % t)
            for k, v in enumerate(st):
                if prev is None or prev[k] != v:
                    f.write('%d%s\n' % (v, ids[k]))
            prev = st


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--vcd')
    a = ap.parse_args()
    usbkey, C = load_firmware()
    print('firmware: %s (KEY=0x%04X, BIT_US=%d)' % (usbkey.__file__, C.KEY, C.BIT_US))
    electrical()
    run_waveform(usbkey, C)
    run_scenarios(usbkey, C, a.verbose, a.vcd)
    run_sof(usbkey, C)
    print('\n%s: %d check(s) failed' % ('FAILED' if FAILS else 'ALL PASSED', len(FAILS)))
    for f in FAILS:
        print('  - ' + f)
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
