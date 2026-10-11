#!/usr/bin/env python3
"""Printable 1:1 sequencer overlay for the SMC-PAD (A4 landscape).

Writes docs/overlay/smcpad_seq_overlay.html; with --pdf it also prints a PDF through headless
Chrome. Geometry (mm, origin = top-left corner of the device) was measured from the panel
drawing in SMC-PAD_ZE.pdf, which is to scale with the 227 x 147 mm body. Print at 100 %.
"""
import html
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_DIR = os.path.join(ROOT, "docs", "overlay")
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

# ---- device geometry (mm) -----------------------------------------------------------------
KNOB_X = (18.6, 48.7)                    # left / right column
KNOB_Y = (24.5, 57.4, 90.4, 123.3)       # top .. bottom row
KNOB_HOLE_R = 9.75                       # knob collar is ~17.9 mm across
BTN_X0, BTN_X1 = 202.9, 215.0
BTN_Y0, BTN_PITCH, BTN_H = 10.8, 13.0, 9.5
LEFT_STRIP = (2.5, 2.5, 63.5, 144.5)     # x0 y0 x1 y1, ends where the pad area starts
RIGHT_STRIP = (197.5, 2.5, 224.5, 144.5)
CORNER_R = 9.0

C_TRACK, C_STEP, C_PAT = "#111111", "#e0600b", "#1f6fd1"

# knob number -> (track page, holding a step, KNOB BANK page); "" = unused
KNOBS = {
    1: ("Volume", "Velocity", "Tempo"),
    2: ("Gate", "Gate", "Swing"),
    3: ("Base note", "Note (chord)", "Track root"),
    4: ("Rec quantize", "Micro-timing", "Track scale"),
    5: ("Probability", "Probability", ""),
    6: ("Ratchet rate", "Ratchet rate", ""),
    7: ("Steps (length)", "Trig condition", "MIDI channel"),
    8: ("Rate", "Tie", "Play early"),
}
KNOB_POS = {7: (0, 0), 8: (1, 0), 5: (0, 1), 6: (1, 1), 3: (0, 2), 4: (1, 2), 1: (0, 3), 2: (1, 3)}

# buttons top -> bottom: label lines next to each button
BUTTONS = [
    ("BT", []),
    ("PAD BANK", ["TAP: VIEW", "off  step", "on   play", "HOLD TRACKS"]),
    ("KNOB BANK", ["PATTERN", "KNOBS"]),
    ("<", ["PAGE \u25c2", "\u21e7 UNDO"]),
    (">", ["PAGE \u25b8", "\u21e7 REDO"]),
    ("PLAY", ["PLAY"]),
    ("STOP", ["STOP", "BANK+pad", "MUTE", "\u21e7 CLEAR"]),
    ("REC", ["REC", "\u21e7 SAVE \u2020"]),
    ("SHIFT", ["+K3 ROOT", "+K4 SCALE", "+pad PATT.\u2020"]),
    ("NOTE REPEAT", ["hold TIMING", "\u21e7 SEQ", "+step", "RATCHET"]),
]


def esc(s):
    return html.escape(s, quote=True)


def strip_path(x0, y0, x1, y1, round_left, round_right):
    r = CORNER_R
    tl = tr = br = bl = 0.0
    if round_left:
        tl = bl = r
    if round_right:
        tr = br = r
    d = [f"M{x0 + tl:.2f},{y0:.2f}", f"L{x1 - tr:.2f},{y0:.2f}"]
    if tr:
        d.append(f"A{tr},{tr} 0 0 1 {x1:.2f},{y0 + tr:.2f}")
    d.append(f"L{x1:.2f},{y1 - br:.2f}")
    if br:
        d.append(f"A{br},{br} 0 0 1 {x1 - br:.2f},{y1:.2f}")
    d.append(f"L{x0 + bl:.2f},{y1:.2f}")
    if bl:
        d.append(f"A{bl},{bl} 0 0 1 {x0:.2f},{y1 - bl:.2f}")
    d.append(f"L{x0:.2f},{y0 + tl:.2f}")
    if tl:
        d.append(f"A{tl},{tl} 0 0 1 {x0 + tl:.2f},{y0:.2f}")
    return " ".join(d) + " Z"


