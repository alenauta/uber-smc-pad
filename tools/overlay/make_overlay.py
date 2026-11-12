#!/usr/bin/env python3
"""Printable 1:1 sequencer overlays for the SMC-PAD (2-strip A4 & full-panel Silhouette Cameo 5).

Writes into docs/overlay/:
  - smcpad_seq_overlay.html (.pdf with --pdf)       : 2-strip cutout + reference card (A4)
  - smcpad_seq_cameo_overlay.html (.pdf with --pdf) : full-panel 1:1 overlay with background image
                                                      support (CLI --bg and interactive browser UI)
  - smcpad_seq_cameo.svg                            : 1:1 mm layered SVG (background, print, cut)
  - smcpad_seq_cameo_cut.svg                        : 1:1 mm cut-lines-only SVG
  - smcpad_seq_cameo_cut.dxf                        : 1:1 mm R12 DXF cut file (Silhouette Basic)

Geometry (mm, origin = top-left corner of the 227 x 147 mm device body) was measured from the
CAD panel drawing in SMC-PAD_ZE.pdf. Print at 100 % / actual size.
"""
import argparse
import base64
import html
import math
import mimetypes
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_DIR = os.path.join(ROOT, "docs", "overlay")
CHROME = next(
    (c for c in ("/usr/bin/google-chrome", "/usr/bin/chromium", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome") if os.path.exists(c)),
    "google-chrome",
)

# ---- device geometry (mm, origin = top-left of 227 x 147 mm body) -------------------------
BODY_W, BODY_H = 227.0, 147.0
PANEL_RECT = (2.5, 2.5, 224.5, 144.5)    # recessed top face: 222 x 142 mm
CORNER_R = 9.0

KNOB_X = (18.6, 48.7)                    # left / right column
KNOB_Y = (24.5, 57.4, 90.4, 123.3)       # top .. bottom row
KNOB_HOLE_R = 9.75                       # knob collar is ~17.9 mm across

# 4x4 pad grid (from SMC-PAD_ZE.pdf CAD lines: plastic opening is 28.27 x 28.14 mm;
# we use 28.80 x 28.70 mm so the cutout has ~0.27 mm clearance per side around each pad)
PAD_X = (80.92, 113.95, 146.98, 180.00)  # col 0..3 centers
PAD_Y = (24.26, 57.13, 90.00, 122.88)    # row 0..3 centers (top -> bottom)
PAD_W, PAD_H = 28.80, 28.70
PAD_R = 1.2                              # standard pad corner radius
PAD_CENTER_R = 3.2                       # inner corners of pads 6, 7, 10, 11 around center island

BTN_X0, BTN_X1 = 202.9, 215.0
BTN_Y0, BTN_PITCH, BTN_H = 10.8, 13.0, 9.5
BTN_CLEAR = 0.4                          # hole clearance per side (12.9 x 10.3 mm cutout)
BTN_R = 1.2

LEFT_STRIP = (2.5, 2.5, 63.5, 144.5)     # x0 y0 x1 y1, ends where the pad area starts
RIGHT_STRIP = (197.5, 2.5, 224.5, 144.5)

C_TRACK, C_STEP, C_PAT = "#111111", "#e0600b", "#1f6fd1"

# knob number -> (track page, holding a step, KNOB BANK page); "" = unused
KNOBS = {
    1: ("Volume", "Velocity", "Tempo"),
    2: ("Gate", "Gate", "Swing"),
    3: ("Base note", "Note (chord)", "Track root"),
    4: ("Quantize", "Micro-timing", "Track scale"),
    5: ("Probability", "Probability", ""),
    6: ("Ratchet rate", "Ratchet rate", "Track colour"),
    7: ("Steps (length)", "Trig condition", "MIDI channel"),
    8: ("Rate (\u21e7 speed)", "Tie", "Sync offset"),
}
KNOB_POS = {7: (0, 0), 8: (1, 0), 5: (0, 1), 6: (1, 1), 3: (0, 2), 4: (1, 2), 1: (0, 3), 2: (1, 3)}

# buttons top -> bottom: label lines next to each button
BUTTONS = [
    ("BT", []),
    ("PAD BANK", ["TAP: VIEW", "off  step", "on   play", "HOLD TRACKS"]),
    ("KNOB BANK", ["PATTERN", "KNOBS"]),
    ("<", ["PAGE \u25c2", "\u21e7 UNDO"]),
    (">", ["PAGE \u25b8", "\u21e7 REDO"]),
    ("PLAY", ["PLAY", "\u21e7 LOAD"]),
    ("STOP", ["STOP", "BANK+pad", "MUTE", "\u21e7 CLEAR"]),
    ("REC", ["REC", "\u21e7 SAVE"]),
    ("SHIFT", ["+pad MENU", "+K3 ROOT", "+K4 SCALE"]),
    ("NOTE REPEAT", ["hold TIMING", "\u21e7 SEQ", "+step", "RATCHET"]),
]

TIMING = ["1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T",
          "sw 50", "sw 56", "sw 62", "sw 68", "sw 75", "<b>FILL</b>", "SYNC", "TAP"]
TIMING_PLAIN = ["1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T",
                "SW 50%", "SW 56%", "SW 62%", "SW 68%", "SW 75%", "FILL", "SYNC", "TAP"]
SHIFT_PAD = {
    0: "\u21e7PRESET",
    1: "\u21e7CURVE",
    2: "\u21e7TR \u2212",
    3: "\u21e7TR +",
    4: "\u21e7OCT \u2212",
    5: "\u21e7OCT +",
}

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
SCALE_NAMES = ["chrom.", "major", "minor", "dorian", "phryg.", "lydian", "mixol.", "locrian",
               "harm.min", "pent.maj", "pent.min", "blues"]


def esc(s):
    return html.escape(s, quote=True)


def rounded_rect_path(x0, y0, x1, y1, r_tl, r_tr, r_br, r_bl):
    """SVG path for a rectangle with independent corner radii (clockwise)."""
    d = [f"M{x0 + r_tl:.3f},{y0:.3f}", f"L{x1 - r_tr:.3f},{y0:.3f}"]
    if r_tr > 0:
        d.append(f"A{r_tr:.3f},{r_tr:.3f} 0 0 1 {x1:.3f},{y0 + r_tr:.3f}")
    d.append(f"L{x1:.3f},{y1 - r_br:.3f}")
    if r_br > 0:
        d.append(f"A{r_br:.3f},{r_br:.3f} 0 0 1 {x1 - r_br:.3f},{y1:.3f}")
    d.append(f"L{x0 + r_bl:.3f},{y1:.3f}")
    if r_bl > 0:
        d.append(f"A{r_bl:.3f},{r_bl:.3f} 0 0 1 {x0:.3f},{y1 - r_bl:.3f}")
    d.append(f"L{x0:.3f},{y0 + r_tl:.3f}")
    if r_tl > 0:
        d.append(f"A{r_tl:.3f},{r_tl:.3f} 0 0 1 {x0 + r_tl:.3f},{y0:.3f}")
    d.append("Z")
    return " ".join(d)


def strip_path(x0, y0, x1, y1, round_left, round_right):
    tl = bl = CORNER_R if round_left else 0.0
    tr = br = CORNER_R if round_right else 0.0
    return rounded_rect_path(x0, y0, x1, y1, tl, tr, br, bl)


def pad_corners(col, row_from_top):
    """Corner radii (tl, tr, br, bl) for a pad cutout, enlarging the center-island corner."""
    tl = tr = br = bl = PAD_R
    if col == 1 and row_from_top == 1:
        br = PAD_CENTER_R
    elif col == 2 and row_from_top == 1:
        bl = PAD_CENTER_R
    elif col == 1 and row_from_top == 2:
        tr = PAD_CENTER_R
    elif col == 2 and row_from_top == 2:
        tl = PAD_CENTER_R
    return tl, tr, br, bl


def pad_hole_path(col, row_from_top):
    cx, cy = PAD_X[col], PAD_Y[row_from_top]
    x0, y0 = cx - PAD_W / 2, cy - PAD_H / 2
    x1, y1 = cx + PAD_W / 2, cy + PAD_H / 2
    tl, tr, br, bl = pad_corners(col, row_from_top)
    return rounded_rect_path(x0, y0, x1, y1, tl, tr, br, bl)


# ---- 2-strip overlay (original) -----------------------------------------------------------
def svg_open(x0, y0, x1, y1, page_x, page_y):
    w, h = x1 - x0 + 1.0, y1 - y0 + 1.0
    return (f'<svg class="strip" style="left:{page_x - 0.5}mm;top:{page_y - 0.5}mm;width:{w}mm;height:{h}mm" '
            f'viewBox="{x0 - 0.5} {y0 - 0.5} {w} {h}" xmlns="http://www.w3.org/2000/svg">')


def knob_strip(page_x, page_y):
    x0, y0, x1, y1 = LEFT_STRIP
    o = [svg_open(x0, y0, x1, y1, page_x, page_y)]
    o.append(f'<path d="{strip_path(x0, y0, x1, y1, True, False)}" class="cut"/>')
    o.append(f'<text x="{(x0 + x1) / 2}" y="7.6" class="title" text-anchor="middle">SEQUENCER KNOBS</text>')
    lx = 7.0
    for name, col in (("TRACK", C_TRACK), ("HOLD STEP", C_STEP), ("KNOB BANK", C_PAT)):
        o.append(f'<circle cx="{lx}" cy="11.1" r="0.9" fill="{col}"/>')
        o.append(f'<text x="{lx + 1.6}" y="11.9" class="legend" fill="{col}">{name}</text>')
        lx += 4.0 + len(name) * 1.45
    for k, (col, row) in KNOB_POS.items():
        cx, cy = KNOB_X[col], KNOB_Y[row]
        o.append(f'<circle cx="{cx}" cy="{cy}" r="{KNOB_HOLE_R}" class="hole"/>')
        o.append(f'<line x1="{cx - 1.2}" y1="{cy}" x2="{cx + 1.2}" y2="{cy}" class="mark"/>')
        o.append(f'<line x1="{cx}" y1="{cy - 1.2}" x2="{cx}" y2="{cy + 1.2}" class="mark"/>')
        bx, by = cx - KNOB_HOLE_R - 0.4, cy - KNOB_HOLE_R + 1.0
        o.append(f'<circle cx="{bx}" cy="{by}" r="2.1" fill="#111"/>')
        o.append(f'<text x="{bx}" y="{by + 1.05}" class="badge" text-anchor="middle">{k}</text>')
        for i, (txt, c) in enumerate(zip(KNOBS[k], (C_TRACK, C_STEP, C_PAT))):
            y = cy + KNOB_HOLE_R + 2.9 + i * 3.05
            if txt:
                o.append(f'<text x="{cx}" y="{y:.2f}" class="klabel" fill="{c}" text-anchor="middle">{esc(txt)}</text>')
            else:
                o.append(f'<text x="{cx}" y="{y:.2f}" class="klabel" fill="#bbb" text-anchor="middle">\u2013</text>')
    o.append("</svg>")
    return "".join(o)


def button_strip(page_x, page_y):
    x0, y0, x1, y1 = RIGHT_STRIP
    o = [svg_open(x0, y0, x1, y1, page_x, page_y)]
    o.append(f'<path d="{strip_path(x0, y0, x1, y1, False, True)}" class="cut"/>')
    for i, (name, lines) in enumerate(BUTTONS):
        by = BTN_Y0 + i * BTN_PITCH
        o.append(f'<rect x="{BTN_X0 - BTN_CLEAR}" y="{by - BTN_CLEAR:.2f}" '
                 f'width="{BTN_X1 - BTN_X0 + 2 * BTN_CLEAR:.2f}" '
                 f'height="{BTN_H + 2 * BTN_CLEAR}" rx="{BTN_R}" class="hole"/>')
        o.append(f'<text x="{(BTN_X0 + BTN_X1) / 2}" y="{by + BTN_H / 2 + 0.6:.2f}" class="btnname" '
                 f'text-anchor="middle">{esc(name)}</text>')
        n = len(lines)
        for j, ln in enumerate(lines):
            y = by + BTN_H / 2 + 0.6 + (j - (n - 1) / 2) * 2.0
            o.append(f'<text x="{BTN_X1 + 0.9}" y="{y:.2f}" class="blabel">{esc(ln)}</text>')
    o.append("</svg>")
    return "".join(o)


def grid(cells, cls=""):
    """cells: 16 entries, index 0 = pad 1 (bottom-left). Drawn as the device: pad 13 top-left."""
    rows = []
    for r in range(3, -1, -1):
        tds = "".join(f'<td><span class="pn">{r * 4 + c + 1}</span>{cells[r * 4 + c]}</td>' for c in range(4))
        rows.append(f"<tr>{tds}</tr>")
    return f'<table class="grid {cls}">{"".join(rows)}</table>'


def step_cells():
    cells = [""] * 16
    for step in range(16):
        pad = (3 - step // 4) * 4 + step % 4
        cells[pad] = f"<b>{step + 1}</b>"
    return cells


def pick_text():
    return ("Pads 1&ndash;12: C&hellip;B, or " + ", ".join(SCALE_NAMES) + "; none lit = follow.")


def card():
    dash = "\u2013"
    knob_rows = "".join(
        f"<tr><td class='kn'>{k}</td><td>{esc(t)}</td><td class='st'>{esc(s) or dash}</td>"
        f"<td class='pt'>{esc(p) or dash}</td></tr>"
        for k, (t, s, p) in sorted(KNOBS.items()))
    return f"""
<div class="card">
 <div class="col">
  <h2>Sequencer mode</h2>
  <ul>
   <li><b>Shift + Note Repeat</b> enter / leave. The Note Repeat LED flashes on the beat.
    <b>Shift + pad</b>: the normal Shift menu (preset, curve, transpose, octave).</li>
   <li><b>PAD BANK</b> tap: Step &harr; Play view (LED on = Play).</li>
   <li><b>Hold PAD BANK</b>: track layer: pad = select, <b>hold src + tap dst</b> = copy notes &amp; scale
    (<b>+ Shift</b> = also ch/note/vol), <b>+ Stop + pad</b> = mute, <b>+ Shift + Stop + pad</b> = clear.</li>
   <li><b>KNOB BANK</b>: Track &harr; Pattern knobs (LED on = Pattern).</li>
   <li><b>&lt; &gt;</b> step pages of 16 (LED lit = more pages). <b>Shift + &lt; / &gt;</b> undo / redo.</li>
  </ul>
  <h3>Step view</h3>
  <div class="row">{grid(step_cells(), "steps")}
   <ul class="side">
    <li>Tap a pad: step on / off.</li>
    <li>Hold step + knob: step settings (orange). K3 moves the whole chord.</li>
    <li>Hold step + <b>Note Repeat</b>: ratchet &times;1&rarr;2&rarr;3&rarr;4.</li>
   </ul></div>
  <h3>Play view</h3>
  <ul>
   <li>Pads play the track from its base note in its scale (chromatic = drum kit). Root pads brighter.
    Shift-menu octave / transpose move the pads (transpose = one scale step).</li>
   <li><b>Rec</b> + Play: live recording (K4 = quantize strength, adjustable after recording); hold time
    sets the step's length. Hits on a step that already plays add chord notes (up to 4).</li>
  </ul>
  <h3>Transport &amp; sync</h3>
  <ul>
   <li><b>Play</b>: start / continue. <b>Stop</b>: stop; press again = back to the top.</li>
   <li>Sync on (timing pad 15): the DAW's Start / Stop / clock drive the pad. Otherwise the pad sends clock.</li>
   <li><b>Sync offset</b> (KNOB BANK K8, &minus;120&hellip;+120 ms): &minus; early / + late (master &amp; slave).</li>
  </ul>
  <h3>Root &amp; scale</h3>
  <ul>
   <li><b>KNOB BANK K3 / K4</b>: track root / scale (follow = global). <b>Shift + K3 / K4</b>: global,
    also normal mode (follow = preset, root = pad 1's note).</li>
   <li>{pick_text()}</li>
  </ul>
 </div>
 <div class="col">
  <h3>Timing page: hold Note Repeat</h3>
  <div class="row">{grid(TIMING, "timing")}
   <ul class="side">
    <li>1&ndash;8: rate of the current track.</li>
    <li>9&ndash;13: swing (all tracks).</li>
    <li>14: fill while held. 15: sync on / off. 16: tap tempo.</li>
   </ul></div>
  <h3>Knobs</h3>
  <table class="knobs"><tr><th>K</th><th>Track</th><th class="st">Hold step</th><th class="pt">KNOB BANK</th></tr>
  {knob_rows}</table>
  <h3>Reading the pads while turning a knob</h3>
  <ul>
   <li><b>Amounts</b> (volume, velocity, probability, quantize, steps, tempo, swing): bar from pad 1, dim &rarr; full. Micro-timing &amp; sync offset: centre-out (&minus; pads 8&hellip;1 early, + pads 9&hellip;16 late).</li>
   <li><b>Choices</b> light one pad: rate (pads 1&ndash;8 = printed 1/4&hellip;1/32T), ratchet rate, trig condition, root, scale.</li>
   <li><b>Counts</b>: MIDI channel (1 pad = ch 1 &hellip; 16 = ch 16), gate.</li>
   <li>Base note / step note: pads 1&ndash;12 = keyboard, top row = octave. Ratchet count: pads 1&ndash;4.</li>
   <li>Trig conditions (K7): always, 1:2, 2:2, 1:3, 2:3, 3:3, 1:4&hellip;4:4, first, fill, not fill.</li>
   <li>Ratchet rate (K6): auto (split the step), 1/8, 1/16, 1/16T, 1/32, 1/32T, 1/64.</li>
  </ul>
  <h3>Rate = the editing grid</h3>
  <ul>
   <li>K8 / timing pads 1&ndash;8: notes keep their timing. A coarser grid keeps the notes in between
    (pad dim white). <b>Shift + K8</b>: the same steps faster / slower.</li>
  </ul>
  <h3>Pattern slots (16, pad = slot)</h3>
  <ul>
   <li><b>Hold Shift + Rec</b> + pad: save (no pad = current slot). <b>Hold Shift + Play</b> + pad: load
    (playing: on the next bar). Off empty, dim used, full current. Target pad blinks 3&times; green / red.</li>
   <li>Stop saves changes. Another slot: changes kept first; empty slot = new pattern; current slot = revert.</li>
  </ul>
 </div>
</div>"""


CSS = """
@page { size: 297mm 210mm; margin: 0; }
* { box-sizing: border-box; -webkit-print-color-adjust: exact; print-color-adjust: exact; }
html, body { margin: 0; padding: 0; }
body { font-family: "Avenir Next", "Helvetica Neue", Arial, sans-serif; color: #111; }
.page { position: relative; width: 297mm; height: 210mm; overflow: hidden; page-break-after: always; }
.strip { position: absolute; overflow: visible; }
.cut { fill: #fff; stroke: #888; stroke-width: 0.25; stroke-dasharray: 1.6 1.0; }
.hole { fill: #e9e9e9; stroke: #333; stroke-width: 0.25; }
.mark { stroke: #999; stroke-width: 0.2; }
text { font-family: "Avenir Next Condensed", "Arial Narrow", "Helvetica Neue", sans-serif; }
.title { font-size: 2.6px; font-weight: 700; letter-spacing: 0.25px; }
.legend { font-size: 1.85px; font-weight: 700; }
.badge { font-size: 2.6px; font-weight: 700; fill: #fff; }
.klabel { font-size: 2.55px; font-weight: 600; }
.btnname { font-size: 1.7px; fill: #999; font-weight: 600; }
.blabel { font-size: 1.62px; font-weight: 700; }
.head { position: absolute; left: 116mm; top: 9mm; width: 172mm; }
.head h1 { font-size: 5mm; margin: 0 0 1mm; }
.head p { font-size: 2.6mm; margin: 0.6mm 0; line-height: 1.3; }
.ruler { position: absolute; left: 116mm; top: 33mm; }
.sticker { position: absolute; left: 12mm; top: 162mm; width: 17mm; height: 3.8mm; border: 0.25mm dashed #888;
  font: 700 2.3mm "Avenir Next Condensed", sans-serif; text-align: center; line-height: 3.4mm; }
.stickerlab { position: absolute; left: 32mm; top: 161.6mm; font-size: 2.3mm; color: #555; width: 70mm; line-height: 1.25; }
.card { position: absolute; left: 116mm; top: 42mm; width: 172mm; border: 0.3mm solid #111;
  border-radius: 3mm; padding: 3mm 3.5mm; display: flex; gap: 4mm; font-size: 2.4mm; line-height: 1.25; }
.col { flex: 1; min-width: 0; }
.card h2 { font-size: 3.2mm; margin: 0 0 0.8mm; }
.card h3 { font-size: 2.6mm; margin: 1.8mm 0 0.6mm; border-bottom: 0.2mm solid #ccc; }
.card ul { margin: 0; padding-left: 3.2mm; }
.card li { margin: 0.25mm 0; }
.row { display: flex; gap: 2.5mm; align-items: flex-start; }
.side { flex: 1; }
table.grid { border-collapse: separate; border-spacing: 0.7mm; }
table.grid td { width: 9mm; height: 7.2mm; border: 0.25mm solid #333; border-radius: 0.8mm; position: relative;
  text-align: center; vertical-align: middle; font-size: 2.3mm; padding: 1.4mm 0 0; }
table.grid .pn { position: absolute; left: 0.5mm; top: 0.2mm; font-size: 1.5mm; color: #999; }
table.steps b { font-size: 3mm; color: #e0600b; }
table.knobs { border-collapse: collapse; width: 100%; }
table.knobs th, table.knobs td { text-align: left; padding: 0.35mm 1mm; border-bottom: 0.15mm solid #ddd; }
table.knobs .kn { font-weight: 700; }
.st { color: #e0600b; } .pt { color: #1f6fd1; }
.foot { font-size: 2.2mm; color: #555; margin: 2.5mm 0 0; }
"""


def ruler(cls="ruler", style="width:140mm;height:7mm"):
    ticks = []
    for mm in range(0, 101):
        h = 3.0 if mm % 10 == 0 else (2.0 if mm % 5 == 0 else 1.2)
        ticks.append(f'<line x1="{mm}" y1="0" x2="{mm}" y2="{h}" stroke="#111" stroke-width="0.18"/>')
        if mm % 10 == 0:
            ticks.append(f'<text x="{mm}" y="5.4" font-size="2" text-anchor="middle">{mm // 10}</text>')
    ticks.append('<text x="102" y="2.6" font-size="2.2">100 mm: check with a ruler</text>')
    return (f'<svg class="{cls}" style="{style}" viewBox="-1 -0.5 140 7" '
            f'xmlns="http://www.w3.org/2000/svg"><line x1="0" y1="0" x2="100" y1="0" stroke="#111" '
            f'stroke-width="0.25"/>{"".join(ticks)}</svg>')


def build():
    left = knob_strip(10.0, 9.0)
    right = button_strip(80.0, 9.0)
    head = """<div class="head"><h1>SMC-PAD sequencer overlay</h1>
<p>Print at <b>100 % / actual size</b> on card, then check the ruler below. Cut along the dashed outlines and cut out
the grey holes. The left strip goes over the knobs (pull the knob caps off if they don't pass through), the right
strip next to the buttons. The pads stay uncovered: their printed timing labels already match the timing page,
except pad 14 (FILL, use the sticker).</p>
<p>The sizes come from the drawing in the manual. Measure your unit before you cut. Firmware 918 and later.</p></div>"""
    sticker = ('<div class="sticker">FILL</div><div class="stickerlab">&larr; sticker for pad 14: covers the '
               'printed &ldquo;LATCH&rdquo; label</div>')
    page = f'<div class="page">{left}{right}{head}{ruler()}{sticker}{card()}</div>'
    return (f'<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><title>SMC-PAD sequencer overlay'
            f'</title><meta name="description" content="Printable 1:1 overlay for the SMC-PAD sequencer">'
            f'<style>{CSS}</style></head><body>{page}</body></html>')


# ---- Silhouette Cameo 5 full-panel overlay (SVG + DXF + interactive HTML) -----------------
def image_to_data_uri(path):
    if not path:
        return ""
    mime, _ = mimetypes.guess_type(path)
    if not mime:
        mime = "image/png"
    with open(path, "rb") as f:
        b64 = base64.b64encode(f.read()).decode("ascii")
    return f"data:{mime};base64,{b64}"


def cameo_svg(bg_uri="", bg_opacity=1.0, bg_fit="cover", bleed_mm=1.5,
              pill_mode="light", mask_holes=False, cut_only=False, standalone=True):
    """Build the 1:1 full-panel SVG (227 x 147 mm coordinate space).

    Layers:
      - #background     : optional custom background image (clipped to panel + bleed)
      - #print-graphics : all knob, pad, and button labels with readable pill backing
      - #cut-lines      : vector cut paths (#ff0000, 0.12 mm stroke, fill=none) for Cameo 5
    """
    x0, y0, x1, y1 = PANEL_RECT
    outer_d = strip_path(x0, y0, x1, y1, True, True)
    bleed_d = rounded_rect_path(
        x0 - bleed_mm, y0 - bleed_mm, x1 + bleed_mm, y1 + bleed_mm,
        CORNER_R + bleed_mm, CORNER_R + bleed_mm, CORNER_R + bleed_mm, CORNER_R + bleed_mm
    )

    # Theme / pill colors
    if pill_mode == "dark":
        pill_fill = "rgba(16, 18, 24, 0.84)"
        pill_stroke = "rgba(255, 255, 255, 0.22)"
        c_track = "#f3f4f6"
        c_step = "#ff8533"
        c_pat = "#5ea3ff"
        c_muted = "#9ca3af"
        base_fill = "#181a20"
    elif pill_mode == "none":
        pill_fill = "none"
        pill_stroke = "none"
        c_track, c_step, c_pat = C_TRACK, C_STEP, C_PAT
        c_muted = "#666666"
        base_fill = "#ffffff"
    else:
        pill_fill = "rgba(255, 255, 255, 0.86)"
        pill_stroke = "rgba(0, 0, 0, 0.12)"
        c_track, c_step, c_pat = C_TRACK, C_STEP, C_PAT
        c_muted = "#555555"
        base_fill = "#ffffff"

    attrs = (f'width="{BODY_W}mm" height="{BODY_H}mm" viewBox="0 0 {BODY_W} {BODY_H}"'
             if standalone else
             f'id="cameo-svg" class="cameo-svg" viewBox="0 0 {BODY_W} {BODY_H}"')
    o = [f'<svg {attrs} xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">']

    # Embedded styles for SVG portability (Silhouette Studio + browsers + Illustrator/Inkscape)
    o.append("""<style>
    .cut-line { fill: none; stroke: #ff0000; stroke-width: 0.12; stroke-linecap: round; stroke-linejoin: round; }
    .hole-mask { fill: #ffffff; stroke: none; }
    .svg-text { font-family: "Avenir Next Condensed", "Arial Narrow", "Helvetica Neue", Arial, sans-serif; }
    .title { font-size: 2.5px; font-weight: 700; letter-spacing: 0.25px; }
    .legend { font-size: 1.8px; font-weight: 700; }
    .badge { font-size: 2.55px; font-weight: 700; fill: #ffffff; }
    .klabel { font-size: 2.5px; font-weight: 700; }
    .blabel { font-size: 1.50px; font-weight: 700; letter-spacing: -0.03px; }
    .pstep { font-size: 2.15px; font-weight: 800; fill: #ffffff; }
    .ptiming { font-size: 2.15px; font-weight: 700; }
    .pshift { font-size: 1.55px; font-weight: 700; }
    .phead { font-size: 2.1px; font-weight: 700; letter-spacing: 0.15px; }
    </style>""")

    if not cut_only:
        o.append(f'<defs><clipPath id="overlay-bleed-clip"><path id="bleed-clip-path" d="{bleed_d}"/></clipPath></defs>')

        # Layer 1: Background (covers full panel + bleed across sides and pads area)
        par = {"cover": "xMidYMid slice", "contain": "xMidYMid meet", "stretch": "none"}.get(bg_fit, "xMidYMid slice")
        img_disp = "" if bg_uri else "display:none;"
        o.append('<g id="background" clip-path="url(#overlay-bleed-clip)">')
        o.append(f'<path id="bg-base" d="{bleed_d}" fill="{base_fill}"/>')
        o.append(f'<image id="bg-image" href="{esc(bg_uri)}" x="{x0 - bleed_mm:.2f}" y="{y0 - bleed_mm:.2f}" '
                 f'width="{(x1 - x0) + 2 * bleed_mm:.2f}" height="{(y1 - y0) + 2 * bleed_mm:.2f}" '
                 f'preserveAspectRatio="{par}" opacity="{bg_opacity:.2f}" style="{img_disp}"/>')
        # Optional hole-mask preview group (hidden by default so Print & Cut has full bleed inside holes)
        mask_disp = "" if mask_holes else "display:none;"
        o.append(f'<g id="hole-masks" style="{mask_disp}">')
        for cx in KNOB_X:
            for cy in KNOB_Y:
                o.append(f'<circle cx="{cx:.3f}" cy="{cy:.3f}" r="{KNOB_HOLE_R:.3f}" class="hole-mask"/>')
        for r_top in range(4):
            for c_idx in range(4):
                o.append(f'<path d="{pad_hole_path(c_idx, r_top)}" class="hole-mask"/>')
        for i in range(len(BUTTONS)):
            by = BTN_Y0 + i * BTN_PITCH
            bx0, by0 = BTN_X0 - BTN_CLEAR, by - BTN_CLEAR
            bw, bh = BTN_X1 - BTN_X0 + 2 * BTN_CLEAR, BTN_H + 2 * BTN_CLEAR
            o.append(f'<path d="{rounded_rect_path(bx0, by0, bx0 + bw, by0 + bh, BTN_R, BTN_R, BTN_R, BTN_R)}" class="hole-mask"/>')
        o.append('</g>')
        o.append('</g>')

        # Layer 2: Print graphics (knobs, pad headers/footers, buttons)
        o.append('<g id="print-graphics" class="svg-text">')

        # 2a. Left section header + legend
        lx_mid = (LEFT_STRIP[0] + LEFT_STRIP[2]) / 2
        o.append(f'<rect class="label-pill" x="5.2" y="4.8" width="55.6" height="8.4" rx="1.6" '
                 f'fill="{pill_fill}" stroke="{pill_stroke}" stroke-width="0.15"/>')
        o.append(f'<text x="{lx_mid:.2f}" y="7.8" class="title track-txt" fill="{c_track}" '
                 f'text-anchor="middle">SEQUENCER KNOBS</text>')
        lx = 7.2
        for name, col_c, cls_n in (("TRACK", c_track, "track-txt"),
                                   ("HOLD STEP", c_step, "step-txt"),
                                   ("KNOB BANK", c_pat, "pat-txt")):
            o.append(f'<circle cx="{lx:.2f}" cy="11.1" r="0.9" fill="{col_c}" class="{cls_n}-dot"/>')
            o.append(f'<text x="{lx + 1.6:.2f}" y="11.85" class="legend {cls_n}" fill="{col_c}">{name}</text>')
            lx += 4.0 + len(name) * 1.45

        # 2b. 8 Knobs
        for k, (col, row) in KNOB_POS.items():
            cx, cy = KNOB_X[col], KNOB_Y[row]
            # Backing pill for the 3 knob labels below each knob
            pw, ph = 27.6, 10.2
            py = cy + KNOB_HOLE_R + 0.55
            o.append(f'<rect class="label-pill" x="{cx - pw / 2:.2f}" y="{py:.2f}" width="{pw:.2f}" '
                     f'height="{ph:.2f}" rx="1.4" fill="{pill_fill}" stroke="{pill_stroke}" stroke-width="0.15"/>')
            bx, by = cx - KNOB_HOLE_R - 0.4, cy - KNOB_HOLE_R + 1.0
            o.append(f'<circle cx="{bx:.2f}" cy="{by:.2f}" r="2.15" fill="#111111" stroke="#ffffff" stroke-width="0.25"/>')
            o.append(f'<text x="{bx:.2f}" y="{by + 0.92:.2f}" class="badge" text-anchor="middle">{k}</text>')
            for i, (txt, c, cls_n) in enumerate(zip(KNOBS[k], (c_track, c_step, c_pat),
                                                    ("track-txt", "step-txt", "pat-txt"))):
                ty = cy + KNOB_HOLE_R + 2.95 + i * 3.0
                if txt:
                    o.append(f'<text x="{cx:.2f}" y="{ty:.2f}" class="klabel {cls_n}" fill="{c}" '
                             f'text-anchor="middle">{esc(txt)}</text>')
                else:
                    o.append(f'<text x="{cx:.2f}" y="{ty:.2f}" class="klabel muted-txt" fill="{c_muted}" '
                             f'text-anchor="middle">\u2013</text>')

        # 2c. Center pad section top header bar (above Row 0 pads, y = 4.6 .. 8.7 mm)
        pad_x0 = PAD_X[0] - PAD_W / 2
        pad_x1 = PAD_X[3] + PAD_W / 2
        pad_span = pad_x1 - pad_x0
        o.append(f'<rect class="label-pill" x="{pad_x0:.2f}" y="4.6" width="{pad_span:.2f}" height="4.1" rx="1.3" '
                 f'fill="{pill_fill}" stroke="{pill_stroke}" stroke-width="0.15"/>')
        o.append(f'<text x="{(pad_x0 + pad_x1) / 2:.2f}" y="7.35" class="phead track-txt" fill="{c_track}" '
                 f'text-anchor="middle">'
                 f'<tspan fill="{c_step}" class="step-txt">STEP 1\u201316</tspan>  \u00b7  '
                 f'HOLD NOTE REPEAT = TIMING  \u00b7  '
                 f'HOLD PAD BANK = TRACKS (HOLD SRC + TAP DST = COPY)  \u00b7  '
                 f'\u21e7+REC SAVE / \u21e7+PLAY LOAD</text>')

        # 2d. 16 Pad footer strips (in the 4.2 mm gap directly below each pad)
        for r_top in range(4):
            for c_idx in range(4):
                cx, cy = PAD_X[c_idx], PAD_Y[r_top]
                step_num = r_top * 4 + c_idx + 1          # top-left = step 1 .. bottom-right = step 16
                pad_idx = (3 - r_top) * 4 + c_idx         # bottom-left = pad 0 (PAD 1) .. top-right = pad 15 (PAD 16)
                timing_lbl = TIMING_PLAIN[pad_idx]
                shift_lbl = SHIFT_PAD.get(pad_idx, "")

                fx0 = cx - PAD_W / 2 + 0.3
                fw = PAD_W - 0.6
                fy0 = cy + PAD_H / 2 + 0.42
                fh = 3.30
                o.append(f'<rect class="label-pill" x="{fx0:.2f}" y="{fy0:.2f}" width="{fw:.2f}" height="{fh:.2f}" '
                         f'rx="0.9" fill="{pill_fill}" stroke="{pill_stroke}" stroke-width="0.14"/>')
                # Orange step badge on the left of the strip
                sbw = 5.4
                o.append(f'<rect x="{fx0 + 0.25:.2f}" y="{fy0 + 0.25:.2f}" width="{sbw:.2f}" height="{fh - 0.50:.2f}" '
                         f'rx="0.65" fill="{c_step}"/>')
                o.append(f'<text x="{fx0 + 0.25 + sbw / 2:.2f}" y="{fy0 + 2.38:.2f}" class="pstep" '
                         f'text-anchor="middle">{step_num}</text>')

                # Timing label (and optional Shift-menu label)
                if shift_lbl:
                    o.append(f'<text x="{fx0 + sbw + 1.2:.2f}" y="{fy0 + 2.40:.2f}" class="ptiming track-txt" '
                             f'fill="{c_track}">{esc(timing_lbl)}</text>')
                    o.append(f'<text x="{fx0 + fw - 0.8:.2f}" y="{fy0 + 2.32:.2f}" class="pshift pat-txt" '
                             f'fill="{c_pat}" text-anchor="end">{esc(shift_lbl)}</text>')
                else:
                    t_col = c_step if timing_lbl == "FILL" else c_track
                    t_cls = "step-txt" if timing_lbl == "FILL" else "track-txt"
                    o.append(f'<text x="{fx0 + sbw + (fw - sbw) / 2:.2f}" y="{fy0 + 2.40:.2f}" '
                             f'class="ptiming {t_cls}" fill="{t_col}" text-anchor="middle">{esc(timing_lbl)}</text>')

        # 2e. 10 Right-side buttons
        for i, (name, lines) in enumerate(BUTTONS):
            by = BTN_Y0 + i * BTN_PITCH
            if lines:
                n = len(lines)
                bh_pill = max(4.2, n * 2.05 + 1.0)
                by_pill = by + BTN_H / 2 - bh_pill / 2
                bx_pill = BTN_X1 + BTN_CLEAR + 0.25
                bw_pill = 224.0 - bx_pill
                o.append(f'<rect class="label-pill" x="{bx_pill:.2f}" y="{by_pill:.2f}" width="{bw_pill:.2f}" '
                         f'height="{bh_pill:.2f}" rx="0.8" fill="{pill_fill}" stroke="{pill_stroke}" stroke-width="0.12"/>')
                for j, ln in enumerate(lines):
                    y = by + BTN_H / 2 + 0.55 + (j - (n - 1) / 2) * 1.95
                    o.append(f'<text x="{BTN_X1 + 0.85:.2f}" y="{y:.2f}" class="blabel track-txt" '
                             f'fill="{c_track}">{esc(ln)}</text>')

        o.append('</g>')

    # Layer 3: Cut lines (Silhouette Cameo 5 vector paths)
    o.append('<g id="cut-lines">')
    o.append(f'<path id="cut-outer-panel" d="{outer_d}" class="cut-line"/>')
    for k, (col, row) in sorted(KNOB_POS.items()):
        cx, cy = KNOB_X[col], KNOB_Y[row]
        o.append(f'<circle id="cut-knob-{k}" cx="{cx:.3f}" cy="{cy:.3f}" r="{KNOB_HOLE_R:.3f}" class="cut-line"/>')
    for r_top in range(4):
        for c_idx in range(4):
            pad_num = (3 - r_top) * 4 + c_idx + 1
            o.append(f'<path id="cut-pad-{pad_num}" d="{pad_hole_path(c_idx, r_top)}" class="cut-line"/>')
    for i, (name, _) in enumerate(BUTTONS):
        by = BTN_Y0 + i * BTN_PITCH
        bx0 = BTN_X0 - BTN_CLEAR
        by0 = by - BTN_CLEAR
        bw = BTN_X1 - BTN_X0 + 2 * BTN_CLEAR
        bh = BTN_H + 2 * BTN_CLEAR
        o.append(f'<path id="cut-btn-{i + 1}" d="{rounded_rect_path(bx0, by0, bx0 + bw, by0 + bh, BTN_R, BTN_R, BTN_R, BTN_R)}" '
                 f'class="cut-line"/>')
    o.append('</g>')

    o.append('</svg>')
    return "\n".join(o)


def cameo_dxf():
    """Generate a 1:1 millimeter AutoCAD R12 DXF file of all cut lines for Silhouette Studio Basic."""
    lines = ["0", "SECTION", "2", "HEADER", "9", "$INSUNITS", "70", "4", "0", "ENDSEC",
             "0", "SECTION", "2", "ENTITIES"]

    # DXF uses Y-up; flip Y so the panel looks upright (0..147 mm)
    def fy(y):
        return BODY_H - y

    def add_circle(cx, cy, r, layer="CUT"):
        lines.extend([
            "0", "CIRCLE", "8", layer, "62", "1",
            "10", f"{cx:.4f}", "20", f"{fy(cy):.4f}", "30", "0.0",
            "40", f"{r:.4f}"
        ])

    def add_rounded_rect(x0, y0, x1, y1, r_tl, r_tr, r_br, r_bl, layer="CUT", segs=12):
        # Trace clockwise in top-down coords, sampling corner arcs
        pts = []
        corners = [
            (x0 + r_tl, y0 + r_tl, r_tl, math.pi, 1.5 * math.pi),
            (x1 - r_tr, y0 + r_tr, r_tr, 1.5 * math.pi, 2.0 * math.pi),
            (x1 - r_br, y1 - r_br, r_br, 0.0, 0.5 * math.pi),
            (x0 + r_bl, y1 - r_bl, r_bl, 0.5 * math.pi, math.pi),
        ]
        for cx, cy, r, a0, a1 in corners:
            if r <= 0:
                pts.append((cx, cy))
            else:
                for s in range(segs + 1):
                    a = a0 + (a1 - a0) * s / segs
                    pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
        lines.extend(["0", "POLYLINE", "8", layer, "62", "1", "66", "1", "70", "1"])
        for px, py in pts:
            lines.extend(["0", "VERTEX", "8", layer, "10", f"{px:.4f}", "20", f"{fy(py):.4f}", "30", "0.0"])
        lines.extend(["0", "SEQEND"])

    # Outer panel
    x0, y0, x1, y1 = PANEL_RECT
    add_rounded_rect(x0, y0, x1, y1, CORNER_R, CORNER_R, CORNER_R, CORNER_R)
    # 8 knobs
    for cx in KNOB_X:
        for cy in KNOB_Y:
            add_circle(cx, cy, KNOB_HOLE_R)
    # 16 pads
    for r_top in range(4):
        for c_idx in range(4):
            cx, cy = PAD_X[c_idx], PAD_Y[r_top]
            tl, tr, br, bl = pad_corners(c_idx, r_top)
            add_rounded_rect(cx - PAD_W / 2, cy - PAD_H / 2, cx + PAD_W / 2, cy + PAD_H / 2, tl, tr, br, bl)
    # 10 buttons
    for i in range(len(BUTTONS)):
        by = BTN_Y0 + i * BTN_PITCH
        add_rounded_rect(
            BTN_X0 - BTN_CLEAR, by - BTN_CLEAR,
            BTN_X1 + BTN_CLEAR, by + BTN_H + BTN_CLEAR,
            BTN_R, BTN_R, BTN_R, BTN_R
        )

    lines.extend(["0", "ENDSEC", "0", "EOF", ""])
    return "\n".join(lines)


CAMEO_CSS = """
@page { size: 297mm 210mm; margin: 0; }
* { box-sizing: border-box; -webkit-print-color-adjust: exact; print-color-adjust: exact; }
html, body { margin: 0; padding: 0; background: #f3f4f6; color: #111;
  font-family: "Avenir Next", "Helvetica Neue", Arial, sans-serif; }
.toolbar { background: #181a20; color: #f3f4f6; padding: 10px 18px; display: flex; flex-wrap: wrap;
  gap: 14px; align-items: center; font-size: 13px; border-bottom: 1px solid #2d3139; }
.toolbar h1 { font-size: 15px; margin: 0 8px 0 0; font-weight: 700; color: #fff; }
.toolbar label { display: inline-flex; align-items: center; gap: 6px; color: #d1d5db; font-weight: 500; }
.toolbar input[type="range"] { width: 85px; }
.toolbar select, .toolbar button, .toolbar .btn-file {
  background: #2b303c; color: #fff; border: 1px solid #454c5c; border-radius: 5px;
  padding: 5px 10px; font-size: 12.5px; cursor: pointer; font-weight: 600; }
.toolbar button:hover, .toolbar .btn-file:hover { background: #3b4252; }
.toolbar button.primary { background: #e0600b; border-color: #f97316; }
.toolbar button.primary:hover { background: #f97316; }
.page { position: relative; width: 297mm; height: 210mm; margin: 8mm auto; background: #fff;
  box-shadow: 0 4px 20px rgba(0,0,0,0.12); overflow: hidden; }
.skin-wrap { position: absolute; left: 35mm; top: 20mm; width: 227mm; height: 147mm; }
.cameo-svg { width: 227mm; height: 147mm; display: block; }
.meta-bar { position: absolute; left: 35mm; top: 8mm; width: 227mm; display: flex;
  justify-content: space-between; align-items: baseline; font-size: 2.5mm; color: #333; }
.meta-bar b { font-size: 3.4mm; color: #111; }
.foot-bar { position: absolute; left: 35mm; top: 171mm; width: 227mm; display: flex;
  justify-content: space-between; align-items: flex-start; gap: 8mm; font-size: 2.35mm; color: #444; line-height: 1.35; }
.reg-mark { position: absolute; pointer-events: none; display: none; }
.show-reg .reg-mark { display: block; }
@media print {
  html, body { background: #fff; }
  .no-print { display: none !important; }
  .page { margin: 0; box-shadow: none; page-break-after: always; }
}
"""


def build_cameo_html(bg_uri="", bg_opacity=1.0, bg_fit="cover", bleed_mm=1.5, pill_mode="light", mask_holes=False):
    svg_markup = cameo_svg(
        bg_uri=bg_uri, bg_opacity=bg_opacity, bg_fit=bg_fit,
        bleed_mm=bleed_mm, pill_mode=pill_mode, mask_holes=mask_holes, cut_only=False, standalone=False
    )
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>SMC-PAD Full-Panel Overlay (Silhouette Cameo 5)</title>
<meta name="description" content="1:1 full-panel printable and cuttable overlay for SMC-PAD and Silhouette Cameo 5">
<style>{CAMEO_CSS}</style>
</head>
<body>
<div class="toolbar no-print">
  <h1>SMC-PAD Cameo 5 Overlay</h1>
  <label class="btn-file">
    <span>Background Image&hellip;</span>
    <input type="file" id="bg-file" accept="image/*" style="display:none">
  </label>
  <button type="button" id="bg-clear">Clear Image</button>
  <label>Fit
    <select id="bg-fit">
      <option value="cover" {"selected" if bg_fit == "cover" else ""}>Cover</option>
      <option value="contain" {"selected" if bg_fit == "contain" else ""}>Contain</option>
      <option value="stretch" {"selected" if bg_fit == "stretch" else ""}>Stretch</option>
    </select>
  </label>
  <label>Opacity <input type="range" id="bg-opacity" min="0.1" max="1" step="0.05" value="{bg_opacity:.2f}"></label>
  <label>Zoom <input type="range" id="bg-zoom" min="0.5" max="2.5" step="0.05" value="1.00"></label>
  <label>Pan X <input type="range" id="bg-panx" min="-60" max="60" step="1" value="0"></label>
  <label>Pan Y <input type="range" id="bg-pany" min="-60" max="60" step="1" value="0"></label>
  <label>Theme
    <select id="pill-mode">
      <option value="light" {"selected" if pill_mode == "light" else ""}>Light</option>
      <option value="dark" {"selected" if pill_mode == "dark" else ""}>Dark</option>
    </select>
  </label>
  <label><input type="checkbox" id="toggle-cut" checked> Cut lines</label>
  <label><input type="checkbox" id="toggle-holes" {"checked" if mask_holes else ""}> Mask holes</label>
  <label><input type="checkbox" id="toggle-reg"> Print&amp;Cut marks</label>
  <button type="button" id="dl-svg" class="primary">Download SVG (Print+Cut)</button>
  <button type="button" id="dl-cut-svg">Download Cut-Only SVG</button>
  <button type="button" onclick="window.print()">Print / Save PDF</button>
</div>

<div class="page" id="sheet">
  <!-- Optional Silhouette-style registration marks preview around the 227x147mm area -->
  <svg class="reg-mark" style="left:25mm;top:12mm;width:247mm;height:163mm" viewBox="0 0 247 163" xmlns="http://www.w3.org/2000/svg">
    <rect x="0" y="0" width="5" height="5" fill="#000"/>
    <path d="M247,0 L237,0 L237,1.2 L245.8,1.2 L245.8,10 L247,10 Z" fill="#000"/>
    <path d="M0,163 L10,163 L10,161.8 L1.2,161.8 L1.2,153 L0,153 Z" fill="#000"/>
  </svg>

  <div class="meta-bar no-print-hide">
    <div><b>SMC-PAD Sequencer Full-Panel Overlay</b> &mdash; 1:1 scale (222.0 &times; 142.0 mm panel inside 227 &times; 147 mm body)</div>
    <div>Silhouette Cameo 5: cut red paths (<code>#ff0000</code>)</div>
  </div>

  <div class="skin-wrap">
    {svg_markup}
  </div>

  <div class="foot-bar">
    <div>
      {ruler(cls="cameo-ruler", style="width:130mm;height:7mm")}
    </div>
    <div style="max-width:92mm">
      <b>Silhouette Cameo 5 workflow:</b> Open <code>smcpad_seq_cameo.svg</code> (Designer) or
      <code>smcpad_seq_cameo_cut.dxf</code> (Basic) at <b>100 % / mm units</b>, or use this page/SVG for
      <b>Print &amp; Cut</b> on printable vinyl / sticker sheet. Uncheck <i>Cut lines</i> before printing if you only
      want the blade to follow the SVG/DXF paths without visible red outlines.
    </div>
  </div>
</div>

<script>
(function() {{
  const svg = document.getElementById('cameo-svg');
  const bgImg = document.getElementById('bg-image');
  const bgBase = document.getElementById('bg-base');
  const holeMasks = document.getElementById('hole-masks');
  const cutGroup = document.getElementById('cut-lines');
  const sheet = document.getElementById('sheet');
  const baseBox = {{ x: {PANEL_RECT[0] - bleed_mm:.2f}, y: {PANEL_RECT[1] - bleed_mm:.2f},
                     w: {(PANEL_RECT[2] - PANEL_RECT[0]) + 2 * bleed_mm:.2f},
                     h: {(PANEL_RECT[3] - PANEL_RECT[1]) + 2 * bleed_mm:.2f} }};

  function updateTransform() {{
    const z = parseFloat(document.getElementById('bg-zoom').value) || 1;
    const px = parseFloat(document.getElementById('bg-panx').value) || 0;
    const py = parseFloat(document.getElementById('bg-pany').value) || 0;
    const w = baseBox.w * z, h = baseBox.h * z;
    const x = baseBox.x + (baseBox.w - w) / 2 + px;
    const y = baseBox.y + (baseBox.h - h) / 2 + py;
    bgImg.setAttribute('x', x.toFixed(2));
    bgImg.setAttribute('y', y.toFixed(2));
    bgImg.setAttribute('width', w.toFixed(2));
    bgImg.setAttribute('height', h.toFixed(2));
  }}

  function loadFile(file) {{
    if (!file) return;
    const reader = new FileReader();
    reader.onload = function(e) {{
      bgImg.setAttribute('href', e.target.result);
      bgImg.style.display = '';
    }};
    reader.readAsDataURL(file);
  }}

  document.getElementById('bg-file').addEventListener('change', e => loadFile(e.target.files[0]));
  window.addEventListener('dragover', e => e.preventDefault());
  window.addEventListener('drop', e => {{
    e.preventDefault();
    if (e.dataTransfer.files && e.dataTransfer.files[0]) loadFile(e.dataTransfer.files[0]);
  }});

  document.getElementById('bg-clear').addEventListener('click', () => {{
    bgImg.setAttribute('href', '');
    bgImg.style.display = 'none';
  }});

  document.getElementById('bg-fit').addEventListener('change', e => {{
    const map = {{ cover: 'xMidYMid slice', contain: 'xMidYMid meet', stretch: 'none' }};
    bgImg.setAttribute('preserveAspectRatio', map[e.target.value] || 'xMidYMid slice');
  }});

  document.getElementById('bg-opacity').addEventListener('input', e => {{
    bgImg.setAttribute('opacity', parseFloat(e.target.value).toFixed(2));
  }});

  ['bg-zoom', 'bg-panx', 'bg-pany'].forEach(id => {{
    document.getElementById(id).addEventListener('input', updateTransform);
  }});

  document.getElementById('pill-mode').addEventListener('change', e => {{
    const m = e.target.value;
    const pills = svg.querySelectorAll('.label-pill');
    const trackTxt = svg.querySelectorAll('.track-txt');
    const stepTxt = svg.querySelectorAll('.step-txt');
    const patTxt = svg.querySelectorAll('.pat-txt');
    if (m === 'dark') {{
      pills.forEach(p => {{ p.setAttribute('fill', 'rgba(16, 18, 24, 0.84)'); p.setAttribute('stroke', 'rgba(255,255,255,0.22)'); }});
      trackTxt.forEach(t => t.setAttribute('fill', '#f3f4f6'));
      stepTxt.forEach(t => t.setAttribute('fill', '#ff8533'));
      patTxt.forEach(t => t.setAttribute('fill', '#5ea3ff'));
      bgBase.setAttribute('fill', '#181a20');
    }} else {{
      pills.forEach(p => {{ p.setAttribute('fill', 'rgba(255, 255, 255, 0.86)'); p.setAttribute('stroke', 'rgba(0,0,0,0.12)'); }});
      trackTxt.forEach(t => t.setAttribute('fill', '#111111'));
      stepTxt.forEach(t => t.setAttribute('fill', '#e0600b'));
      patTxt.forEach(t => t.setAttribute('fill', '#1f6fd1'));
      bgBase.setAttribute('fill', '#ffffff');
    }}
  }});

  document.getElementById('toggle-cut').addEventListener('change', e => {{
    cutGroup.style.display = e.target.checked ? '' : 'none';
  }});

  document.getElementById('toggle-holes').addEventListener('change', e => {{
    holeMasks.style.display = e.target.checked ? '' : 'none';
  }});

  document.getElementById('toggle-reg').addEventListener('change', e => {{
    sheet.classList.toggle('show-reg', e.target.checked);
  }});

  function downloadSvg(cutOnly, filename) {{
    const clone = svg.cloneNode(true);
    clone.removeAttribute('id');
    clone.removeAttribute('class');
    clone.setAttribute('width', '227.0mm');
    clone.setAttribute('height', '147.0mm');
    const cg = clone.querySelector('#cut-lines');
    if (cg) cg.style.display = '';
    if (cutOnly) {{
      const bg = clone.querySelector('#background');
      const pg = clone.querySelector('#print-graphics');
      const defs = clone.querySelector('defs');
      if (bg) bg.remove();
      if (pg) pg.remove();
      if (defs) defs.remove();
    }}
    const blob = new Blob(['<?xml version="1.0" encoding="UTF-8"?>\\n' + clone.outerHTML], {{ type: 'image/svg+xml' }});
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = filename;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  }}

  document.getElementById('dl-svg').addEventListener('click', () => downloadSvg(false, 'smcpad_seq_cameo.svg'));
  document.getElementById('dl-cut-svg').addEventListener('click', () => downloadSvg(true, 'smcpad_seq_cameo_cut.svg'));
}})();
</script>
</body>
</html>"""


def render_pdf(html_path, pdf_path):
    subprocess.run([CHROME, "--headless", "--disable-gpu", "--no-sandbox", "--no-pdf-header-footer",
                    f"--print-to-pdf={pdf_path}", "file://" + html_path], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pdf", action="store_true", help="also render PDFs via headless Chrome")
    ap.add_argument("--bg", metavar="IMAGE", default="", help="background image path for the full-panel Cameo overlay")
    ap.add_argument("--bg-opacity", type=float, default=1.0, help="background image opacity (0.0..1.0, default 1.0)")
    ap.add_argument("--bg-fit", choices=("cover", "contain", "stretch"), default="cover", help="background image fit")
    ap.add_argument("--bleed", type=float, default=1.5, help="background bleed outside outer cut line in mm (default 1.5)")
    ap.add_argument("--theme", choices=("light", "dark"), default="light",
                    help="theme style (default: light)")
    ap.add_argument("--mask-holes", action="store_true", help="mask knob/pad/button cutouts in white on the print layer")
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    bg_uri = image_to_data_uri(args.bg) if args.bg else ""

    # 1. Original 2-strip overlay + reference sheet
    html_path = os.path.join(OUT_DIR, "smcpad_seq_overlay.html")
    with open(html_path, "w", encoding="utf-8") as f:
        f.write(build())
    print(html_path)

    # 2. Full-panel Silhouette Cameo 5 interactive HTML
    cameo_html_path = os.path.join(OUT_DIR, "smcpad_seq_cameo_overlay.html")
    with open(cameo_html_path, "w", encoding="utf-8") as f:
        f.write(build_cameo_html(
            bg_uri=bg_uri, bg_opacity=args.bg_opacity, bg_fit=args.bg_fit,
            bleed_mm=args.bleed, pill_mode=args.theme, mask_holes=args.mask_holes
        ))
    print(cameo_html_path)

    # 3. Standalone 1:1 Print+Cut SVG (227 x 147 mm)
    cameo_svg_path = os.path.join(OUT_DIR, "smcpad_seq_cameo.svg")
    with open(cameo_svg_path, "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n' + cameo_svg(
            bg_uri=bg_uri, bg_opacity=args.bg_opacity, bg_fit=args.bg_fit,
            bleed_mm=args.bleed, pill_mode=args.theme, mask_holes=args.mask_holes, cut_only=False, standalone=True
        ))
    print(cameo_svg_path)

    # 4. Standalone 1:1 Cut-Only SVG (227 x 147 mm)
    cut_svg_path = os.path.join(OUT_DIR, "smcpad_seq_cameo_cut.svg")
    with open(cut_svg_path, "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n' + cameo_svg(cut_only=True, standalone=True))
    print(cut_svg_path)

    # 5. Standalone 1:1 Cut-Only DXF (for Silhouette Studio Basic Edition)
    cut_dxf_path = os.path.join(OUT_DIR, "smcpad_seq_cameo_cut.dxf")
    with open(cut_dxf_path, "w", encoding="utf-8") as f:
        f.write(cameo_dxf())
    print(cut_dxf_path)

    if args.pdf:
        pdf_path = os.path.join(OUT_DIR, "smcpad_seq_overlay.pdf")
        render_pdf(html_path, pdf_path)
        print(pdf_path)
        cameo_pdf_path = os.path.join(OUT_DIR, "smcpad_seq_cameo_overlay.pdf")
        render_pdf(cameo_html_path, cameo_pdf_path)
        print(cameo_pdf_path)


if __name__ == "__main__":
    main()