def svg_open(x0, y0, x1, y1, page_x, page_y):
    w, h = x1 - x0 + 1.0, y1 - y0 + 1.0
    return (f'<svg class="strip" style="left:{page_x - 0.5}mm;top:{page_y - 0.5}mm;width:{w}mm;height:{h}mm" '
            f'viewBox="{x0 - 0.5} {y0 - 0.5} {w} {h}" xmlns="http://www.w3.org/2000/svg">')


def knob_strip(page_x, page_y):
    x0, y0, x1, y1 = LEFT_STRIP
    o = [svg_open(x0, y0, x1, y1, page_x, page_y)]
    o.append(f'<path d="{strip_path(x0, y0, x1, y1, True, False)}" class="cut"/>')
    # legend
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
        o.append(f'<rect x="{BTN_X0 - 0.4}" y="{by - 0.4:.2f}" width="{BTN_X1 - BTN_X0 + 0.8:.2f}" '
                 f'height="{BTN_H + 0.8}" rx="1.2" class="hole"/>')
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


TIMING = ["1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T",
          "sw 50", "sw 56", "sw 62", "sw 68", "sw 75", "<b>FILL</b>", "SYNC", "TAP"]
# root / scale picks (track: pad 1 = follow global; global: root pad 1 unused, scale pad 1 = follow preset)
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
SCALE_NAMES = ["chrom.", "major", "minor", "dorian", "phryg.", "lydian", "mixol.", "locrian",
               "harm.min", "pent.maj", "pent.min", "blues"]


def pick_text():
    return ", ".join(["1 follow"] + [f"{i + 2} {NOTE_NAMES[i]} / {SCALE_NAMES[i]}" for i in range(12)])


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
   <li><b>Shift + Note Repeat</b> enter / leave. The Note Repeat LED flashes on the beat.</li>
   <li><b>PAD BANK</b> tap: Step &harr; Play view (LED on = Play).</li>
   <li><b>Hold PAD BANK</b>: track layer (activity flashes, muted dim): pad = select track,
    <b>+ Stop + pad</b> = mute (playback goes on), <b>+ Shift + Stop + pad</b> = clear the track.</li>
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
   <li>Pads play the track from its base note in its scale (chromatic = drum kit). Root pads brighter.</li>
   <li><b>Rec</b> + Play: live recording, snapped to the nearest step (K4 = strength).
    Hits on a step that already plays add chord notes (up to 4).</li>
  </ul>
  <h3>Transport &amp; sync</h3>
  <ul>
   <li><b>Play</b>: start / continue. <b>Stop</b>: stop; press again = back to the top.</li>
   <li>Sync on (timing pad 15): the DAW's Start / Stop / clock drive the pad. Otherwise the pad sends clock.</li>
   <li><b>Play early</b> (KNOB BANK K8) sends notes ahead to cancel DAW / Bluetooth latency.</li>
  </ul>
  <h3>Root &amp; scale</h3>
  <ul>
   <li><b>KNOB BANK K3 / K4</b>: this track's root / scale (pad 1 = follow global).
    <b>Shift + K3 / K4</b>: global, also in normal mode (scale pad 1 = follow the preset, = chromatic here).</li>
   <li>Pads: {pick_text()}.</li>
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
   <li><b>Amounts</b> (volume, velocity, probability, quantize, steps, tempo, swing, play early 0&ndash;63 ms): bar from pad 1, dim &rarr; full. Micro-timing: centre-out.</li>
   <li><b>Choices</b> light one pad: rate (pads 1&ndash;8 = printed 1/4&hellip;1/32T), ratchet rate, trig condition, root, scale.</li>
   <li><b>Counts</b>: MIDI channel (1 pad = ch 1 &hellip; 16 = ch 16), gate.</li>
   <li>Base note / step note: pads 1&ndash;12 = keyboard, top row = octave. Ratchet count: pads 1&ndash;4.</li>
   <li>Trig conditions (K7): always, 1:2, 2:2, 1:3, 2:3, 3:3, 1:4&hellip;4:4, first, fill, not fill.</li>
   <li>Ratchet rate (K6): auto (split the step), 1/8, 1/16, 1/16T, 1/32, 1/32T, 1/64.</li>
  </ul>
  <p class="foot">&dagger; pattern memory: Shift + pad = pattern, Shift + Rec = save (later build).</p>
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
.card { position: absolute; left: 116mm; top: 44mm; width: 172mm; border: 0.3mm solid #111;
  border-radius: 3mm; padding: 3mm 3.5mm; display: flex; gap: 4mm; font-size: 2.65mm; line-height: 1.3; }
.col { flex: 1; min-width: 0; }
.card h2 { font-size: 3.4mm; margin: 0 0 1mm; }
.card h3 { font-size: 2.8mm; margin: 2.2mm 0 0.8mm; border-bottom: 0.2mm solid #ccc; }
.card ul { margin: 0; padding-left: 3.2mm; }
.card li { margin: 0.35mm 0; }
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


def ruler():
    ticks = []
    for mm in range(0, 101):
        h = 3.0 if mm % 10 == 0 else (2.0 if mm % 5 == 0 else 1.2)
        ticks.append(f'<line x1="{mm}" y1="0" x2="{mm}" y2="{h}" stroke="#111" stroke-width="0.18"/>')
        if mm % 10 == 0:
            ticks.append(f'<text x="{mm}" y="5.4" font-size="2" text-anchor="middle">{mm // 10}</text>')
    ticks.append('<text x="102" y="2.6" font-size="2.2">100 mm: check with a ruler</text>')
    return (f'<svg class="ruler" style="width:140mm;height:7mm" viewBox="-1 -0.5 140 7" '
            f'xmlns="http://www.w3.org/2000/svg"><line x1="0" y1="0" x2="100" y2="0" stroke="#111" '
            f'stroke-width="0.25"/>{"".join(ticks)}</svg>')


def build():
    left = knob_strip(10.0, 9.0)
    right = button_strip(80.0, 9.0)
    head = """<div class="head"><h1>SMC-PAD sequencer overlay</h1>
<p>Print at <b>100 % / actual size</b> on card, then check the ruler below. Cut along the dashed outlines and cut out
the grey holes. The left strip goes over the knobs (pull the knob caps off if they don't pass through), the right
strip next to the buttons. The pads stay uncovered: their printed timing labels already match the timing page,
except pad 14 (FILL, use the sticker).</p>
<p>The sizes come from the drawing in the manual. Measure your unit before you cut. Firmware 913 and later.</p></div>"""
    sticker = ('<div class="sticker">FILL</div><div class="stickerlab">&larr; sticker for pad 14: covers the '
               'printed &ldquo;LATCH&rdquo; label</div>')
    page = f'<div class="page">{left}{right}{head}{ruler()}{sticker}{card()}</div>'
    return (f'<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><title>SMC-PAD sequencer overlay'
            f'</title><meta name="description" content="Printable 1:1 overlay for the SMC-PAD sequencer">'
            f'<style>{CSS}</style></head><body>{page}</body></html>')


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    html_path = os.path.join(OUT_DIR, "smcpad_seq_overlay.html")
    with open(html_path, "w", encoding="utf-8") as f:
        f.write(build())
    print(html_path)
    if "--pdf" in sys.argv:
        pdf_path = os.path.join(OUT_DIR, "smcpad_seq_overlay.pdf")
        subprocess.run([CHROME, "--headless", "--disable-gpu", "--no-pdf-header-footer",
                        f"--print-to-pdf={pdf_path}", "file://" + html_path], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print(pdf_path)


if __name__ == "__main__":
    main()
