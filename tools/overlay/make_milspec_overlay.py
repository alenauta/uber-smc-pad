#!/usr/bin/env python3
"""360-degree full-device Print & Cut overlay generator for SMC-PAD (FW 925+).

Generates 1:1 millimeter Print & Cut assets for Silhouette Cameo 5 + Silhouette Studio
(US Letter 8.5 x 11 in Landscape, A4 Landscape, and 12x12 in) as well as Linux Inkscape
(`fablabnbg/inkscape-silhouette` on US Letter 215.9 x 279.4 mm), with two clean themes
(`light` and `dark`).

All CAD coordinates (top panel + 23.8 mm flat vertical side walls + R=10.66 mm corner arcs +
recessed 48.6 x 15.25 mm rear I/O well + POWER / LED / USB-C / 3.5mm TRS MIDI OUT) were extracted
directly from the SolidWorks vector wireframes in SMC-PAD_ZE.pdf.
"""
import argparse
import html
import json
import math
import os
import shutil
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_DIR = os.path.join(ROOT, "docs", "overlay")

# ---- 1:1 CAD Geometry (mm) extracted from SMC-PAD_ZE.pdf ----------------------------------
BODY_W, BODY_H = 227.0, 147.0
PANEL_RECT = (2.50, 2.50, 224.50, 144.50)   # 222.0 x 142.0 mm recessed top face inside 2.5 mm lip
CORNER_R = 9.00                             # recessed top face corner radius
OUTER_CORNER_R = 10.66                      # outer chassis vertical corner radius (tangent at 10.66 mm)
CORNER_ARC = 0.5 * math.pi * OUTER_CORNER_R # 16.745 mm arc length per 90-deg vertical corner

FLAT_FB_W = BODY_W - 2 * OUTER_CORNER_R     # 205.68 mm flat front/back span between corner tangents
FLAT_LR_W = BODY_H - 2 * OUTER_CORNER_R     # 125.68 mm flat left/right span between corner tangents
SIDE_H = 23.80                              # flat vertical side wall (CAD z=3.15..27.10 mm = 23.95 mm)
SIDE_R = 1.00                               # end corner radius of side bands
# Symmetric 1-corner pinwheel wrap: each band wraps 1 adjacent 90-deg corner (u=0..16.745 mm) + flat wall,
# placing all 4 seams on flat walls while keeping max length at 222.425 mm (< 230 mm US Letter safe height!)
FB_BAND_W = CORNER_ARC + FLAT_FB_W          # 222.425 mm (CNR + 205.68 mm flat Rear/Front wall)
LR_BAND_W = CORNER_ARC + FLAT_LR_W          # 142.425 mm (CNR + 125.68 mm flat Left/Right wall)

# Cameo US Letter (8.5 x 11 in) + Linux Inkscape `inkscape-silhouette` registration mark geometry
LETTER_W, LETTER_H = 215.90, 279.40
MIE_REG_LEFT, MIE_REG_TOP = 15.00, 15.00
MIE_REG_X, MIE_REG_Y = 185.90, 230.00       # Y=230mm leaves 34.4mm bottom grip margin on Cameo 5 rollers

# Top panel controls (in 227 x 147 mm body coordinates)
KNOB_X = (18.60, 48.70)
KNOB_Y = (24.50, 57.40, 90.40, 123.30)
KNOB_HOLE_R = 9.75

PAD_X = (80.92, 113.95, 146.98, 180.00)
PAD_Y = (24.26, 57.13, 90.00, 122.88)
PAD_W, PAD_H = 28.80, 28.70
PAD_R = 1.20
PAD_CENTER_R = 3.20

BTN_X0, BTN_X1 = 202.90, 215.00
BTN_Y0, BTN_PITCH, BTN_H = 10.80, 13.00, 9.50
BTN_CLEAR = 0.40
BTN_R = 1.20

# Rear I/O recessed stadium well on Piece 2 (FB_BAND_W x SIDE_H local coords, u=0 at Back-Right end)
# CAD: x_rear=[165.29, 213.89] (cx=189.59, w=48.60), z_desk=[7.34, 22.59] (cz=14.965, h=15.25)
# Strip mapping: u = CORNER_ARC + (x_rear - OUTER_CORNER_R) = x_rear + 6.085; v = 27.00 - z_desk
IO_WELL_CX = 189.59 + (CORNER_ARC - OUTER_CORNER_R)   # 195.675 mm
IO_WELL_CY = 27.00 - 14.965                           # 12.035 mm
IO_WELL_W = 49.10                                     # 48.60 + 0.50 mm clearance
IO_WELL_H = 15.70                                     # 15.25 + 0.45 mm clearance
IO_WELL_R = 5.70

# Rear I/O drop-in insert sticker (fits flat 46.64 x 13.28 mm floor inside the recessed well)
IO_INS_W, IO_INS_H, IO_INS_R = 46.00, 12.80, 4.20
# Individual port cutouts relative to insert top-left (center = (23.00, 6.40) <-> (x_rear=189.59, z=14.94)):
#   POWER switch: cx=173.68, cz=12.71, CAD 9.39x5.34 -> 9.80 x 5.70 mm, R=1.50
#   Charge LED:   cx=184.14, cz=15.41, CAD d=1.80    -> circle R=1.15 mm (d=2.30 mm)
#   USB-C port:   cx=192.42, cz=14.90, CAD 9.73x4.00 -> 10.10 x 4.40 mm, R=1.65
#   MIDI OUT TRS: cx=206.90, cz=14.01, CAD d=5.89    -> circle R=3.15 mm (d=6.30 mm)
IO_PORTS = {
    "pwr":  (23.00 + (173.68 - 189.59), 6.40 - (12.71 - 14.94), 9.80, 5.70, 1.50),
    "led":  (23.00 + (184.14 - 189.59), 6.40 - (15.41 - 14.94), 1.15),
    "usb":  (23.00 + (192.42 - 189.59), 6.40 - (14.90 - 14.94), 10.10, 4.40, 1.65),
    "midi": (23.00 + (206.90 - 189.59), 6.40 - (14.01 - 14.94), 3.15),
}

# Alignment frames (100% inside Silhouette Studio Type-1 default registration mark safe zones!)
FRAME_LTR_W, FRAME_LTR_H = 240.00, 154.00   # Cameo US Letter Landscape (279.4 x 215.9 mm)
FRAME_A4_W, FRAME_A4_H = 250.00, 165.00     # A4 Landscape (297.0 x 210.0 mm)
FRAME_12_W, FRAME_12_H = 256.00, 284.00     # 12x12 in (304.8 x 304.8 mm)

# ---- Simple Light & Dark Themes ------------------------------------------------------------
THEMES = {
    "light": {
        "label": "Light",
        "bg": "#FFFFFF",
        "panel_sub": "#F8FAFC",
        "box_bg": "#F8FAFC",
        "box_alt": "#F1F5F9",
        "grid_min": "#EDF2F7",
        "grid_maj": "#CBD5E1",
        "trace": "#94A3B8",
        "rule": "#475569",
        "rule_hi": "#0F172A",
        "ink": "#0F172A",
        "muted": "#475569",
        "step": "#EA580C",
        "step_fg": "#FFFFFF",
        "pat": "#0284C7",
        "warn": "#CA8A04",
        "ok": "#16A34A",
    },
    "dark": {
        "label": "Dark",
        "bg": "#13171D",
        "panel_sub": "#171C24",
        "box_bg": "#1B222C",
        "box_alt": "#10141A",
        "grid_min": "#1C232E",
        "grid_maj": "#252F3D",
        "trace": "#2E3B4E",
        "rule": "#4A5B73",
        "rule_hi": "#7C91AD",
        "ink": "#ECF0F5",
        "muted": "#8C9BAE",
        "step": "#FF7A1A",
        "step_fg": "#111317",
        "pat": "#38BDF8",
        "warn": "#FACC15",
        "ok": "#34D399",
    },
}

# ---- Firmware 925 Telemetry Tables --------------------------------------------------------
# Knob -> (Track Primary, Hold-Step P-Lock, Knob-Bank Pattern/Track, Shoulder Shortcut, Is Accent)
KNOB_DATA = {
    7: ("STEPS [1-64]",    "TRIG COND",     "MIDI CH [1-16]", "LEN 1–64",   False),
    8: ("RATE [⇧SPEED]",   "STEP TIE",      "SYNC [±120ms]",  "⇧+K8 SPEED", True),
    5: ("PROB [0-100%]",   "STEP PROB",     "— [UNASSIGNED]", "PROB %",     False),
    6: ("RATCHET RATE",    "STEP RATCHET",  "TRACK COLOUR",   "RATCH DIV",  False),
    3: ("BASE NOTE",       "NOTE / CHORD",  "TRACK ROOT",     "NR:BPM ⇧RT", True),
    4: ("QUANT [0-100%]",  "MICRO [±48]",   "TRACK SCALE",    "NR:LTC ⇧SC", True),
    1: ("VOLUME [0-127]",  "VELOCITY",      "TEMPO [40-200]", "NR: RATE",   True),
    2: ("GATE [1/8-4S]",   "LEN [1/8-32S]", "SWING [50-75%]", "NR: SWING",  True),
}
KNOB_POS = {7: (0, 0), 8: (1, 0), 5: (0, 1), 6: (1, 1), 3: (0, 2), 4: (1, 2), 1: (0, 3), 2: (1, 3)}

TIMING_PLAIN = [
    "1/4", "1/4T", "1/8", "1/8T",
    "1/16", "1/16T", "1/32", "1/32T",
    "SW 50%", "SW 56%", "SW 62%", "SW 68%",
    "SW 75%", "FILL", "SYNC", "TAP",
]

# Verified against smcpad_preset.c shift_pad()!
SHIFT_PAD_MAP = {
    0: "⇧ PST 1·PRF", 1: "⇧ PST 2·DAW", 2: "⇧ PRESET 3",  3: "⇧ PRESET 4",
    4: "⇧ PRESET 5",  5: "⇧ PRESET 6",  6: "⇧ PRESET 7",  7: "⇧ PRESET 8",
    8: "⇧ VEL 1·SFT", 9: "⇧ VEL 2·MED", 10: "⇧ VEL 3·HRD", 11: "⇧ VEL 4·MAX",
    12: "⇧ TRANS −1", 13: "⇧ TRANS +1", 14: "⇧ OCTAVE −1", 15: "⇧ OCTAVE +1",
}

PAD_SUB_META = {
    0: "C · CHROM",   1: "C# · MAJOR",  2: "D · MINOR",    3: "D# · DORIAN",
    4: "E · PHRYG",   5: "F · LYDIAN",  6: "F# · MIXOL",   7: "G · LOCRIAN",
    8: "G# · H.MIN",  9: "A · P.MAJ",   10: "A# · P.MIN",  11: "B · BLUES",
    12: "PAD 13 · C3", 13: "PAD 14 · C#3", 14: "PAD 15 · D3", 15: "⇧15+16 = RST",
}

# 10 Hardware Buttons: (stock_name, left_lbl, right_l1, right_l2, under_bar_text)
BTN_SPECS = [
    ("BT",          "BT",   "BLE",   "OTA",   "HOLD: BLE MIDI + OTA"),
    ("PAD BANK",    "BANK", "TRKS",  "COPY",  "TAP: VIEW · HLD: TRACKS"),
    ("KNOB BANK",   "KNOB", "PAT",   "BANK",  "KNOB BANK (LED = PAT)"),
    ("<",           "PG <", "⇧ + <", "UNDO",  "STEP PAGE <  ·  ⇧ UNDO"),
    (">",           "PG >", "⇧ + >", "REDO",  "STEP PAGE >  ·  ⇧ REDO"),
    ("PLAY",        "PLAY", "⇧+PAD", "LOAD",  "PLAY / CONT · ⇧+PAD LOAD"),
    ("STOP",        "STOP", "×2",    "SPP 0", "STOP+SAVE · ×2 REW SPP0"),
    ("REC",         "REC",  "⇧+PAD", "SAVE",  "LIVE REC  ·  ⇧+PAD SAVE"),
    ("SHIFT",       "SHFT", "+PAD",  "MENU",  "⇧+PAD MENU · +K3/4 SCL"),
    ("NOTE REPEAT", "RPT",  "⇧+RPT", "SEQ",   "HLD: TIMING · ⇧ SEQ MODE"),
]


def esc(s):
    return html.escape(str(s), quote=True)


def rounded_rect_path(x0, y0, x1, y1, r_tl, r_tr, r_br, r_bl):
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


def rrect(x, y, w, h, r):
    return rounded_rect_path(x, y, x + w, y + h, r, r, r, r)


def pad_corners(col, row_from_top):
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


def pad_hole_path(col, row_from_top, ox=0.0, oy=0.0):
    cx, cy = ox + PAD_X[col], oy + PAD_Y[row_from_top]
    tl, tr, br, bl = pad_corners(col, row_from_top)
    return rounded_rect_path(cx - PAD_W / 2, cy - PAD_H / 2, cx + PAD_W / 2, cy + PAD_H / 2, tl, tr, br, bl)


def corner_brackets(x0, y0, x1, y1, arm=1.4, cls="sc-rule-hi", sw=0.18, corners=("tl", "tr", "br", "bl")):
    segs = []
    if "tl" in corners:
        segs.append(f"M{x0:.2f},{y0+arm:.2f} L{x0:.2f},{y0:.2f} L{x0+arm:.2f},{y0:.2f}")
    if "tr" in corners:
        segs.append(f"M{x1-arm:.2f},{y0:.2f} L{x1:.2f},{y0:.2f} L{x1:.2f},{y0+arm:.2f}")
    if "bl" in corners:
        segs.append(f"M{x0:.2f},{y1-arm:.2f} L{x0:.2f},{y1:.2f} L{x0+arm:.2f},{y1:.2f}")
    if "br" in corners:
        segs.append(f"M{x1-arm:.2f},{y1:.2f} L{x1:.2f},{y1:.2f} L{x1:.2f},{y1-arm:.2f}")
    return f'<path d="{" ".join(segs)}" fill="none" class="{cls}" stroke-width="{sw}"/>'


def crosshair(cx, cy, r=1.5, cls="sc-rule"):
    return (
        f'<g class="{cls}" fill="none" stroke-width="0.13">'
        f'<circle cx="{cx:.2f}" cy="{cy:.2f}" r="{r*0.62:.2f}"/>'
        f'<line x1="{cx-r:.2f}" y1="{cy:.2f}" x2="{cx+r:.2f}" y2="{cy:.2f}"/>'
        f'<line x1="{cx:.2f}" y1="{cy-r:.2f}" x2="{cx:.2f}" y2="{cy+r:.2f}"/>'
        f'</g>'
    )


def svg_theme_css(theme_key="light"):
    t = THEMES[theme_key]
    return f"""
    .cut-line {{ fill: none; stroke: #ff0000; stroke-width: 0.12; stroke-linecap: round; stroke-linejoin: round; }}
    .align-frame {{ fill: none; stroke: #0066ff; stroke-width: 0.10; stroke-dasharray: 2.0 1.2; }}
    .sheet-paper-bg {{ fill: #ffffff; stroke: #cbd5e1; stroke-width: 0.16; }}
    .sheet-annotations text {{ fill: #334155 !important; }}
    .sheet-ruler .box-bg {{ fill: #ffffff !important; fill-opacity: 1 !important; stroke: #0f172a !important; }}
    .sheet-ruler .ink, .sheet-ruler .step-c, .sheet-ruler .pat-c, .sheet-ruler .warn-c {{ fill: #0f172a !important; }}
    .sheet-ruler .sc-rule, .sheet-ruler .sc-rule-hi {{ stroke: #0f172a !important; }}
    .cutout-preview-mask {{ display: none; fill: #0b0e14; fill-opacity: 0.76; stroke: #64748b; stroke-width: 0.14; pointer-events: none; }}
    .seam-tick {{ fill: none; stroke: {t['warn']}; stroke-width: 0.20; stroke-dasharray: 0.9 0.7; }}
    .bg-base {{ fill: {t['bg']}; }}
    .bg-veil {{ fill: {t['bg']}; fill-opacity: 0; pointer-events: none; }}
    .panel-sub {{ fill: {t['panel_sub']}; }}
    .box-bg {{ fill: {t['box_bg']}; stroke: {t['rule']}; stroke-width: 0.16; }}
    .box-alt {{ fill: {t['box_alt']}; stroke: {t['rule']}; stroke-width: 0.15; }}
    .cad-grid-lines {{ display: none; }}
    .grid-min {{ stroke: {t['grid_min']}; stroke-width: 0.07; fill: none; }}
    .grid-maj {{ stroke: {t['grid_maj']}; stroke-width: 0.12; fill: none; }}
    .sc-trace {{ stroke: {t['trace']}; stroke-width: 0.18; fill: none; stroke-linecap: round; stroke-linejoin: round; }}
    .sc-rule {{ stroke: {t['rule']}; fill: none; }}
    .sc-rule-hi {{ stroke: {t['rule_hi']}; fill: none; }}
    .ink {{ fill: {t['ink']}; }}
    .muted {{ fill: {t['muted']}; }}
    .step-c {{ fill: {t['step']}; }}
    .step-bg {{ fill: {t['step']}; }}
    .step-fg {{ fill: {t['step_fg']}; }}
    .pat-c {{ fill: {t['pat']}; }}
    .warn-c {{ fill: {t['warn']}; }}
    .warn-stroke {{ stroke: {t['warn']}; fill: none; }}
    .ok-c {{ fill: {t['ok']}; }}
    .f-cond {{ font-family: "Roboto Condensed", "Avenir Next Condensed", "Bahnschrift", "Liberation Sans Narrow", "Arial Narrow", sans-serif; }}
    .f-mono {{ font-family: "Noto Sans Mono", "JetBrains Mono", "SF Mono", "DejaVu Sans Mono", "Liberation Mono", monospace; }}
    """


def gapped_panel_border_path(bx0, by0, bx1, by1, r, top_gaps=(), bot_gaps=()):
    """Rounded rect border with clean vector breaks along top (by0) and bottom (by1) straight segments."""
    x_l, x_r = bx0 + r, bx1 - r
    d = [f"M{x_l:.3f},{by0:.3f}"]
    cur_x = x_l
    for g0, g1 in sorted(top_gaps):
        c0, c1 = max(x_l, g0), min(x_r, g1)
        if c1 > c0:
            if c0 > cur_x:
                d.append(f"L{c0:.3f},{by0:.3f}")
            d.append(f"M{c1:.3f},{by0:.3f}")
            cur_x = c1
    if cur_x < x_r:
        d.append(f"L{x_r:.3f},{by0:.3f}")
    d.append(f"A{r:.3f},{r:.3f} 0 0 1 {bx1:.3f},{by0 + r:.3f}")
    d.append(f"L{bx1:.3f},{by1 - r:.3f}")
    d.append(f"A{r:.3f},{r:.3f} 0 0 1 {x_r:.3f},{by1:.3f}")
    cur_x = x_r
    for g0, g1 in sorted(bot_gaps, reverse=True):
        c0, c1 = max(x_l, g0), min(x_r, g1)
        if c1 > c0:
            if cur_x > c1:
                d.append(f"L{c1:.3f},{by1:.3f}")
            d.append(f"M{c0:.3f},{by1:.3f}")
            cur_x = c0
    if cur_x > x_l:
        d.append(f"L{x_l:.3f},{by1:.3f}")
    d.append(f"A{r:.3f},{r:.3f} 0 0 1 {bx0:.3f},{by1 - r:.3f}")
    d.append(f"L{bx0:.3f},{by0 + r:.3f}")
    d.append(f"A{r:.3f},{r:.3f} 0 0 1 {x_l:.3f},{by0:.3f}")
    return " ".join(d)


def cad_grid_group(clip_id, x0, y0, w, h, sheet_slot="s1", piece_kind="top", ref_rect=None):
    """Detached background layer clipped to sticker bleed: solid color + user image slot + contrast veil + CAD grid."""
    rx, ry, rw, rh = ref_rect if ref_rect is not None else (x0, y0, w, h)
    o = [
        f'<g class="piece-bg-layer" clip-path="url(#{clip_id})" '
        f'data-sheet="{sheet_slot}" data-piece="{piece_kind}" '
        f'data-rx="{rx:.2f}" data-ry="{ry:.2f}" data-rw="{rw:.2f}" data-rh="{rh:.2f}">'
    ]
    o.append(f'<rect x="{x0:.2f}" y="{y0:.2f}" width="{w:.2f}" height="{h:.2f}" class="bg-base"/>')
    o.append('<g class="bg-image-slot"></g>')
    o.append(f'<rect x="{x0:.2f}" y="{y0:.2f}" width="{w:.2f}" height="{h:.2f}" class="bg-veil"/>')
    # 5 mm minor grid + 25 mm major grid (toggleable independently of foreground/background)
    ix0, ix1 = int(math.floor(x0 / 5.0)) * 5, int(math.ceil((x0 + w) / 5.0)) * 5
    iy0, iy1 = int(math.floor(y0 / 5.0)) * 5, int(math.ceil((y0 + h) / 5.0)) * 5
    min_d, maj_d = [], []
    for gx in range(ix0, ix1 + 1, 5):
        target = maj_d if (gx % 25 == 0) else min_d
        target.append(f"M{gx},{y0:.1f}V{y0+h:.1f}")
    for gy in range(iy0, iy1 + 1, 5):
        target = maj_d if (gy % 25 == 0) else min_d
        target.append(f"M{x0:.1f},{gy}H{x0+w:.1f}")
    o.append('<g class="cad-grid-lines">')
    if min_d:
        o.append(f'<path d="{" ".join(min_d)}" class="grid-min"/>')
    if maj_d:
        o.append(f'<path d="{" ".join(maj_d)}" class="grid-maj"/>')
    o.append('</g></g>')
    return "\n".join(o)


def render_top_panel(ox, oy, bleed=1.50, include_print=True, include_cut=True, uid="tp", sheet_slot="s1", ref_rect=None):
    """Render Piece 1: 222.0 x 142.0 mm Top Panel shifted so body (0,0) is at (ox, oy)."""
    px0, py0, px1, py1 = PANEL_RECT[0] + ox, PANEL_RECT[1] + oy, PANEL_RECT[2] + ox, PANEL_RECT[3] + oy
    outer_cut = rounded_rect_path(px0, py0, px1, py1, CORNER_R, CORNER_R, CORNER_R, CORNER_R)
    bleed_path = rounded_rect_path(
        px0 - bleed, py0 - bleed, px1 + bleed, py1 + bleed,
        CORNER_R + bleed, CORNER_R + bleed, CORNER_R + bleed, CORNER_R + bleed
    )
    o = []

    if include_print:
        clip_id = f"{uid}-bleed-clip"
        haz_id = f"{uid}-haz-pat"
        o.append(
            f'<defs>'
            f'<clipPath id="{clip_id}"><path d="{bleed_path}"/></clipPath>'
            f'<pattern id="{haz_id}" width="2.4" height="2.4" patternTransform="rotate(45)" patternUnits="userSpaceOnUse">'
            f'<rect width="1.1" height="2.4" class="warn-c" opacity="0.22"/></pattern>'
            f'</defs>'
        )
        o.append(f'<g id="{uid}-print" class="f-cond">')
        o.append(cad_grid_group(
            clip_id, px0 - bleed, py0 - bleed, (px1 - px0) + 2 * bleed, (py1 - py0) + 2 * bleed,
            sheet_slot=sheet_slot, piece_kind="top", ref_rect=ref_rect,
        ))
        o.append(f'<g id="{uid}-fg" class="piece-fg-layer">')

        # Inner perimeter border (inset 1.05 mm inside cut line, cleanly gapped around bottom label)
        bx0, by0, bx1, by1 = px0 + 1.05, py0 + 1.05, px1 - 1.05, py1 - 1.05
        bot_gaps = [(ox + 20.10, ox + 47.20)]
        o.append(f'<path d="{gapped_panel_border_path(bx0, by0, bx1, by1, CORNER_R-1.05, (), bot_gaps)}" '
                 f'class="sc-rule" stroke-width="0.18"/>')

        # Vertical subsystem dividers: Knobs | Pads | Buttons
        div1_x = ox + 64.55
        div2_x = ox + 195.85
        o.append(f'<line x1="{div1_x:.2f}" y1="{by0+0.6:.2f}" x2="{div1_x:.2f}" y2="{by1-0.6:.2f}" class="sc-rule" stroke-width="0.22"/>')
        o.append(f'<line x1="{div2_x:.2f}" y1="{by0+0.6:.2f}" x2="{div2_x:.2f}" y2="{by1-0.6:.2f}" class="sc-rule" stroke-width="0.22"/>')

        o.append(f'<text x="{ox+34.85:.2f}" y="{oy+111.55:.2f}" class="f-cond warn-c" font-size="1.45" font-weight="700" text-anchor="middle">'
                 f'RPT + K1–4 LIVE</text>')

        # ---- SEC-01: Left Encoder Title Block (concentric R=6.65mm TL curve inside R=9.00mm panel corner) ----
        tx0, ty0, tw, th = ox + 4.85, oy + 4.85, 57.95, 8.25
        o.append(f'<path d="{rounded_rect_path(tx0, ty0, tx0 + tw, ty0 + th, 6.65, 0.80, 0.80, 0.80)}" class="box-bg"/>')
        o.append(corner_brackets(tx0, ty0, tx0 + tw, ty0 + th, arm=1.2, corners=("tr", "br", "bl")))
        o.append(f'<line x1="{tx0+0.55:.2f}" y1="{ty0+4.20:.2f}" x2="{tx0+tw:.2f}" y2="{ty0+4.20:.2f}" class="sc-rule" stroke-width="0.13"/>')
        o.append(f'<text x="{tx0+3.95:.2f}" y="{ty0+3.02:.2f}" class="ink" font-size="2.22" font-weight="700" letter-spacing="0.14">'
                 f'ENCODERS K1–K8</text>'
                 f'<text x="{tx0+tw-1.2:.2f}" y="{ty0+2.98:.2f}" class="f-cond pat-c" font-size="1.78" font-weight="700" text-anchor="end">'
                 f'SMC-PAD</text>')
        # 3-Mode Color Legend
        o.append(f'<circle cx="{tx0+2.5:.2f}" cy="{ty0+6.3:.2f}" r="0.88" class="ink"/>')
        o.append(f'<text x="{tx0+4.0:.2f}" y="{ty0+7.02:.2f}" class="ink" font-size="1.92" font-weight="700">TRACK</text>')
        o.append(f'<rect x="{tx0+16.9:.2f}" y="{ty0+5.42:.2f}" width="1.75" height="1.75" rx="0.25" class="step-bg"/>')
        o.append(f'<text x="{tx0+19.3:.2f}" y="{ty0+7.02:.2f}" class="step-c" font-size="1.92" font-weight="700">HOLD STEP</text>')
        o.append(f'<polygon points="{tx0+38.6:.2f},{ty0+5.38:.2f} {tx0+39.65:.2f},{ty0+7.22:.2f} {tx0+37.55:.2f},{ty0+7.22:.2f}" class="pat-c"/>')
        o.append(f'<text x="{tx0+40.3:.2f}" y="{ty0+7.02:.2f}" class="pat-c" font-size="1.92" font-weight="700">KNOB BANK</text>')

        # ---- 8 Rotary Encoders + Vernier Dials + 3-Row Telemetry Readouts ----
        for k, (col, row) in KNOB_POS.items():
            cx, cy = ox + KNOB_X[col], oy + KNOB_Y[row]
            trk_txt, stp_txt, pat_txt, sh_lbl, is_acc = KNOB_DATA[k]

            # Vernier graduation ticks around hole (skipping shoulder badge ticks 6..7 and shortcut ticks 12..13)
            ticks = []
            for ti in range(21):
                if ti in (6, 7, 12, 13):
                    continue
                ang = math.radians(135.0 + ti * (270.0 / 20.0))
                r_in = 10.08
                r_out = 11.08 if (ti % 5 == 0) else 10.62
                x_a, y_a = cx + r_in * math.cos(ang), cy + r_in * math.sin(ang)
                x_b, y_b = cx + r_out * math.cos(ang), cy + r_out * math.sin(ang)
                ticks.append(f"M{x_a:.2f},{y_a:.2f}L{x_b:.2f},{y_b:.2f}")
            o.append(f'<path d="{" ".join(ticks)}" class="sc-rule" stroke-width="0.16"/>')

            # Stenciled Knob ID tag (top-left shoulder)
            bx, by = cx - 9.85, cy - 9.45
            o.append(f'<rect x="{bx-2.85:.2f}" y="{by-1.90:.2f}" width="5.7" height="3.7" rx="0.6" class="step-bg"/>')
            o.append(f'<text x="{bx:.2f}" y="{by+0.72:.2f}" class="f-cond step-fg" font-size="2.45" font-weight="700" text-anchor="middle">K{k}</text>')

            # Bold secondary shortcut on top-right shoulder (clear of vernier ticks)
            sh_cls = "warn-c" if is_acc else "muted"
            o.append(f'<text x="{cx+13.15:.2f}" y="{cy-8.95:.2f}" class="f-cond {sh_cls}" font-size="1.70" font-weight="700" text-anchor="end">{esc(sh_lbl)}</text>')

            # 3-Row Telemetry Box below knob (K1 at BL uses concentric r_bl=6.65mm inside R=9.00mm panel corner!)
            kw, kh = 27.50, 8.80
            kx0 = (ox + 4.85) if (col == 0) else (cx - kw / 2)
            ky0 = cy + KNOB_HOLE_R + 0.30
            is_bl_corner = (k == 1)
            r_bl_k = 6.65 if is_bl_corner else 0.80
            o.append(f'<path d="{rounded_rect_path(kx0, ky0, kx0 + kw, ky0 + kh, 0.80, 0.80, 0.80, r_bl_k)}" class="box-bg"/>')
            o.append(corner_brackets(kx0, ky0, kx0 + kw, kx0 + kh, arm=1.0, sw=0.15,
                                     corners=("tl", "tr", "br") if is_bl_corner else ("tl", "tr", "br", "bl")))
            o.append(f'<line x1="{kx0+0.6:.2f}" y1="{ky0+2.94:.2f}" x2="{kx0+kw-0.6:.2f}" y2="{ky0+2.94:.2f}" class="grid-maj"/>')
            div2_x1 = (kx0 + 1.60) if is_bl_corner else (kx0 + 0.60)
            o.append(f'<line x1="{div2_x1:.2f}" y1="{ky0+5.86:.2f}" x2="{kx0+kw-0.6:.2f}" y2="{ky0+5.86:.2f}" class="grid-maj"/>')

            # Mode prefix badges (T / S / B) + parameter text
            rows_spec = [
                ("T", trk_txt, "ink", False),
                ("S", stp_txt, "step-c", False),
                ("B", pat_txt, "muted" if pat_txt.startswith("—") else "pat-c", pat_txt.startswith("—")),
            ]
            for ri, (tag, label, cls_c, is_dim) in enumerate(rows_spec):
                ry_txt = ky0 + 2.22 + ri * 2.92
                tag_x = (kx0 + 4.35) if (is_bl_corner and ri == 2) else (kx0 + 1.15)
                val_x = (kx0 + kw / 2 + 2.65) if (is_bl_corner and ri == 2) else (kx0 + kw / 2 + 1.35)
                o.append(f'<text x="{tag_x:.2f}" y="{ry_txt:.2f}" class="f-cond muted" font-size="1.78" font-weight="700">{tag}:</text>')
                o.append(f'<text x="{val_x:.2f}" y="{ry_txt:.2f}" class="{cls_c}" font-size="2.14" '
                         f'font-weight="{"600" if is_dim else "700"}" text-anchor="middle">{esc(label)}</text>')

        # Left bottom telemetry stamp framed along gapped by1 (oy + 143.45)
        o.append(f'<text x="{ox+33.65:.2f}" y="{by1+0.50:.2f}" class="f-cond muted" font-size="1.48" font-weight="700" text-anchor="middle">'
                 f'360° ENDLESS · ×4 ACCEL</text>')

        # ---- SEC-02: Center 4x4 Velocity/Step Matrix ----
        pad_x0 = ox + PAD_X[0] - PAD_W / 2
        pad_x1 = ox + PAD_X[3] + PAD_W / 2
        pad_span = pad_x1 - pad_x0

        # Top Tactical Command Ribbon (y = 4.25 .. 9.55 mm, above Row 0 pad cutouts at y=9.91 mm)
        hx0, hy0, hw, hh = pad_x0, oy + 4.25, pad_span, 5.30
        o.append(f'<rect x="{hx0:.2f}" y="{hy0:.2f}" width="{hw:.2f}" height="{hh:.2f}" rx="0.85" class="box-bg"/>')
        o.append(corner_brackets(hx0, hy0, hx0 + hw, hy0 + hh, arm=1.2))
        o.append(f'<line x1="{hx0:.2f}" y1="{hy0+2.65:.2f}" x2="{hx0+hw:.2f}" y2="{hy0+2.65:.2f}" class="sc-rule" stroke-width="0.12"/>')
        o.append(
            f'<text x="{hx0+hw/2:.2f}" y="{hy0+1.98:.2f}" class="ink" font-size="1.95" font-weight="700" text-anchor="middle">'
            f'<tspan class="step-c">HOLD STEP + LATER PAD = NOTE LEN [1–32S]</tspan>  ·  '
            f'HOLD STEP + RPT = RATCHET  ·  '
            f'<tspan class="pat-c">REC + STEP = CHORD</tspan></text>'
        )
        o.append(
            f'<text x="{hx0+hw/2:.2f}" y="{hy0+4.64:.2f}" class="ink" font-size="1.90" font-weight="700" text-anchor="middle">'
            f'<tspan class="warn-c">HOLD RPT = TIMING</tspan>  ·  '
            f'HOLD BANK = TRACKS [<tspan class="pat-c">SRC+DST = COPY</tspan> · +STOP = MUTE]  ·  '
            f'<tspan class="step-c">⇧+REC SAVE</tspan> / <tspan class="ok-c">⇧+PLAY LOAD</tspan></text>'
        )

        # 16 Pad Telemetry Strips in the 4.17 mm inter-pad bridges
        for r_top in range(4):
            for c_idx in range(4):
                cx, cy = ox + PAD_X[c_idx], oy + PAD_Y[r_top]
                step_num = r_top * 4 + c_idx + 1        # S01..S16 reading order
                pad_idx = (3 - r_top) * 4 + c_idx       # 0..15 (PAD 1 bottom-left .. PAD 16 top-right)
                timing_lbl = TIMING_PLAIN[pad_idx]
                shift_lbl = SHIFT_PAD_MAP[pad_idx]
                sub_meta = PAD_SUB_META[pad_idx]

                fx0 = cx - PAD_W / 2 + 0.20
                fw = PAD_W - 0.40
                fy0 = cy + PAD_H / 2 + 0.22
                fh = 3.68
                o.append(f'<rect x="{fx0:.2f}" y="{fy0:.2f}" width="{fw:.2f}" height="{fh:.2f}" rx="0.65" class="box-bg"/>')
                if timing_lbl == "FILL":
                    o.append(f'<rect x="{fx0:.2f}" y="{fy0:.2f}" width="{fw:.2f}" height="{fh:.2f}" rx="0.65" fill="url(#{haz_id})"/>')

                # Left Bold Step Badge (S01..S16)
                sbw = 6.70
                o.append(f'<rect x="{fx0+0.22:.2f}" y="{fy0+0.22:.2f}" width="{sbw:.2f}" height="{fh-0.44:.2f}" rx="0.45" class="step-bg"/>')
                o.append(f'<text x="{fx0+0.22+sbw/2:.2f}" y="{fy0+2.64:.2f}" class="f-cond step-fg" font-size="2.30" font-weight="700" text-anchor="middle">'
                         f'S{step_num:02d}</text>')

                # Center Timing Callout
                t_cls = "warn-c" if timing_lbl in ("FILL", "SYNC", "TAP") else "ink"
                o.append(f'<text x="{fx0+sbw+0.95:.2f}" y="{fy0+2.62:.2f}" class="{t_cls}" font-size="2.30" font-weight="700">'
                         f'{esc(timing_lbl)}</text>')

                # Right Stacked Dual Readout: Top = Shift Menu (Cyan), Bottom = Root/Scale Picker (Muted)
                o.append(f'<text x="{fx0+fw-0.55:.2f}" y="{fy0+1.58:.2f}" class="f-cond pat-c" font-size="1.70" font-weight="700" text-anchor="end">'
                         f'{esc(shift_lbl)}</text>')
                o.append(f'<text x="{fx0+fw-0.55:.2f}" y="{fy0+3.28:.2f}" class="f-cond muted" font-size="1.62" font-weight="700" text-anchor="end">'
                         f'{esc(sub_meta)}</text>')

        # Bottom Pad Matrix System Bus Bar (y = 141.32 .. 143.87 mm)
        bx_bar0, by_bar0, bw_bar, bh_bar = pad_x0, oy + 141.32, pad_span, 2.55
        o.append(f'<rect x="{bx_bar0:.2f}" y="{by_bar0:.2f}" width="{bw_bar:.2f}" height="{bh_bar:.2f}" rx="0.55" class="box-alt"/>')
        o.append(
            f'<text x="{bx_bar0+bw_bar/2:.2f}" y="{by_bar0+1.88:.2f}" class="f-cond muted" font-size="1.75" font-weight="700" text-anchor="middle">'
            f'960 PPQN ENGINE  ·  16 TRACKS × 64 STEPS  ·  USB-C + BLE + 3.5MM TRS MIDI OUT  ·  AUTO-SAVE ON STOP  ·  FW ≥ 925</text>'
        )

        # ---- SEC-03: Right Function Button Column (concentric R=6.65mm TR curve inside R=9.00mm panel corner) ----
        sec3_d = (
            f"M{ox+197.35:.3f},{oy+4.85:.3f} L{ox+215.50:.3f},{oy+4.85:.3f} "
            f"A6.650,6.650 0 0 1 {ox+221.86:.3f},{oy+9.55:.3f} "
            f"L{ox+197.35:.3f},{oy+9.55:.3f} A0.700,0.700 0 0 1 {ox+196.65:.3f},{oy+8.85:.3f} "
            f"L{ox+196.65:.3f},{oy+5.55:.3f} A0.700,0.700 0 0 1 {ox+197.35:.3f},{oy+4.85:.3f} Z"
        )
        o.append(f'<path d="{sec3_d}" class="box-bg"/>')
        o.append(f'<text x="{ox+208.1:.2f}" y="{oy+6.92:.2f}" class="ink" font-size="1.85" font-weight="700" text-anchor="middle">'
                 f'TRANSPORT</text>'
                 f'<text x="{ox+209.0:.2f}" y="{oy+8.88:.2f}" class="f-cond muted" font-size="1.52" font-weight="700" text-anchor="middle">'
                 f'MODE &amp; SEQ CONTROLS</text>')

        for i, (bname, llbl, rl1, rl2, ubar) in enumerate(BTN_SPECS):
            by = oy + BTN_Y0 + i * BTN_PITCH
            bcy = by + BTN_H / 2
            hx0_b = ox + BTN_X0 - BTN_CLEAR
            hx1_b = ox + BTN_X1 + BTN_CLEAR

            # Left bold button identifier + crisp leader tick (x = 196.4 .. 202.1 mm)
            o.append(f'<line x1="{hx0_b-0.95:.2f}" y1="{bcy:.2f}" x2="{hx0_b-0.22:.2f}" y2="{bcy:.2f}" class="sc-rule-hi" stroke-width="0.20"/>')
            l_cls = "step-c" if llbl in ("REC", "SHFT") else ("ok-c" if llbl == "PLAY" else "ink")
            o.append(f'<text x="{hx0_b-1.15:.2f}" y="{bcy+0.78:.2f}" class="f-cond {l_cls}" font-size="2.22" font-weight="700" text-anchor="end">'
                     f'{esc(llbl)}</text>')

            # Right framed Shift/Alt shortcut pill (x = 215.65 .. 222.25 mm)
            rx0_b = hx1_b + 0.25
            rw_b = (ox + 222.25) - rx0_b
            rh_b = 5.50
            ry0_b = bcy - rh_b / 2
            o.append(f'<rect x="{rx0_b:.2f}" y="{ry0_b:.2f}" width="{rw_b:.2f}" height="{rh_b:.2f}" rx="0.55" class="box-alt"/>')
            r1_cls = "step-c" if ("⇧" in rl1 or "×2" in rl1) else "pat-c"
            o.append(f'<text x="{rx0_b+rw_b/2:.2f}" y="{bcy-0.55:.2f}" class="f-cond {r1_cls}" font-size="1.74" font-weight="700" text-anchor="middle">'
                     f'{esc(rl1)}</text>')
            o.append(f'<text x="{rx0_b+rw_b/2:.2f}" y="{bcy+1.88:.2f}" class="f-cond ink" font-size="1.74" font-weight="700" text-anchor="middle">'
                     f'{esc(rl2)}</text>')

            # Full-width Under-Button Primary Label Bar in the 2.70 mm horizontal bridge below every button!
            # (B9 uses r_br=2.10 and x1=ox+220.85 so it stays strictly inside the R=6.65mm bottom-right corner arc)
            ub_x0 = ox + 196.45
            ub_x1 = (ox + 220.85) if (i == 9) else (ox + 222.25)
            ub_y0 = by + BTN_H + BTN_CLEAR + 0.20
            ub_h = 2.30
            ub_r_br = 2.10 if (i == 9) else 0.45
            o.append(f'<path d="{rounded_rect_path(ub_x0, ub_y0, ub_x1, ub_y0 + ub_h, 0.45, 0.45, ub_r_br, 0.45)}" class="box-bg"/>')
            ub_cls = "step-c" if ("SAVE" in ubar and "STOP" not in ubar) else ("pat-c" if "KNOB BANK" in ubar else "ink")
            o.append(f'<text x="{(ub_x0 + ub_x1)/2:.2f}" y="{ub_y0+1.74:.2f}" class="f-cond {ub_cls}" font-size="1.86" font-weight="700" text-anchor="middle">'
                     f'{esc(ubar)}</text>')

        o.append("</g>")  # end piece-fg-layer

        # Non-printing on-screen cutout hole preview mask so users framing background images see physical holes
        o.append(f'<g id="{uid}-hole-mask" class="cutout-preview-mask no-print">')
        for k, (col, row) in sorted(KNOB_POS.items()):
            o.append(f'<circle cx="{ox+KNOB_X[col]:.3f}" cy="{oy+KNOB_Y[row]:.3f}" r="{KNOB_HOLE_R:.3f}"/>')
        for r_top in range(4):
            for c_idx in range(4):
                o.append(f'<path d="{pad_hole_path(c_idx, r_top, ox, oy)}"/>')
        for i in range(len(BTN_SPECS)):
            by = oy + BTN_Y0 + i * BTN_PITCH
            o.append(f'<path d="{rrect(ox + BTN_X0 - BTN_CLEAR, by - BTN_CLEAR, (BTN_X1 - BTN_X0) + 2 * BTN_CLEAR, BTN_H + 2 * BTN_CLEAR, BTN_R)}"/>')
        o.append("</g>")
        o.append("</g>")  # end print group

    if include_cut:
        o.append(f'<g id="{uid}-cut">')
        o.append(f'<path id="{uid}-cut-outer" d="{outer_cut}" class="cut-line"/>')
        for k, (col, row) in sorted(KNOB_POS.items()):
            o.append(f'<circle id="{uid}-cut-knob-{k}" cx="{ox+KNOB_X[col]:.3f}" cy="{oy+KNOB_Y[row]:.3f}" r="{KNOB_HOLE_R:.3f}" class="cut-line"/>')
        for r_top in range(4):
            for c_idx in range(4):
                pnum = (3 - r_top) * 4 + c_idx + 1
                o.append(f'<path id="{uid}-cut-pad-{pnum}" d="{pad_hole_path(c_idx, r_top, ox, oy)}" class="cut-line"/>')
        for i in range(len(BTN_SPECS)):
            by = oy + BTN_Y0 + i * BTN_PITCH
            bx0_c = ox + BTN_X0 - BTN_CLEAR
            by0_c = by - BTN_CLEAR
            bw_c = (BTN_X1 - BTN_X0) + 2 * BTN_CLEAR
            bh_c = BTN_H + 2 * BTN_CLEAR
            o.append(f'<path id="{uid}-cut-btn-{i+1}" d="{rrect(bx0_c, by0_c, bw_c, bh_c, BTN_R)}" class="cut-line"/>')
        o.append("</g>")

    return "\n".join(o)


def render_corner_tab(ox, oy, h, tag, sub1, sub2, sub3="R10.7 · 90°", sub2_cls="pat-c", sub3_cls="muted"):
    """Render 1-corner 90-deg pinwheel wrap tab on left end (u = 0..CORNER_ARC) + dashed tangent seam tick."""
    u_t1 = ox + CORNER_ARC
    ux = ox + 0.90
    cw = CORNER_ARC - 1.70
    cx_c = ux + cw / 2.0
    return (
        f'<rect x="{ux:.2f}" y="{oy+1.4:.2f}" width="{cw:.2f}" height="{h-2.8:.2f}" rx="0.6" class="box-alt"/>'
        f'<text x="{cx_c:.2f}" y="{oy+5.6:.2f}" class="f-cond warn-c" font-size="1.95" font-weight="700" text-anchor="middle">{esc(tag)}</text>'
        f'<text x="{cx_c:.2f}" y="{oy+10.3:.2f}" class="f-cond ink" font-size="1.90" font-weight="700" text-anchor="middle">{esc(sub1)}</text>'
        f'<text x="{cx_c:.2f}" y="{oy+15.0:.2f}" class="f-cond {sub2_cls}" font-size="1.78" font-weight="700" text-anchor="middle">{esc(sub2)}</text>'
        f'<text x="{cx_c:.2f}" y="{oy+19.6:.2f}" class="f-cond {sub3_cls}" font-size="1.65" font-weight="700" text-anchor="middle">{esc(sub3)}</text>'
        f'<line x1="{u_t1:.2f}" y1="{oy+0.7:.2f}" x2="{u_t1:.2f}" y2="{oy+h-0.7:.2f}" class="seam-tick"/>'
    )


def render_rear_band(ox, oy, bleed=1.50, include_print=True, include_cut=True, uid="rear", sheet_slot="s2", ref_rect=None):
    """Piece 2: 222.425 x 23.80 mm Rear I/O + 1-Corner Pinwheel (CNR-BR + Flat Rear) Band."""
    w, h = FB_BAND_W, SIDE_H
    outer_cut = rrect(ox, oy, w, h, SIDE_R)
    bleed_path = rrect(ox - bleed, oy - bleed, w + 2 * bleed, h + 2 * bleed, SIDE_R + bleed)
    io_x = ox + IO_WELL_CX - IO_WELL_W / 2
    io_y = oy + IO_WELL_CY - IO_WELL_H / 2
    io_cut = rrect(io_x, io_y, IO_WELL_W, IO_WELL_H, IO_WELL_R)
    o = []

    if include_print:
        clip_id = f"{uid}-clip"
        o.append(f'<defs><clipPath id="{clip_id}"><path d="{bleed_path}"/></clipPath></defs>')
        o.append(f'<g id="{uid}-print" class="f-cond">')
        o.append(cad_grid_group(clip_id, ox - bleed, oy - bleed, w + 2 * bleed, h + 2 * bleed,
                                sheet_slot=sheet_slot, piece_kind="side", ref_rect=ref_rect))
        o.append(f'<g id="{uid}-fg" class="piece-fg-layer">')
        o.append(f'<rect x="{ox+0.7:.2f}" y="{oy+0.7:.2f}" width="{w-1.4:.2f}" height="{h-1.4:.2f}" rx="0.6" class="sc-rule" stroke-width="0.16"/>')

        # Back-Right 90-deg corner wrap at u = 0..16.745 (right end u=222.425 butts cleanly at Back-Left tangent!)
        u_t1 = ox + CORNER_ARC
        o.append(render_corner_tab(ox, oy, h, "CNR-BR", "BTN SIDE", "SEAM ──►"))

        # Block A (u = 18.0 .. 94.5): Hardware Architecture Block Schematic
        bx1, by1, bw1, bh1 = u_t1 + 1.2, oy + 1.4, 76.5, h - 2.8
        o.append(f'<rect x="{bx1:.2f}" y="{by1:.2f}" width="{bw1:.2f}" height="{bh1:.2f}" rx="0.7" class="box-bg"/>')
        o.append(corner_brackets(bx1, by1, bx1 + bw1, by1 + bh1, arm=1.1))
        o.append(f'<text x="{bx1+1.5:.2f}" y="{by1+2.75:.2f}" class="step-c" font-size="2.00" font-weight="700">'
                 f'REAR-01 // HARDWARE SCHEMATIC [JIELI AC636N · BR25]</text>')
        o.append(f'<line x1="{bx1:.2f}" y1="{by1+3.60:.2f}" x2="{bx1+bw1:.2f}" y2="{by1+3.60:.2f}" class="sc-rule" stroke-width="0.12"/>')

        # Central MCU node + peripheral schematic blocks
        mcu_x, mcu_y, mcu_w, mcu_h = bx1 + 26.3, by1 + 4.7, 23.8, 15.0
        o.append(f'<rect x="{mcu_x:.2f}" y="{mcu_y:.2f}" width="{mcu_w:.2f}" height="{mcu_h:.2f}" rx="0.6" class="box-alt"/>')
        o.append(f'<text x="{mcu_x+mcu_w/2:.2f}" y="{mcu_y+3.25:.2f}" class="f-cond pat-c" font-size="1.88" font-weight="700" text-anchor="middle">AC636N · BR25</text>')
        o.append(f'<text x="{mcu_x+mcu_w/2:.2f}" y="{mcu_y+6.75:.2f}" class="f-cond ink" font-size="1.68" font-weight="700" text-anchor="middle">pi32v2 · 960 PPQN</text>')
        o.append(f'<text x="{mcu_x+mcu_w/2:.2f}" y="{mcu_y+10.25:.2f}" class="f-cond warn-c" font-size="1.65" font-weight="700" text-anchor="middle">PI32V2 · 512K NOR</text>')
        o.append(f'<text x="{mcu_x+mcu_w/2:.2f}" y="{mcu_y+13.65:.2f}" class="f-cond ok-c" font-size="1.68" font-weight="700" text-anchor="middle">RAM ISR SAFE</text>')

        # Left input blocks -> MCU
        for idx_b, (lbl_a, lbl_b) in enumerate([
            ("16× VEL PADS",   "MUX PB2/3 → PA4/6"),
            ("8× ENC + 10× BTN", "SPI2 595 → PB0/PC1"),
            ("3.7V LI-ION BAT", "704060 · 2000mAh"),
        ]):
            ly_b = by1 + 4.45 + idx_b * 5.15
            o.append(f'<rect x="{bx1+1.2:.2f}" y="{ly_b:.2f}" width="23.2" height="4.6" rx="0.45" class="box-alt"/>')
            o.append(f'<text x="{bx1+2.0:.2f}" y="{ly_b+1.98:.2f}" class="f-cond ink" font-size="1.68" font-weight="700">{esc(lbl_a)}</text>')
            o.append(f'<text x="{bx1+2.0:.2f}" y="{ly_b+3.95:.2f}" class="f-cond muted" font-size="1.48" font-weight="700">{esc(lbl_b)}</text>')
            o.append(f'<line x1="{bx1+24.4:.2f}" y1="{ly_b+2.30:.2f}" x2="{mcu_x:.2f}" y2="{ly_b+2.30:.2f}" class="sc-rule-hi" stroke-width="0.18"/>')

        # MCU -> Right output blocks
        for idx_b, (lbl_a, lbl_b) in enumerate([
            ("16× WS2812 RGB",  "SPI1 PB1 · 800kHz"),
            ("TRS MIDI OUT",    "UART1 PC5 · 31.25k"),
            ("USB-C + BLE MIDI", "3-CBL · AE40 OTA"),
        ]):
            ry_b = by1 + 4.45 + idx_b * 5.15
            rx_b = mcu_x + mcu_w + 1.9
            o.append(f'<line x1="{mcu_x+mcu_w:.2f}" y1="{ry_b+2.30:.2f}" x2="{rx_b:.2f}" y2="{ry_b+2.30:.2f}" class="sc-rule-hi" stroke-width="0.18"/>')
            o.append(f'<rect x="{rx_b:.2f}" y="{ry_b:.2f}" width="23.3" height="4.6" rx="0.45" class="box-alt"/>')
            o.append(f'<text x="{rx_b+0.8:.2f}" y="{ry_b+1.98:.2f}" class="f-cond ink" font-size="1.68" font-weight="700">{esc(lbl_a)}</text>')
            o.append(f'<text x="{rx_b+0.8:.2f}" y="{ry_b+3.95:.2f}" class="f-cond pat-c" font-size="1.48" font-weight="700">{esc(lbl_b)}</text>')

        # Block B (u = 95.8 .. 169.6): Flash Memory Map, USB Cables & Safe-Mode Escape Hatch
        bx2, by2, bw2, bh2 = bx1 + bw1 + 1.4, oy + 1.4, (io_x - 1.5) - (bx1 + bw1 + 1.4), h - 2.8
        o.append(f'<rect x="{bx2:.2f}" y="{by2:.2f}" width="{bw2:.2f}" height="{bh2:.2f}" rx="0.7" class="box-bg"/>')
        o.append(corner_brackets(bx2, by2, bx2 + bw2, by2 + bh2, arm=1.1))
        o.append(f'<text x="{bx2+1.5:.2f}" y="{by2+2.75:.2f}" class="pat-c" font-size="2.00" font-weight="700">'
                 f'REAR-02 // FLASH MAP, SYSEX &amp; SAFE-MODE RECOVERY</text>')
        o.append(f'<line x1="{bx2:.2f}" y1="{by2+3.60:.2f}" x2="{bx2+bw2:.2f}" y2="{by2+3.60:.2f}" class="sc-rule" stroke-width="0.12"/>')

        flash_lines = [
            ("FLASH 512K:", "0x00000 CODE · 0x30000 16×PAT · 0x50000 VM · 0x77000 PST"),
            ("USB 3-CBL:",  "CBL0: Master MIDI · CBL1: Private Sinco · CBL2: Mackie"),
            ("DIAG SYSEX:", "F0 7D 53 50 01/02/05 F7 (status / counters / crash dump)"),
            ("SAFE MODE:",  "Power-cycle 2× in 5s → skips pads/keys, keeps BLE + OTA!"),
            ("UBOOT ROM:",  "USB_KEY handshake on DP/DM enters BR25 Mask-ROM loader"),
        ]
        for fi, (fk, fv) in enumerate(flash_lines):
            fy_t = by2 + 6.15 + fi * 3.25
            k_cls = "warn-c" if "SAFE" in fk else ("step-c" if "FLASH" in fk else "pat-c")
            o.append(f'<text x="{bx2+1.3:.2f}" y="{fy_t:.2f}" class="f-cond {k_cls}" font-size="1.78" font-weight="700">{esc(fk)}</text>')
            o.append(f'<text x="{bx2+15.6:.2f}" y="{fy_t:.2f}" class="f-cond ink" font-size="1.80" font-weight="700">{esc(fv)}</text>')

        # Port Stencil Callouts aligned directly over the 4 physical ports inside IO_WELL!
        port_stencils = [
            (ox + 173.68 + 6.085, "PWR", "ink"),
            (ox + 184.14 + 6.085, "CHG", "warn-c"),
            (ox + 192.42 + 6.085, "USB-C", "pat-c"),
            (ox + 206.90 + 6.085, "MIDI OUT", "step-c"),
        ]
        for px_s, plbl, pcls in port_stencils:
            o.append(f'<text x="{px_s:.2f}" y="{oy+3.18:.2f}" class="f-cond {pcls}" font-size="1.85" font-weight="700" text-anchor="middle">{plbl}</text>')
            o.append(f'<line x1="{px_s:.2f}" y1="{oy+3.42:.2f}" x2="{px_s:.2f}" y2="{io_y-0.22:.2f}" class="sc-rule-hi" stroke-width="0.20"/>')

        o.append(f'<text x="{ox+195.0:.2f}" y="{oy+21.95:.2f}" class="f-cond muted" font-size="1.65" font-weight="700" text-anchor="middle">'
                 f'5V ⎓ 500mA · RED=CHG / GRN=FULL · TRS-A MIDI OUT</text>')
        o.append("</g>")  # end piece-fg-layer
        o.append(f'<g id="{uid}-hole-mask" class="cutout-preview-mask no-print"><path d="{io_cut}"/></g>')
        o.append("</g>")

    if include_cut:
        o.append(f'<g id="{uid}-cut">')
        o.append(f'<path id="{uid}-cut-outer" d="{outer_cut}" class="cut-line"/>')
        o.append(f'<path id="{uid}-cut-io-well" d="{io_cut}" class="cut-line"/>')
        o.append("</g>")

    return "\n".join(o)


def render_front_band(ox, oy, bleed=1.50, include_print=True, include_cut=True, uid="front", sheet_slot="s2", ref_rect=None):
    """Piece 4: 222.425 x 23.80 mm Front Operator Cheat-Sheet Band (CNR-FL + Flat Front)."""
    w, h = FB_BAND_W, SIDE_H
    outer_cut = rrect(ox, oy, w, h, SIDE_R)
    bleed_path = rrect(ox - bleed, oy - bleed, w + 2 * bleed, h + 2 * bleed, SIDE_R + bleed)
    o = []

    if include_print:
        clip_id = f"{uid}-clip"
        o.append(f'<defs><clipPath id="{clip_id}"><path d="{bleed_path}"/></clipPath></defs>')
        o.append(f'<g id="{uid}-print" class="f-cond">')
        o.append(cad_grid_group(clip_id, ox - bleed, oy - bleed, w + 2 * bleed, h + 2 * bleed,
                                sheet_slot=sheet_slot, piece_kind="side", ref_rect=ref_rect))
        o.append(f'<g id="{uid}-fg" class="piece-fg-layer">')
        o.append(f'<rect x="{ox+0.7:.2f}" y="{oy+0.7:.2f}" width="{w-1.4:.2f}" height="{h-1.4:.2f}" rx="0.6" class="sc-rule" stroke-width="0.16"/>')

        u_t1 = ox + CORNER_ARC
        o.append(render_corner_tab(ox, oy, h, "CNR-FL", "KNOB SIDE", "⇧+RPT=SEQ", sub3="FW ≥ 925", sub2_cls="step-c", sub3_cls="ok-c"))

        # 4 Operator Quick-Reference Modules across the 205.68 mm flat front face
        modules = [
            ("F1 // STEP & NOTE LENGTH", "step-c", 50.6, [
                ("TAP PAD:",    "Step On / Off (S01..S16)"),
                ("HOLD+PAD:",   "Note Length 1–32 steps!"),
                ("HOLD+KNOB:",  "Step P-Lock (K3 moves chord)"),
                ("HOLD+RPT:",   "Ratchet ×1 → ×2 → ×3 → ×4"),
                ("PG < / >:",   "Page 1–4  ·  ⇧+< / > Undo/Redo"),
            ]),
            ("F2 // TRACKS [HOLD BANK]", "pat-c", 51.2, [
                ("TAP BANK:",   "Step View ↔ Play View"),
                ("HOLD+PAD:",   "Select active Track 1–16"),
                ("SRC+DST:",    "Hold Src+Tap Dst = Copy Notes"),
                ("+⇧ COPY:",    "Full Copy (+Ch / Note / Vol)"),
                ("+STOP+PAD:",  "Mute Trk · +⇧+STOP = Clear"),
            ]),
            ("F3 // LIVE REC & TIMING", "warn-c", 50.8, [
                ("REC+PLAY:",   "Live Rec (hold = gate len)"),
                ("CHORDS:",     "Hit active step (up to 4 notes)"),
                ("QUANTIZE:",   "K4 (0–100%) non-destructive!"),
                ("HOLD RPT:",   "P1-8 Rate · P9-13 Swg · P14 Fill"),
                ("SYNC/TAP:",   "P15 Ext Clock · P16 Tap Tempo"),
            ]),
            ("F4 // 16 PATTERNS & CUE", "ok-c", 48.0, [
                ("⇧+REC+PAD:",  "Save Slot 1–16 (no pad=cur)"),
                ("⇧+PLAY+PAD:", "Load Slot (cues next bar!)"),
                ("SLOT LEDS:",  "Off=Empty · Dim=Used · Lit=Cur"),
                ("FEEDBACK:",   "Blinks 3× GRN=OK / RED=ERR"),
                ("AUTO-SAVE:",  "STOP saves edits to flash"),
            ]),
        ]
        cur_x = u_t1 + 1.15
        for title, tcls, mw, rows in modules:
            my, mh = oy + 1.4, h - 2.8
            o.append(f'<rect x="{cur_x:.2f}" y="{my:.2f}" width="{mw:.2f}" height="{mh:.2f}" rx="0.65" class="box-bg"/>')
            o.append(corner_brackets(cur_x, my, cur_x + mw, my + mh, arm=1.0))
            o.append(f'<text x="{cur_x+1.2:.2f}" y="{my+2.72:.2f}" class="{tcls}" font-size="1.95" font-weight="700">{esc(title)}</text>')
            o.append(f'<line x1="{cur_x:.2f}" y1="{my+3.55:.2f}" x2="{cur_x+mw:.2f}" y2="{my+3.55:.2f}" class="sc-rule" stroke-width="0.12"/>')
            for ri, (rk, rv) in enumerate(rows):
                ry_t = my + 6.12 + ri * 3.25
                o.append(f'<text x="{cur_x+1.15:.2f}" y="{ry_t:.2f}" class="f-cond step-c" font-size="1.76" font-weight="700">{esc(rk)}</text>')
                o.append(f'<text x="{cur_x+13.9:.2f}" y="{ry_t:.2f}" class="f-cond ink" font-size="1.80" font-weight="700">{esc(rv)}</text>')
            cur_x += mw + 0.80

        o.append("</g>")  # end piece-fg-layer
        o.append("</g>")

    if include_cut:
        o.append(f'<g id="{uid}-cut"><path id="{uid}-cut-outer" d="{outer_cut}" class="cut-line"/></g>')

    return "\n".join(o)


def render_left_band(ox, oy, bleed=1.50, include_print=True, include_cut=True, uid="left", sheet_slot="s2", ref_rect=None):
    """Piece 5: 142.425 x 23.80 mm Left Side Band (CNR-BL + Flat Left Encoder Matrix & Tables)."""
    w, h = LR_BAND_W, SIDE_H
    outer_cut = rrect(ox, oy, w, h, SIDE_R)
    bleed_path = rrect(ox - bleed, oy - bleed, w + 2 * bleed, h + 2 * bleed, SIDE_R + bleed)
    o = []

    if include_print:
        clip_id = f"{uid}-clip"
        o.append(f'<defs><clipPath id="{clip_id}"><path d="{bleed_path}"/></clipPath></defs>')
        o.append(f'<g id="{uid}-print" class="f-cond">')
        o.append(cad_grid_group(clip_id, ox - bleed, oy - bleed, w + 2 * bleed, h + 2 * bleed,
                                sheet_slot=sheet_slot, piece_kind="side", ref_rect=ref_rect))
        o.append(f'<g id="{uid}-fg" class="piece-fg-layer">')
        o.append(f'<rect x="{ox+0.7:.2f}" y="{oy+0.7:.2f}" width="{w-1.4:.2f}" height="{h-1.4:.2f}" rx="0.6" class="sc-rule" stroke-width="0.16"/>')

        u_t1 = ox + CORNER_ARC
        o.append(render_corner_tab(ox, oy, h, "CNR-BL", "REAR SEAM", "SEAM ──►"))

        # Sub-box 1: Compact 8-Knob x 3-Mode Matrix (w = 59.0 mm)
        bx1, by1, bw1, bh1 = u_t1 + 1.15, oy + 1.4, 59.0, h - 2.8
        o.append(f'<rect x="{bx1:.2f}" y="{by1:.2f}" width="{bw1:.2f}" height="{bh1:.2f}" rx="0.65" class="box-bg"/>')
        o.append(f'<text x="{bx1+1.2:.2f}" y="{by1+2.65:.2f}" class="step-c" font-size="1.92" font-weight="700">'
                 f'LEFT-01 // K1–K8 KNOB MATRIX</text>')
        o.append(f'<line x1="{bx1:.2f}" y1="{by1+3.45:.2f}" x2="{bx1+bw1:.2f}" y2="{by1+3.45:.2f}" class="sc-rule" stroke-width="0.12"/>')
        k_compact = [
            ("K1", "Vol 0–127",  "Velocity",   "BPM 40–200"),
            ("K2", "Gate 1/8–4", "Len 1/8–32", "Swing 50–75"),
            ("K3", "Base Note",  "Note/Chord", "Trk Root"),
            ("K4", "Quantize",   "Micro ±48",  "Trk Scale"),
            ("K5", "Prob %",     "Step Prob",  "—"),
            ("K6", "Ratch Rate", "Stp Ratch",  "Trk Colour"),
            ("K7", "Steps 1–64", "Trig Cond",  "MIDI Ch"),
            ("K8", "Grid Rate",  "Step Tie",   "Sync ±120ms"),
        ]
        for idx_k, (kn, kt, ks, kp) in enumerate(k_compact):
            ky_r = by1 + 5.38 + idx_k * 2.08
            o.append(f'<text x="{bx1+1.1:.2f}" y="{ky_r:.2f}" class="f-cond warn-c" font-size="1.66" font-weight="700">{kn}</text>')
            o.append(f'<text x="{bx1+5.8:.2f}" y="{ky_r:.2f}" class="f-cond ink" font-size="1.68" font-weight="700">{kt}</text>')
            o.append(f'<text x="{bx1+23.4:.2f}" y="{ky_r:.2f}" class="f-cond step-c" font-size="1.68" font-weight="700">{ks}</text>')
            o.append(f'<text x="{bx1+41.0:.2f}" y="{ky_r:.2f}" class="f-cond pat-c" font-size="1.68" font-weight="700">{kp}</text>')

        # Sub-box 2: Trig Conditions, Gate Steps & Ratchet Table
        bx2 = bx1 + bw1 + 1.15
        bw2 = (ox + w - 1.40) - bx2
        o.append(f'<rect x="{bx2:.2f}" y="{by1:.2f}" width="{bw2:.2f}" height="{bh1:.2f}" rx="0.65" class="box-bg"/>')
        o.append(f'<text x="{bx2+1.2:.2f}" y="{by1+2.65:.2f}" class="pat-c" font-size="1.92" font-weight="700">'
                 f'LEFT-02 // TRIG, GATE &amp; RATCHET</text>')
        o.append(f'<line x1="{bx2:.2f}" y1="{by1+3.45:.2f}" x2="{bx2+bw2:.2f}" y2="{by1+3.45:.2f}" class="sc-rule" stroke-width="0.12"/>')
        spec_rows = [
            ("TRIG 1–6:",   "P1:ALW · P2-3: 1:2, 2:2 · P4-6: 1..3:3"),
            ("TRIG 7–13:",  "P7-10: 1..4:4 · P11:1ST · P12:FILL · 13:!F"),
            ("GATE [K2]:",  "1/8..7/8, 1..4S  ·  Step Len: 1..32S"),
            ("RATCH [K6]:", "AUTO(split) · 1/8 · 1/16 · 16T · 32 · 64"),
            ("GRID [K8]:",  "Keeps timing (dim=off-grid) · ⇧+K8 Speed"),
        ]
        for si, (sk, sv) in enumerate(spec_rows):
            sy_r = by1 + 6.12 + si * 3.25
            o.append(f'<text x="{bx2+1.1:.2f}" y="{sy_r:.2f}" class="f-cond step-c" font-size="1.76" font-weight="700">{esc(sk)}</text>')
            o.append(f'<text x="{bx2+15.0:.2f}" y="{sy_r:.2f}" class="f-cond ink" font-size="1.80" font-weight="700">{esc(sv)}</text>')

        o.append("</g>")  # end piece-fg-layer
        o.append("</g>")

    if include_cut:
        o.append(f'<g id="{uid}-cut"><path id="{uid}-cut-outer" d="{outer_cut}" class="cut-line"/></g>')

    return "\n".join(o)


def render_right_band(ox, oy, bleed=1.50, include_print=True, include_cut=True, uid="right", sheet_slot="s2", ref_rect=None):
    """Piece 6: 142.425 x 23.80 mm Right Side Band (CNR-FR + Flat Right Scales, LEDs & Clock)."""
    w, h = LR_BAND_W, SIDE_H
    outer_cut = rrect(ox, oy, w, h, SIDE_R)
    bleed_path = rrect(ox - bleed, oy - bleed, w + 2 * bleed, h + 2 * bleed, SIDE_R + bleed)
    o = []

    if include_print:
        clip_id = f"{uid}-clip"
        o.append(f'<defs><clipPath id="{clip_id}"><path d="{bleed_path}"/></clipPath></defs>')
        o.append(f'<g id="{uid}-print" class="f-cond">')
        o.append(cad_grid_group(clip_id, ox - bleed, oy - bleed, w + 2 * bleed, h + 2 * bleed,
                                sheet_slot=sheet_slot, piece_kind="side", ref_rect=ref_rect))
        o.append(f'<g id="{uid}-fg" class="piece-fg-layer">')
        o.append(f'<rect x="{ox+0.7:.2f}" y="{oy+0.7:.2f}" width="{w-1.4:.2f}" height="{h-1.4:.2f}" rx="0.6" class="sc-rule" stroke-width="0.16"/>')

        u_t1 = ox + CORNER_ARC
        o.append(render_corner_tab(ox, oy, h, "CNR-FR", "FRONT SEAM", "STOP×2=0", sub2_cls="step-c"))

        # Sub-box 1: 12 Roots & 12 Scales Lookup (w = 59.5 mm)
        bx1, by1, bw1, bh1 = u_t1 + 1.15, oy + 1.4, 59.5, h - 2.8
        o.append(f'<rect x="{bx1:.2f}" y="{by1:.2f}" width="{bw1:.2f}" height="{bh1:.2f}" rx="0.65" class="box-bg"/>')
        o.append(f'<text x="{bx1+1.2:.2f}" y="{by1+2.65:.2f}" class="pat-c" font-size="1.92" font-weight="700">'
                 f'RIGHT-01 // ROOTS &amp; SCALES [P1–12]</text>')
        o.append(f'<line x1="{bx1:.2f}" y1="{by1+3.45:.2f}" x2="{bx1+bw1:.2f}" y2="{by1+3.45:.2f}" class="sc-rule" stroke-width="0.12"/>')
        scl_rows = [
            ("P01: C · Chromatic",  "P07: F# · Mixolydian"),
            ("P02: C# · Major",     "P08: G · Locrian"),
            ("P03: D · Nat Minor",  "P09: G# · Harm Minor"),
            ("P04: D# · Dorian",    "P10: A · Pent Major"),
            ("P05: E · Phrygian",   "P11: A# · Pent Minor"),
            ("P06: F · Lydian",     "P12: B · Blues"),
        ]
        for si, (c_l, c_r) in enumerate(scl_rows):
            sy_r = by1 + 5.75 + si * 2.38
            o.append(f'<text x="{bx1+1.2:.2f}" y="{sy_r:.2f}" class="f-cond ink" font-size="1.75" font-weight="700">{c_l}</text>')
            o.append(f'<text x="{bx1+30.2:.2f}" y="{sy_r:.2f}" class="f-cond ink" font-size="1.75" font-weight="700">{c_r}</text>')
        o.append(f'<text x="{bx1+bw1/2:.2f}" y="{by1+19.98:.2f}" class="f-cond warn-c" font-size="1.58" font-weight="700" text-anchor="middle">'
                 f'0 LIT = FOLLOW  ·  BANK K3/4 = TRK  ·  ⇧+K3/4 = GLOBAL</text>')

        # Sub-box 2: Pad LED Overlay Decoder & MIDI Sync
        bx2 = bx1 + bw1 + 1.15
        bw2 = (ox + w - 1.40) - bx2
        o.append(f'<rect x="{bx2:.2f}" y="{by1:.2f}" width="{bw2:.2f}" height="{bh1:.2f}" rx="0.65" class="box-bg"/>')
        o.append(f'<text x="{bx2+1.2:.2f}" y="{by1+2.65:.2f}" class="step-c" font-size="1.92" font-weight="700">'
                 f'RIGHT-02 // LED READOUTS &amp; CLOCK</text>')
        o.append(f'<line x1="{bx2:.2f}" y1="{by1+3.45:.2f}" x2="{bx2+bw2:.2f}" y2="{by1+3.45:.2f}" class="sc-rule" stroke-width="0.12"/>')
        led_rows = [
            ("BAR LEDS:", "Vol / Vel / Prob / Qnt / BPM: P1→16"),
            ("SIGNED:",   "Micro & Sync: P8..1 (−) · P9..16 (+)"),
            ("NOTE KEY:", "P1–12 = C..B Root  ·  P13–16 = Octave"),
            ("EXT SYNC:", "P15 On = 24 PPQN  ·  STOP×2 = SPP 0"),
            ("SYNC MS:",  "BANK K8: −120..+120ms (Master/Slave)"),
        ]
        for li, (lk, lv) in enumerate(led_rows):
            ly_r = by1 + 6.12 + li * 3.25
            o.append(f'<text x="{bx2+1.1:.2f}" y="{ly_r:.2f}" class="f-cond pat-c" font-size="1.76" font-weight="700">{esc(lk)}</text>')
            o.append(f'<text x="{bx2+14.8:.2f}" y="{ly_r:.2f}" class="f-cond ink" font-size="1.80" font-weight="700">{esc(lv)}</text>')

        o.append("</g>")  # end piece-fg-layer
        o.append("</g>")

    if include_cut:
        o.append(f'<g id="{uid}-cut"><path id="{uid}-cut-outer" d="{outer_cut}" class="cut-line"/></g>')

    return "\n".join(o)


def render_io_insert(ox, oy, bleed=1.20, include_print=True, include_cut=True, uid="ioins", sheet_slot="s2", ref_rect=None):
    """Piece 3: 46.00 x 12.80 mm Drop-In Rear I/O Recessed Well Insert Sticker."""
    w, h = IO_INS_W, IO_INS_H
    outer_cut = rrect(ox, oy, w, h, IO_INS_R)
    bleed_path = rrect(ox - bleed, oy - bleed, w + 2 * bleed, h + 2 * bleed, IO_INS_R + bleed)
    px, py, pw, ph, pr = IO_PORTS["pwr"]
    lx, ly, lr = IO_PORTS["led"]
    ux, uy, uw, uh, ur = IO_PORTS["usb"]
    mx, my, mr = IO_PORTS["midi"]
    o = []

    if include_print:
        clip_id = f"{uid}-clip"
        o.append(f'<defs><clipPath id="{clip_id}"><path d="{bleed_path}"/></clipPath></defs>')
        o.append(f'<g id="{uid}-print" class="f-cond">')
        o.append(cad_grid_group(clip_id, ox - bleed, oy - bleed, w + 2 * bleed, h + 2 * bleed,
                                sheet_slot=sheet_slot, piece_kind="side", ref_rect=ref_rect))
        o.append(f'<g id="{uid}-fg" class="piece-fg-layer">')
        o.append(f'<rect x="{ox+0.45:.2f}" y="{oy+0.45:.2f}" width="{w-0.9:.2f}" height="{h-0.9:.2f}" rx="{IO_INS_R-0.4:.2f}" class="sc-rule" stroke-width="0.14"/>')
        o.append(f'<text x="{ox+px:.2f}" y="{oy+3.95:.2f}" class="f-cond ink" font-size="2.10" font-weight="700" text-anchor="middle">POWER</text>')
        o.append(f'<text x="{ox+lx:.2f}" y="{oy+3.45:.2f}" class="f-cond warn-c" font-size="1.85" font-weight="700" text-anchor="middle">CHG</text>')
        o.append(f'<text x="{ox+ux:.2f}" y="{oy+3.18:.2f}" class="f-cond pat-c" font-size="1.95" font-weight="700" text-anchor="middle">USB-C</text>')
        o.append(f'<text x="{ox+mx-0.55:.2f}" y="{oy+3.18:.2f}" class="f-cond step-c" font-size="1.95" font-weight="700" text-anchor="middle">MIDI</text>')
        o.append("</g>")  # end piece-fg-layer
        o.append(
            f'<g id="{uid}-hole-mask" class="cutout-preview-mask no-print">'
            f'<path d="{rrect(ox+px-pw/2, oy+py-ph/2, pw, ph, pr)}"/>'
            f'<circle cx="{ox+lx:.3f}" cy="{oy+ly:.3f}" r="{lr:.3f}"/>'
            f'<path d="{rrect(ox+ux-uw/2, oy+uy-uh/2, uw, uh, ur)}"/>'
            f'<circle cx="{ox+mx:.3f}" cy="{oy+my:.3f}" r="{mr:.3f}"/>'
            f'</g>'
        )
        o.append("</g>")

    if include_cut:
        o.append(f'<g id="{uid}-cut">')
        o.append(f'<path id="{uid}-cut-outer" d="{outer_cut}" class="cut-line"/>')
        o.append(f'<path id="{uid}-cut-pwr" d="{rrect(ox+px-pw/2, oy+py-ph/2, pw, ph, pr)}" class="cut-line"/>')
        o.append(f'<circle id="{uid}-cut-led" cx="{ox+lx:.3f}" cy="{oy+ly:.3f}" r="{lr:.3f}" class="cut-line"/>')
        o.append(f'<path id="{uid}-cut-usb" d="{rrect(ox+ux-uw/2, oy+uy-uh/2, uw, uh, ur)}" class="cut-line"/>')
        o.append(f'<circle id="{uid}-cut-midi" cx="{ox+mx:.3f}" cy="{oy+my:.3f}" r="{mr:.3f}" class="cut-line"/>')
        o.append("</g>")

    return "\n".join(o)


def render_vernier_ruler(ox, oy, uid="vr"):
    """100.00 mm 1:1 physical verification ruler + wrap guide."""
    o = [f'<g class="f-cond sheet-ruler" transform="translate({ox:.2f},{oy:.2f})">']
    o.append('<rect x="0" y="0" width="104" height="23.8" rx="0.8" class="box-bg"/>')
    o.append('<text x="2.0" y="3.35" class="step-c" font-size="1.92" font-weight="700">SCALE CHECK: 100.00 MM VERNIER</text>')
    o.append('<line x1="2" y1="5.0" x2="102" y2="5.0" class="sc-rule-hi" stroke-width="0.22"/>')
    ticks = []
    for mm in range(101):
        th = 3.2 if (mm % 10 == 0) else (2.1 if (mm % 5 == 0) else 1.2)
        ticks.append(f"M{2+mm},5.0V{5.0+th:.1f}")
        if mm % 10 == 0:
            o.append(f'<text x="{2+mm}" y="10.65" class="ink" font-size="1.65" font-weight="700" text-anchor="middle">{mm}</text>')
    o.append(f'<path d="{" ".join(ticks)}" class="sc-rule-hi" stroke-width="0.16"/>')

    o.append('<line x1="0" y1="11.8" x2="104" y2="11.8" class="sc-rule" stroke-width="0.14"/>')
    o.append('<line x1="52.0" y1="11.8" x2="52.0" y2="23.8" class="sc-rule" stroke-width="0.14"/>')
    o.append('<text x="2.0" y="15.0" class="pat-c" font-size="1.78" font-weight="700">360° PINWHEEL WRAP:</text>')
    o.append('<text x="2.0" y="18.6" class="ink" font-size="1.72" font-weight="700">1. Top Deck (flush inside rim)</text>')
    o.append('<text x="2.0" y="22.1" class="ink" font-size="1.72" font-weight="700">2. Rear → Left (butt at SEAM)</text>')
    o.append('<text x="53.8" y="15.0" class="ink" font-size="1.72" font-weight="700">3. Front → Right (butt at SEAM)</text>')
    o.append('<text x="53.8" y="18.6" class="ink" font-size="1.72" font-weight="700">4. I/O Insert (inside rear well)</text>')
    o.append('<text x="53.8" y="22.1" class="warn-c" font-size="1.66" font-weight="700">1:1 SCALE · US LETTER 8.5×11</text>')
    o.append("</g>")
    return "\n".join(o)


# ---- Cameo US Letter (8.5 x 11 in) + Inkscape `inkscape-silhouette` 3-Layer Builders ---
def build_cameo_regmarks_layer():
    """Exact `fablabnbg/inkscape-silhouette` Regmarks layer for Cameo US Letter (15/15/185.9/230.0mm)."""
    l, t, w, h = MIE_REG_LEFT, MIE_REG_TOP, MIE_REG_X, MIE_REG_Y
    r = l + w       # 200.9 mm
    b = t + h       # 245.0 mm
    safe_d = (
        f"M {l:.1f},{t+20.0:.1f} L {l+20.0:.1f},{t+20.0:.1f} L {l+20.0:.1f},{t:.1f} "
        f"L {r-20.0:.1f},{t:.1f} L {r-20.0:.1f},{t+20.0:.1f} L {r:.1f},{t+20.0:.1f} "
        f"L {r:.1f},{b:.1f} L {l+20.0:.1f},{b:.1f} L {l+20.0:.1f},{b-20.0:.1f} "
        f"L {l:.1f},{b-20.0:.1f} Z"
    )
    notes = (
        f"mark distance from document: Left={l:.1f}mm, Top={t:.1f}mm; "
        f"mark to mark distance: X={w:.1f}mm, Y={h:.1f}mm; "
    )
    return (
        f'<g inkscape:groupmode="layer" id="regmark" inkscape:label="Regmarks" sodipodi:insensitive="true">\n'
        f'  <path id="regmark-safe-area" d="{safe_d}" style="fill:#ffffff;stroke:none;"/>\n'
        f'  <rect id="regmark-tl" x="{l:.1f}" y="{t:.1f}" width="5.0" height="5.0" style="fill:#000000;stroke:none;"/>\n'
        f'  <path id="regmark-tr" d="M {r-20.0:.1f},{t:.1f} L {r:.1f},{t:.1f} L {r:.1f},{t+20.0:.1f}" '
        f'style="fill:none;stroke:#000000;stroke-width:0.3;"/>\n'
        f'  <path id="regmark-bl" d="M {l+20.0:.1f},{b:.1f} L {l:.1f},{b:.1f} L {l:.1f},{b-20.0:.1f}" '
        f'style="fill:none;stroke:#000000;stroke-width:0.3;"/>\n'
        f'  <text id="regmark-notes" x="{l+23.0:.1f}" y="{b+7.5:.1f}" '
        f'style="font-size:2.5px;fill:#000000;font-family:sans-serif;">{notes}</text>\n'
        f'</g>'
    )


def _inkscape_svg_open(fw, fh, standalone=True, dom_id="svg-mie"):
    ns = (
        'xmlns="http://www.w3.org/2000/svg" '
        'xmlns:xlink="http://www.w3.org/1999/xlink" '
        'xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
        'xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"'
    )
    if standalone:
        attrs = f'width="{fw:.2f}mm" height="{fh:.2f}mm" viewBox="0 0 {fw:.2f} {fh:.2f}" {ns}'
    else:
        attrs = f'id="{dom_id}" class="sheet-svg" data-w="{fw:.2f}" data-h="{fh:.2f}" data-inkscape="1" viewBox="0 0 {fw:.2f} {fh:.2f}" {ns}'
    return (
        f'<svg {attrs}>\n'
        f'<sodipodi:namedview inkscape:document-units="mm" pagecolor="#ffffff" bordercolor="#666666" borderopacity="1.0"/>'
    )


def build_cameo_letter_sheet1_svg(theme="light", include_cut=True, standalone=True):
    """Cameo US Letter (215.9 x 279.4 mm) Sheet 1: Rotated +90 deg Top Panel inside 185.9 x 230.0 mm Regmarks."""
    fw, fh = LETTER_W, LETTER_H
    # Rotated +90 deg: local (x, y) -> (tx - y, ty + x).
    # Cut rect [2.5..224.5] x [2.5..144.5] maps to X=[36.95..178.95], Y=[19.00..241.00] (100% inside [35..180.9] x [15..245]!)
    tx, ty = 181.450, 16.500
    o = [_inkscape_svg_open(fw, fh, standalone=standalone, dom_id="svg-mie-s1")]
    o.append(f'<style class="theme-style">{svg_theme_css(theme)}</style>')
    o.append(build_cameo_regmarks_layer())

    o.append('<g inkscape:groupmode="layer" id="layer-print" inkscape:label="Print">')
    o.append(f'<g transform="translate({tx:.3f},{ty:.3f}) rotate(90)">')
    o.append(render_top_panel(0.0, 0.0, bleed=1.35, include_print=True, include_cut=False, uid="mie1tp"))
    o.append('</g>')
    o.append(
        f'<text x="{fw/2:.2f}" y="261.0" class="f-cond" fill="#334155" font-size="2.35" font-weight="700" text-anchor="middle">'
        f'SMC-PAD // CAMEO SHEET 1 OF 2: TOP CONTROL DECK (US LETTER 8.5×11" · PRINT 100% · INKSCAPE X-OFFSET = 6 MM)</text>'
    )
    o.append('</g>')

    if include_cut:
        o.append('<g inkscape:groupmode="layer" id="layer-cut" inkscape:label="Cut">')
        o.append(f'<g transform="translate({tx:.3f},{ty:.3f}) rotate(90)">')
        o.append(render_top_panel(0.0, 0.0, bleed=1.35, include_print=False, include_cut=True, uid="mie1tp"))
        o.append('</g>')
        o.append('</g>')

    o.append('</svg>')
    return "\n".join(o)


def build_cameo_letter_sheet2_svg(theme="light", include_cut=True, standalone=True):
    """Cameo US Letter (215.9 x 279.4 mm) Sheet 2: 4 Pinwheel Side Bands + 2x I/O Inserts + 100mm Ruler."""
    fw, fh = LETTER_W, LETTER_H
    # 5 vertical columns inside X=[36.15..179.30], Y=[17.44..242.56] (100% inside [35..180.9] x [15..245]!)
    ty_band = 18.788
    ty_io = 176.000
    o = [_inkscape_svg_open(fw, fh, standalone=standalone, dom_id="svg-mie-s2")]
    o.append(f'<style class="theme-style">{svg_theme_css(theme)}</style>')
    o.append(build_cameo_regmarks_layer())

    o.append('<g inkscape:groupmode="layer" id="layer-print" inkscape:label="Print">')
    for tx, ty, svg_frag in (
        (61.300,  ty_band, render_rear_band(0.0, 0.0, bleed=1.35, include_print=True, include_cut=False, uid="mie2rear")),
        (90.800,  ty_band, render_front_band(0.0, 0.0, bleed=1.35, include_print=True, include_cut=False, uid="mie2front")),
        (120.300, ty_band, render_left_band(0.0, 0.0, bleed=1.35, include_print=True, include_cut=False, uid="mie2left")),
        (114.800, ty_io,   render_io_insert(0.0, 0.0, bleed=1.20, include_print=True, include_cut=False, uid="mie2io1")),
        (149.800, ty_band, render_right_band(0.0, 0.0, bleed=1.35, include_print=True, include_cut=False, uid="mie2right")),
        (144.300, ty_io,   render_io_insert(0.0, 0.0, bleed=1.20, include_print=True, include_cut=False, uid="mie2io2")),
        (179.300, 78.000,  render_vernier_ruler(0.0, 0.0, uid="mie2vr")),
    ):
        o.append(f'<g transform="translate({tx:.3f},{ty:.3f}) rotate(90)">\n{svg_frag}\n</g>')
    o.append(
        f'<text x="{fw/2:.2f}" y="261.0" class="f-cond" fill="#334155" font-size="2.35" font-weight="700" text-anchor="middle">'
        f'SMC-PAD // CAMEO SHEET 2 OF 2: 360° PINWHEEL SIDES + 2× I/O INSERTS (US LETTER · PRINT 100% · X-OFFSET = 6 MM)</text>'
    )
    o.append('</g>')

    if include_cut:
        o.append('<g inkscape:groupmode="layer" id="layer-cut" inkscape:label="Cut">')
        for tx, ty, svg_frag in (
            (61.300,  ty_band, render_rear_band(0.0, 0.0, bleed=1.35, include_print=False, include_cut=True, uid="mie2rear")),
            (90.800,  ty_band, render_front_band(0.0, 0.0, bleed=1.35, include_print=False, include_cut=True, uid="mie2front")),
            (120.300, ty_band, render_left_band(0.0, 0.0, bleed=1.35, include_print=False, include_cut=True, uid="mie2left")),
            (114.800, ty_io,   render_io_insert(0.0, 0.0, bleed=1.20, include_print=False, include_cut=True, uid="mie2io1")),
            (149.800, ty_band, render_right_band(0.0, 0.0, bleed=1.35, include_print=False, include_cut=True, uid="mie2right")),
            (144.300, ty_io,   render_io_insert(0.0, 0.0, bleed=1.20, include_print=False, include_cut=True, uid="mie2io2")),
        ):
            o.append(f'<g transform="translate({tx:.3f},{ty:.3f}) rotate(90)">\n{svg_frag}\n</g>')
        o.append('</g>')

    o.append('</svg>')
    return "\n".join(o)


def build_cameo_roll_to_letter_svg():
    """Optional Step-0 Roll-Feeder job in Inkscape: cuts one crisp 215.9 x 279.4 mm US Letter sheet from a roll."""
    w, h = 222.0, 290.0
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<svg width="{w:.2f}mm" height="{h:.2f}mm" viewBox="0 0 {w:.2f} {h:.2f}"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd">
  <sodipodi:namedview inkscape:document-units="mm" pagecolor="#ffffff"/>
  <style>.cut {{ fill: none; stroke: #ff0000; stroke-width: 0.15; }}</style>
  <g inkscape:groupmode="layer" id="layer-cut" inkscape:label="Cut">
    <rect id="us-letter-blank" x="3.050" y="5.000" width="215.900" height="279.400" class="cut"/>
  </g>
</svg>
"""


# ---- Sheet Builders (Cameo US Letter Studio, A4 Landscape & Single 12x12 SVG + DXF) ----
def build_cameo_studio_sheet1_svg(theme="light", cut_only=False, include_cut=True, standalone=True, include_align=True):
    """Cameo US Letter Landscape (279.4 x 215.9 mm) Silhouette Studio Sheet 1 (240.0 x 154.0 mm frame)."""
    fw, fh = FRAME_LTR_W, FRAME_LTR_H
    ox, oy = (fw - BODY_W) / 2.0, (fh - BODY_H) / 2.0   # 6.50, 3.50
    attrs = (f'width="{fw:.2f}mm" height="{fh:.2f}mm" viewBox="0 0 {fw:.2f} {fh:.2f}"'
             if standalone else
             f'id="svg-mie-studio1" class="sheet-svg" data-w="{fw:.2f}" data-h="{fh:.2f}" viewBox="0 0 {fw:.2f} {fh:.2f}"')
    o = [f'<svg {attrs} xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">']
    o.append(f'<style class="theme-style">{svg_theme_css(theme)}</style>')
    if not cut_only:
        # Solid white canvas + 0.16 mm edge hairline guarantees exact 240.0 x 154.0 mm PNG bounding box in Silhouette Studio
        o.append(f'<rect x="0.08" y="0.08" width="{fw-0.16:.2f}" height="{fh-0.16:.2f}" class="sheet-paper-bg"/>')
        o.append(
            f'<g class="sheet-annotations f-cond">'
            f'<text x="3.5" y="3.3" class="muted" font-size="1.82" font-weight="700">'
            f'SMC-PAD // SHEET 1 OF 2: MAIN CONTROL DECK (US LETTER 8.5×11" LANDSCAPE) — ALIGN FRAME: 240.00 × 154.00 MM</text>'
            f'<text x="236.5" y="3.3" class="muted" font-size="1.72" font-weight="700" text-anchor="end">1:1 MM · SILHOUETTE STUDIO</text>'
            f'</g>'
        )
    o.append(render_top_panel(ox, oy, bleed=1.50, include_print=not cut_only, include_cut=(include_cut or cut_only), uid="mls1tp", sheet_slot="s1"))
    if include_align:
        o.append(f'<g class="align-layer"><rect x="0" y="0" width="{fw:.3f}" height="{fh:.3f}" class="align-frame"/></g>')
    o.append("</svg>")
    return "\n".join(o)


def build_cameo_studio_sheet2_svg(theme="light", cut_only=False, include_cut=True, standalone=True, include_align=True):
    """Cameo US Letter Landscape (279.4 x 215.9 mm) Silhouette Studio Sheet 2 (240.0 x 154.0 mm frame)."""
    fw, fh = FRAME_LTR_W, FRAME_LTR_H
    bx_fb = (fw - FB_BAND_W) / 2.0   # 8.7875 mm
    bx_lr = 5.00
    s2_ref = (3.60, 4.60, 232.80, 113.60)
    attrs = (f'width="{fw:.2f}mm" height="{fh:.2f}mm" viewBox="0 0 {fw:.2f} {fh:.2f}"'
             if standalone else
             f'id="svg-mie-studio2" class="sheet-svg" data-w="{fw:.2f}" data-h="{fh:.2f}" viewBox="0 0 {fw:.2f} {fh:.2f}"')
    o = [f'<svg {attrs} xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">']
    o.append(f'<style class="theme-style">{svg_theme_css(theme)}</style>')

    if not cut_only:
        o.append(f'<rect x="0.08" y="0.08" width="{fw-0.16:.2f}" height="{fh-0.16:.2f}" class="sheet-paper-bg"/>')
        o.append(
            f'<g class="sheet-annotations f-cond">'
            f'<text x="5.0" y="3.8" class="muted" font-size="1.82" font-weight="700">'
            f'SMC-PAD // SHEET 2 OF 2: 360° PINWHEEL SIDES &amp; I/O INSERT (US LETTER 8.5×11") — ALIGN FRAME: 240.00 × 154.00 MM</text>'
            f'<text x="8.8" y="33.2" class="muted" font-size="1.52" font-weight="700">▲ REAR PANEL (222.43×23.80mm)   ▼ FRONT CHEAT SHEET (222.43×23.80mm)</text>'
            f'<text x="5.0" y="62.2" class="muted" font-size="1.52" font-weight="700">▼ LEFT ENCODER SIDE (142.43×23.80mm)</text>'
            f'<text x="171.0" y="66.8" class="muted" font-size="1.52" font-weight="700">▼ REAR I/O INSERTS (PRIMARY + SPARE)</text>'
            f'<text x="5.0" y="91.2" class="muted" font-size="1.52" font-weight="700">▼ RIGHT TRANSPORT SIDE (142.43×23.80mm)</text>'
            f'<text x="114.0" y="128.0" class="muted" font-size="1.75" font-weight="700">SILHOUETTE STUDIO (US LETTER LANDSCAPE):</text>'
            f'<text x="114.0" y="132.2" class="muted" font-size="1.62" font-weight="700">1. Page Setup: Letter 8.5×11" Landscape, Reg Marks ON (Type 1)</text>'
            f'<text x="114.0" y="136.0" class="muted" font-size="1.62" font-weight="700">2. Open 300 DPI PNG (240×154 mm) → Center to Page → Set PNG No Cut</text>'
            f'<text x="114.0" y="139.8" class="muted" font-size="1.62" font-weight="700">3. Open DXF → Cmd+A, Cmd+G (Group!) → Paste &amp; Center to Page</text>'
            f'<text x="114.0" y="143.6" class="muted" font-size="1.62" font-weight="700">4. Ungroup DXF &amp; set outer 240×154 mm frame to No Cut · Print 100%</text>'
            f'</g>'
        )

    show_c = (include_cut or cut_only)
    o.append(render_rear_band(bx_fb, 6.0, bleed=1.40, include_print=not cut_only, include_cut=show_c, uid="mls2rear", sheet_slot="s2", ref_rect=s2_ref))
    o.append(render_front_band(bx_fb, 35.0, bleed=1.40, include_print=not cut_only, include_cut=show_c, uid="mls2front", sheet_slot="s2", ref_rect=s2_ref))
    o.append(render_left_band(bx_lr, 64.0, bleed=1.40, include_print=not cut_only, include_cut=show_c, uid="mls2left", sheet_slot="s2", ref_rect=s2_ref))
    o.append(render_io_insert(171.0, 69.5, bleed=1.20, include_print=not cut_only, include_cut=show_c, uid="mls2io1", sheet_slot="s2", ref_rect=s2_ref))
    o.append(render_right_band(bx_lr, 93.0, bleed=1.40, include_print=not cut_only, include_cut=show_c, uid="mls2right", sheet_slot="s2", ref_rect=s2_ref))
    o.append(render_io_insert(171.0, 98.5, bleed=1.20, include_print=not cut_only, include_cut=show_c, uid="mls2io2", sheet_slot="s2", ref_rect=s2_ref))
    if not cut_only:
        o.append(render_vernier_ruler(bx_lr, 122.0, uid="mls2vr"))

    if include_align:
        o.append(f'<g class="align-layer"><rect x="0" y="0" width="{fw:.3f}" height="{fh:.3f}" class="align-frame"/></g>')
    o.append("</svg>")
    return "\n".join(o)


def build_sheet1_svg(theme="light", cut_only=False, standalone=True, include_align=True):
    """Sheet 1 (250 x 165 mm frame): Flush 222 x 142 mm Top Panel centered at (+11.5, +9.0)."""
    fw, fh = FRAME_A4_W, FRAME_A4_H
    ox, oy = (fw - BODY_W) / 2.0, (fh - BODY_H) / 2.0   # 11.50, 9.00
    attrs = (f'width="{fw:.2f}mm" height="{fh:.2f}mm" viewBox="0 0 {fw:.2f} {fh:.2f}"'
             if standalone else
             f'id="svg-sheet1" class="sheet-svg" data-w="{fw:.2f}" data-h="{fh:.2f}" viewBox="0 0 {fw:.2f} {fh:.2f}"')
    o = [f'<svg {attrs} xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">']
    o.append(f'<style class="theme-style">{svg_theme_css(theme)}</style>')
    if not cut_only:
        o.append(f'<rect x="0.08" y="0.08" width="{fw-0.16:.2f}" height="{fh-0.16:.2f}" class="sheet-paper-bg"/>')
        o.append(
            f'<g class="sheet-annotations f-cond">'
            f'<text x="3.0" y="4.4" class="muted" font-size="1.95" font-weight="700">'
            f'SMC-PAD // SHEET 1 OF 2: MAIN CONTROL DECK (222.0 × 142.0 MM) — ALIGN FRAME: 250.00 × 165.00 MM</text>'
            f'<text x="247.0" y="4.4" class="muted" font-size="1.80" font-weight="700" text-anchor="end">1:1 MM SCALE · FW≥925</text>'
            f'</g>'
        )
    o.append(render_top_panel(ox, oy, bleed=1.50, include_print=not cut_only, include_cut=True, uid="s1tp", sheet_slot="s1"))
    if include_align:
        o.append(f'<g class="align-layer"><rect x="0" y="0" width="{fw:.3f}" height="{fh:.3f}" class="align-frame"/></g>')
    o.append("</svg>")
    return "\n".join(o)


def build_sheet2_svg(theme="light", cut_only=False, standalone=True, include_align=True):
    """Sheet 2 (250 x 165 mm frame): 4 Pinwheel Side Bands + 2x Rear I/O Inserts + Ruler."""
    fw, fh = FRAME_A4_W, FRAME_A4_H
    bx_fb = (fw - FB_BAND_W) / 2.0   # 13.788 mm
    bx_lr = 4.50
    s2_ref = (3.00, 6.50, 244.00, 116.80)
    attrs = (f'width="{fw:.2f}mm" height="{fh:.2f}mm" viewBox="0 0 {fw:.2f} {fh:.2f}"'
             if standalone else
             f'id="svg-sheet2" class="sheet-svg" data-w="{fw:.2f}" data-h="{fh:.2f}" viewBox="0 0 {fw:.2f} {fh:.2f}"')
    o = [f'<svg {attrs} xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">']
    o.append(f'<style class="theme-style">{svg_theme_css(theme)}</style>')

    if not cut_only:
        o.append(f'<rect x="0.08" y="0.08" width="{fw-0.16:.2f}" height="{fh-0.16:.2f}" class="sheet-paper-bg"/>')
        o.append(
            f'<g class="sheet-annotations f-cond">'
            f'<text x="4.5" y="4.8" class="muted" font-size="1.92" font-weight="700">'
            f'SMC-PAD // SHEET 2 OF 2: 360° PINWHEEL SIDE BANDS &amp; I/O WELL INSERT — ALIGN FRAME: 250.00 × 165.00 MM</text>'
            f'<text x="13.8" y="35.6" class="muted" font-size="1.58" font-weight="700">▲ REAR PANEL (222.43×23.80mm)   ▼ FRONT CHEAT SHEET (222.43×23.80mm)</text>'
            f'<text x="4.5" y="65.8" class="muted" font-size="1.58" font-weight="700">▼ LEFT ENCODER SIDE (142.43×23.80mm)</text>'
            f'<text x="152.0" y="69.5" class="muted" font-size="1.58" font-weight="700">▼ REAR I/O WELL INSERTS (PRIMARY + SPARE, 46.00×12.80mm)</text>'
            f'<text x="4.5" y="95.8" class="muted" font-size="1.58" font-weight="700">▼ RIGHT TRANSPORT SIDE (142.43×23.80mm)</text>'
            f'<text x="4.5" y="160.8" class="muted" font-size="1.68" font-weight="700">'
            f'CAMEO 5: Cut RED (#ff0000) paths · Set BLUE (#0066ff) 250×165mm Align Frame to NO CUT after centering.</text>'
            f'</g>'
        )

    o.append(render_rear_band(bx_fb, 8.0, bleed=1.50, include_print=not cut_only, include_cut=True, uid="s2rear", sheet_slot="s2", ref_rect=s2_ref))
    o.append(render_front_band(bx_fb, 38.0, bleed=1.50, include_print=not cut_only, include_cut=True, uid="s2front", sheet_slot="s2", ref_rect=s2_ref))
    o.append(render_left_band(bx_lr, 68.0, bleed=1.50, include_print=not cut_only, include_cut=True, uid="s2left", sheet_slot="s2", ref_rect=s2_ref))
    o.append(render_io_insert(152.0, 73.5, bleed=1.20, include_print=not cut_only, include_cut=True, uid="s2io1", sheet_slot="s2", ref_rect=s2_ref))
    o.append(render_io_insert(201.5, 73.5, bleed=1.20, include_print=not cut_only, include_cut=True, uid="s2io2", sheet_slot="s2", ref_rect=s2_ref))
    o.append(render_right_band(bx_lr, 98.0, bleed=1.50, include_print=not cut_only, include_cut=True, uid="s2right", sheet_slot="s2", ref_rect=s2_ref))
    if not cut_only:
        o.append(render_vernier_ruler(bx_lr, 128.0, uid="s2vr"))

    if include_align:
        o.append(f'<g class="align-layer"><rect x="0" y="0" width="{fw:.3f}" height="{fh:.3f}" class="align-frame"/></g>')
    o.append("</svg>")
    return "\n".join(o)


def build_single_12x12_svg(theme="light", cut_only=False, standalone=True, include_align=True):
    """Single-sheet nested layout (256 x 284 mm frame) for 12x12 in (305x305) or A3 (297x420) roll cuts."""
    fw, fh = FRAME_12_W, FRAME_12_H
    ox_tp = (fw - BODY_W) / 2.0       # 14.50 mm
    bx_fb = (fw - FB_BAND_W) / 2.0    # 16.788 mm
    bx_lr = 4.00
    s2_ref = (2.50, 154.00, 251.00, 119.80)
    attrs = (f'width="{fw:.2f}mm" height="{fh:.2f}mm" viewBox="0 0 {fw:.2f} {fh:.2f}"'
             if standalone else
             f'id="svg-single" class="sheet-svg" data-w="{fw:.2f}" data-h="{fh:.2f}" viewBox="0 0 {fw:.2f} {fh:.2f}"')
    o = [f'<svg {attrs} xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">']
    o.append(f'<style class="theme-style">{svg_theme_css(theme)}</style>')
    if not cut_only:
        o.append(f'<rect x="0.08" y="0.08" width="{fw-0.16:.2f}" height="{fh-0.16:.2f}" class="sheet-paper-bg"/>')
        o.append(
            f'<g class="sheet-annotations f-mono">'
            f'<text x="4.0" y="4.2" class="muted" font-size="1.80" font-weight="700">'
            f'SMC-PAD // SINGLE-SHEET FULL KIT (12×12" OR A3 ROLL CUT) — ALIGN FRAME: 256.00 × 284.00 MM</text>'
            f'</g>'
        )
    o.append(render_top_panel(ox_tp, 5.0, bleed=1.50, include_print=not cut_only, include_cut=True, uid="sgtp", sheet_slot="s1"))
    o.append(render_rear_band(bx_fb, 155.5, bleed=1.50, include_print=not cut_only, include_cut=True, uid="sgrear", sheet_slot="s2", ref_rect=s2_ref))
    o.append(render_front_band(bx_fb, 186.5, bleed=1.50, include_print=not cut_only, include_cut=True, uid="sgfront", sheet_slot="s2", ref_rect=s2_ref))
    o.append(render_left_band(bx_lr, 217.5, bleed=1.50, include_print=not cut_only, include_cut=True, uid="sgleft", sheet_slot="s2", ref_rect=s2_ref))
    o.append(render_io_insert(152.5, 223.0, bleed=1.20, include_print=not cut_only, include_cut=True, uid="sgio1", sheet_slot="s2", ref_rect=s2_ref))
    o.append(render_io_insert(204.5, 223.0, bleed=1.20, include_print=not cut_only, include_cut=True, uid="sgio2", sheet_slot="s2", ref_rect=s2_ref))
    o.append(render_right_band(bx_lr, 248.5, bleed=1.50, include_print=not cut_only, include_cut=True, uid="sgright", sheet_slot="s2", ref_rect=s2_ref))
    if not cut_only:
        o.append(render_vernier_ruler(149.8, 248.5, uid="sgvr"))
    if include_align:
        o.append(f'<g class="align-layer"><rect x="0" y="0" width="{fw:.3f}" height="{fh:.3f}" class="align-frame"/></g>')
    o.append("</svg>")
    return "\n".join(o)


def build_roll_to_a4_svg():
    """Step-0 Cameo 5 Roll-Feeder job: cuts two crisp 297.0 x 210.0 mm A4 sheets from a 12-inch roll."""
    w, h = 300.0, 435.0
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<svg width="{w}mm" height="{h}mm" viewBox="0 0 {w} {h}" xmlns="http://www.w3.org/2000/svg">
  <style>.cut {{ fill: none; stroke: #ff0000; stroke-width: 0.15; }}</style>
  <rect id="a4-blank-1" x="1.500" y="5.000" width="297.000" height="210.000" class="cut"/>
  <rect id="a4-blank-2" x="1.500" y="220.000" width="297.000" height="210.000" class="cut"/>
</svg>
"""


# ---- AutoCAD R12 DXF Writers (1:1 mm, Silhouette Studio Basic Edition compatible) ---------
class DxfBuilder:
    def __init__(self, height_mm):
        self.h = height_mm
        self.lines = [
            "0", "SECTION", "2", "HEADER", "9", "$INSUNITS", "70", "4", "0", "ENDSEC",
            "0", "SECTION", "2", "ENTITIES",
        ]

    def fy(self, y):
        return self.h - y

    def circle(self, cx, cy, r, layer="CUT", color=1):
        self.lines.extend([
            "0", "CIRCLE", "8", layer, "62", str(color),
            "10", f"{cx:.4f}", "20", f"{self.fy(cy):.4f}", "30", "0.0",
            "40", f"{r:.4f}",
        ])

    def rrect(self, x0, y0, x1, y1, r_tl, r_tr, r_br, r_bl, layer="CUT", color=1, segs=12):
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
        self.lines.extend(["0", "POLYLINE", "8", layer, "62", str(color), "66", "1", "70", "1"])
        for px, py in pts:
            self.lines.extend(["0", "VERTEX", "8", layer, "10", f"{px:.4f}", "20", f"{self.fy(py):.4f}", "30", "0.0"])
        self.lines.extend(["0", "SEQEND"])

    def add_top_panel(self, ox, oy):
        x0, y0, x1, y1 = PANEL_RECT[0] + ox, PANEL_RECT[1] + oy, PANEL_RECT[2] + ox, PANEL_RECT[3] + oy
        self.rrect(x0, y0, x1, y1, CORNER_R, CORNER_R, CORNER_R, CORNER_R)
        for cx in KNOB_X:
            for cy in KNOB_Y:
                self.circle(ox + cx, oy + cy, KNOB_HOLE_R)
        for r_top in range(4):
            for c_idx in range(4):
                cx, cy = ox + PAD_X[c_idx], oy + PAD_Y[r_top]
                tl, tr, br, bl = pad_corners(c_idx, r_top)
                self.rrect(cx - PAD_W / 2, cy - PAD_H / 2, cx + PAD_W / 2, cy + PAD_H / 2, tl, tr, br, bl)
        for i in range(len(BTN_SPECS)):
            by = oy + BTN_Y0 + i * BTN_PITCH
            self.rrect(
                ox + BTN_X0 - BTN_CLEAR, by - BTN_CLEAR,
                ox + BTN_X1 + BTN_CLEAR, by + BTN_H + BTN_CLEAR,
                BTN_R, BTN_R, BTN_R, BTN_R,
            )

    def add_band(self, ox, oy, w, with_io=False):
        self.rrect(ox, oy, ox + w, oy + SIDE_H, SIDE_R, SIDE_R, SIDE_R, SIDE_R)
        if with_io:
            ix0 = ox + IO_WELL_CX - IO_WELL_W / 2
            iy0 = oy + IO_WELL_CY - IO_WELL_H / 2
            self.rrect(ix0, iy0, ix0 + IO_WELL_W, iy0 + IO_WELL_H, IO_WELL_R, IO_WELL_R, IO_WELL_R, IO_WELL_R)

    def add_io_insert(self, ox, oy):
        self.rrect(ox, oy, ox + IO_INS_W, oy + IO_INS_H, IO_INS_R, IO_INS_R, IO_INS_R, IO_INS_R)
        px, py, pw, ph, pr = IO_PORTS["pwr"]
        lx, ly, lr = IO_PORTS["led"]
        ux, uy, uw, uh, ur = IO_PORTS["usb"]
        mx, my, mr = IO_PORTS["midi"]
        self.rrect(ox + px - pw / 2, oy + py - ph / 2, ox + px + pw / 2, oy + py + ph / 2, pr, pr, pr, pr)
        self.circle(ox + lx, oy + ly, lr)
        self.rrect(ox + ux - uw / 2, oy + uy - uh / 2, ox + ux + uw / 2, oy + uy + uh / 2, ur, ur, ur, ur)
        self.circle(ox + mx, oy + my, mr)

    def add_align_frame(self, fw, fh):
        self.rrect(0.0, 0.0, fw, fh, 0.0, 0.0, 0.0, 0.0, layer="ALIGN_FRAME", color=5, segs=1)

    def finish(self):
        return "\n".join(self.lines + ["0", "ENDSEC", "0", "EOF", ""])


def build_cameo_letter_sheet1_dxf():
    d = DxfBuilder(FRAME_LTR_H)
    d.add_top_panel((FRAME_LTR_W - BODY_W) / 2.0, (FRAME_LTR_H - BODY_H) / 2.0)
    d.add_align_frame(FRAME_LTR_W, FRAME_LTR_H)
    return d.finish()


def build_cameo_letter_sheet2_dxf():
    d = DxfBuilder(FRAME_LTR_H)
    bx_fb = (FRAME_LTR_W - FB_BAND_W) / 2.0
    bx_lr = 5.00
    d.add_band(bx_fb, 6.0, FB_BAND_W, with_io=True)
    d.add_band(bx_fb, 35.0, FB_BAND_W, with_io=False)
    d.add_band(bx_lr, 64.0, LR_BAND_W, with_io=False)
    d.add_io_insert(171.0, 69.5)
    d.add_band(bx_lr, 93.0, LR_BAND_W, with_io=False)
    d.add_io_insert(171.0, 98.5)
    d.add_align_frame(FRAME_LTR_W, FRAME_LTR_H)
    return d.finish()


def build_cameo_roll_to_letter_dxf():
    """Optional Step-0 Roll-Feeder job in Silhouette Studio: cuts two 279.4 x 215.9 mm US Letter sheets from a 12" roll."""
    d = DxfBuilder(448.0)
    d.rrect(12.70, 5.00, 12.70 + 279.40, 5.00 + 215.90, 0.0, 0.0, 0.0, 0.0, segs=1)
    d.rrect(12.70, 226.00, 12.70 + 279.40, 226.00 + 215.90, 0.0, 0.0, 0.0, 0.0, segs=1)
    return d.finish()


def build_sheet1_dxf():
    d = DxfBuilder(FRAME_A4_H)
    d.add_top_panel((FRAME_A4_W - BODY_W) / 2.0, (FRAME_A4_H - BODY_H) / 2.0)
    d.add_align_frame(FRAME_A4_W, FRAME_A4_H)
    return d.finish()


def build_sheet2_dxf():
    d = DxfBuilder(FRAME_A4_H)
    bx_fb = (FRAME_A4_W - FB_BAND_W) / 2.0
    bx_lr = 4.50
    d.add_band(bx_fb, 8.0, FB_BAND_W, with_io=True)
    d.add_band(bx_fb, 38.0, FB_BAND_W, with_io=False)
    d.add_band(bx_lr, 68.0, LR_BAND_W, with_io=False)
    d.add_io_insert(152.0, 73.5)
    d.add_io_insert(201.5, 73.5)
    d.add_band(bx_lr, 98.0, LR_BAND_W, with_io=False)
    d.add_align_frame(FRAME_A4_W, FRAME_A4_H)
    return d.finish()


def build_single_dxf():
    d = DxfBuilder(FRAME_12_H)
    d.add_top_panel((FRAME_12_W - BODY_W) / 2.0, 5.0)
    bx_fb = (FRAME_12_W - FB_BAND_W) / 2.0
    bx_lr = 4.00
    d.add_band(bx_fb, 155.5, FB_BAND_W, with_io=True)
    d.add_band(bx_fb, 186.5, FB_BAND_W, with_io=False)
    d.add_band(bx_lr, 217.5, LR_BAND_W, with_io=False)
    d.add_io_insert(152.5, 223.0)
    d.add_io_insert(204.5, 223.0)
    d.add_band(bx_lr, 248.5, LR_BAND_W, with_io=False)
    d.add_align_frame(FRAME_12_W, FRAME_12_H)
    return d.finish()


def build_roll_to_a4_dxf():
    d = DxfBuilder(435.0)
    d.rrect(1.5, 5.0, 298.5, 215.0, 0.0, 0.0, 0.0, 0.0, segs=1)
    d.rrect(1.5, 220.0, 298.5, 430.0, 0.0, 0.0, 0.0, 0.0, segs=1)
    return d.finish()


def build_cameo_print_html(theme="light"):
    """Dedicated 2-page 1:1 US Letter (215.9 x 279.4 mm) Print-Only HTML for headless Chrome PDF export."""
    p1 = build_cameo_letter_sheet1_svg(theme=theme, include_cut=False, standalone=False)
    p2 = build_cameo_letter_sheet2_svg(theme=theme, include_cut=False, standalone=False)
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<style>
@page {{ size: 215.9mm 279.4mm; margin: 0; }}
* {{ box-sizing: border-box; -webkit-print-color-adjust: exact; print-color-adjust: exact; }}
html, body {{ margin: 0; padding: 0; background: #fff; }}
.page {{ width: 215.9mm; height: 279.4mm; overflow: hidden; page-break-after: always; }}
.page svg {{ width: 215.9mm; height: 279.4mm; display: block; }}
</style>
</head>
<body>
<div class="page">{p1}</div>
<div class="page">{p2}</div>
</body>
</html>
"""


def build_a4_print_html(theme="light"):
    """Dedicated 2-page 1:1 A4 Landscape (297 x 210 mm) Print+Preview HTML for headless Chrome PDF export."""
    s1 = build_sheet1_svg(theme=theme, cut_only=False, standalone=False, include_align=False)
    s2 = build_sheet2_svg(theme=theme, cut_only=False, standalone=False, include_align=False)
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<style>
@page {{ size: 297mm 210mm; margin: 0; }}
* {{ box-sizing: border-box; -webkit-print-color-adjust: exact; print-color-adjust: exact; }}
html, body {{ margin: 0; padding: 0; background: #fff; }}
.page {{ position: relative; width: 297mm; height: 210mm; overflow: hidden; page-break-after: always; }}
.wrap {{ position: absolute; left: 23.5mm; top: 22.5mm; width: 250mm; height: 165mm; }}
.wrap svg {{ width: 100%; height: 100%; display: block; }}
</style>
</head>
<body>
<div class="page"><div class="wrap">{s1}</div></div>
<div class="page"><div class="wrap">{s2}</div></div>
</body>
</html>
"""


# ---- Interactive HTML Studio (Detached Foreground + Solid BG Picker + Image Drag/Zoom) ----
STUDIO_HTML_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>SMC-PAD 360° Overlay Builder — Light &amp; Dark Print &amp; Cut</title>
<style id="page-size-style">@page { size: 279.4mm 215.9mm; margin: 0; }</style>
<style>
* { box-sizing: border-box; -webkit-print-color-adjust: exact; print-color-adjust: exact; }
html, body { margin: 0; padding: 0; background: #0d1117; color: #e5e7eb;
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
.studio-header { position: sticky; top: 0; z-index: 60; background: #141921; border-bottom: 1px solid #2d3542;
  box-shadow: 0 6px 20px rgba(0,0,0,0.45); }
.top-bar { padding: 8px 16px; display: flex; flex-wrap: wrap; gap: 8px 12px; align-items: center;
  border-bottom: 1px solid #232a35; font-size: 12.5px; }
.top-bar h1 { font-size: 14px; margin: 0 4px 0 0; font-weight: 800; color: #f8fafc; letter-spacing: 0.35px; }
.badge { background: #f97316; color: #0b0e14; font-weight: 800; font-size: 10.5px; padding: 2px 6px; border-radius: 4px; }
.spacer { flex: 1 1 auto; }
.layer-dock { display: grid; grid-template-columns: minmax(340px, 1fr) minmax(460px, 1.45fr); gap: 8px;
  padding: 8px 16px; background: #10141b; }
@media (max-width: 1120px) { .layer-dock { grid-template-columns: 1fr; } }
.layer-card { background: #171d26; border: 1px solid #2a3240; border-radius: 8px; padding: 7px 11px;
  display: flex; flex-wrap: wrap; gap: 7px 11px; align-items: center; font-size: 12px; }
.layer-tag { font-size: 10.5px; font-weight: 800; letter-spacing: 0.5px; text-transform: uppercase;
  padding: 2px 7px; border-radius: 4px; white-space: nowrap; }
.layer-tag.fg { background: #1e293b; color: #38bdf8; border: 1px solid #0284c7; }
.layer-tag.bg { background: #272019; color: #fb923c; border: 1px solid #ea580c; }
.ctrl { display: inline-flex; align-items: center; gap: 5px; color: #cbd5e1; font-weight: 500; white-space: nowrap; }
select, button, input[type="text"] {
  background: #212834; color: #f1f5f9; border: 1px solid #384252; border-radius: 6px;
  padding: 4px 8px; font-size: 12px; font-weight: 600; }
select, button { cursor: pointer; }
button:hover:not(:disabled), select:hover:not(:disabled) { background: #2c3545; border-color: #4b586e; }
button:disabled, input:disabled, select:disabled { opacity: 0.42; cursor: not-allowed; }
button.primary { background: #ea580c; border-color: #f97316; color: #fff; }
button.primary:hover:not(:disabled) { background: #f97316; }
button.cyan { background: #0284c7; border-color: #38bdf8; color: #fff; }
button.cyan:hover:not(:disabled) { background: #0369a1; }
button.ghost { padding: 3px 7px; font-size: 11.5px; color: #cbd5e1; }
input[type="color"] { -webkit-appearance: none; appearance: none; width: 25px; height: 24px;
  border: 1px solid #475569; border-radius: 5px; padding: 1px; background: #0d1117; cursor: pointer; }
input[type="color"]::-webkit-color-swatch-wrapper { padding: 0; }
input[type="color"]::-webkit-color-swatch { border: none; border-radius: 3px; }
input[type="range"] { width: 84px; accent-color: #f97316; cursor: pointer; margin: 0; }
.hex-in { width: 72px; font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 11.5px;
  padding: 3px 6px; text-transform: uppercase; }
.swatches { display: inline-flex; gap: 4px; align-items: center; }
.swatch { width: 17px; height: 17px; border-radius: 4px; border: 1px solid #64748b; padding: 0; cursor: pointer; }
.swatch:hover { transform: scale(1.12); border-color: #f8fafc; }
.val-pill { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 11px; color: #94a3b8;
  min-width: 34px; text-align: right; }
.img-toolbar { width: 100%; display: flex; flex-wrap: wrap; gap: 7px 10px; align-items: center;
  padding-top: 6px; border-top: 1px dashed #2b3444; }
.hint-pill { font-size: 11px; color: #38bdf8; background: #0f172a; border: 1px solid #1e3a5f;
  border-radius: 999px; padding: 2px 8px; font-weight: 600; }
.drop-shield { position: fixed; inset: 0; z-index: 200; background: rgba(14, 165, 233, 0.18);
  border: 3px dashed #38bdf8; display: none; align-items: center; justify-content: center;
  font-size: 20px; font-weight: 800; color: #f8fafc; pointer-events: none; backdrop-filter: blur(2px); }
body.drag-file .drop-shield { display: flex; }
.guide-banner { max-width: 297mm; margin: 10px auto 4px; background: #161b22; border: 1px solid #30363d;
  border-radius: 8px; padding: 10px 15px; font-size: 12.5px; line-height: 1.46; color: #cbd5e1; }
.guide-banner b { color: #f8fafc; }
.guide-banner code { background: #0d1117; padding: 1px 5px; border-radius: 4px; color: #38bdf8; font-size: 11.5px; }
.sheet-page { position: relative; width: 297mm; height: 210mm; margin: 10px auto 18px; background: #ffffff;
  box-shadow: 0 10px 32px rgba(0,0,0,0.45); overflow: hidden; page-break-after: always; user-select: none; }
.sheet-page.letter-ls-mode { width: 279.4mm; height: 215.9mm; }
.sheet-page.letter-mode { width: 215.9mm; height: 279.4mm; }
.sheet-page.single-mode { width: 304.8mm; height: 304.8mm; }
.letter-mode .sheet-svg { width: 215.9mm; height: 279.4mm; display: block; }
.frame-wrap { position: absolute; left: 23.5mm; top: 22.5mm; width: 250mm; height: 165mm; }
.letter-ls-mode .frame-wrap { left: 19.7mm; top: 30.95mm; width: 240mm; height: 154mm; }
.single-mode .frame-wrap { left: 24.4mm; top: 10.4mm; width: 256mm; height: 284mm; }
.sheet-svg { width: 100%; height: 100%; display: block; touch-action: pan-y; }
body.has-bg-image .sheet-svg { cursor: grab; }
body.has-bg-image.dragging-img .sheet-svg { cursor: grabbing; }
.reg-overlay { position: absolute; inset: 0; pointer-events: none; display: none; }
.show-reg .reg-overlay { display: block; }
.hide-cut .cut-line { display: none !important; }
.hide-align .align-frame { display: none !important; }
.hidden-layout { display: none !important; }
@media print {
  html, body { background: #fff; }
  .no-print, .cut-line, .align-frame, .cutout-preview-mask { display: none !important; }
  .sheet-page { margin: 0; box-shadow: none; }
}
</style>
</head>
<body class="show-reg">
<div class="drop-shield no-print">Drop Image Anywhere to Set Overlay Background</div>

<div class="studio-header no-print">
  <div class="top-bar">
    <h1>SMC-PAD 360° OVERLAY BUILDER</h1>
    <span class="badge">FW 925+</span>
    <label class="ctrl">Target Setup
      <select id="sel-layout">
        <option value="cameo_studio" selected>Cameo US Letter · Silhouette Studio (8.5×11" Landscape, 240×154 mm)</option>
        <option value="single">1 × Wide 12×12" Roll · Silhouette Studio (305×305 mm, 256×284 mm)</option>
        <option value="a4">2 × A4 Landscape · Silhouette Studio (297×210 mm, 250×165 mm)</option>
        <option value="cameo">Cameo US Letter · Linux Inkscape Plugin (215.9×279.4 mm)</option>
      </select>
    </label>
    <label class="ctrl"><input type="checkbox" id="chk-cut" checked> Red Cut Lines</label>
    <label class="ctrl" id="lbl-align"><input type="checkbox" id="chk-align" checked> Blue Align Frame</label>
    <label class="ctrl" title="Preview physical knob, pad, button &amp; port holes on screen while framing artwork (auto-removed on export)"><input type="checkbox" id="chk-holes"> Mask Cutout Holes</label>
    <span class="spacer"></span>
    <button type="button" id="btn-png1" class="primary">PNG 300 DPI (Sheet 1)</button>
    <button type="button" id="btn-png2" class="primary">PNG 300 DPI (Sheet 2)</button>
    <button type="button" id="btn-png-single" class="primary" style="display:none">PNG 300 DPI (12×12)</button>
    <button type="button" id="btn-svg" class="cyan">Download Print+Cut SVGs</button>
    <button type="button" id="btn-print" style="display:none" onclick="window.print()">Print / Save PDF</button>
  </div>

  <div class="layer-dock">
    <!-- Layer 1: Detached Foreground Writings & Graphics -->
    <div class="layer-card">
      <span class="layer-tag fg">1 · Theme &amp; Colors</span>
      <label class="ctrl">Theme
        <select id="sel-theme">__OPTS_HTML__</select>
      </label>
      <label class="ctrl" title="Primary text, TRACK mode, line-art &amp; border ink">Ink
        <input type="color" id="inp-fg-ink" value="#0F172A">
      </label>
      <label class="ctrl" title="HOLD STEP mode, S: knob row &amp; K1–K8 / S01–S16 badges">Hold Step
        <input type="color" id="inp-fg-step" value="#EA580C">
      </label>
      <label class="ctrl" title="KNOB BANK mode, B: knob row &amp; Shift pad shortcuts">Knob Bank
        <input type="color" id="inp-fg-pat" value="#0284C7">
      </label>
      <label class="ctrl" title="HOLD RPT timing callouts, FILL/SYNC/TAP &amp; fold ticks">Timing
        <input type="color" id="inp-fg-warn" value="#CA8A04">
      </label>
      <label class="ctrl" title="PLAY transport &amp; LOAD status callouts">Play/OK
        <input type="color" id="inp-fg-ok" value="#16A34A">
      </label>
      <button type="button" id="btn-fg-reset" class="ghost" title="Restore preset ink &amp; mode colors">Reset Ink</button>
      <label class="ctrl" title="Readout box backing color &amp; opacity (0% = pure transparent detached writings)">Card Backing
        <input type="color" id="inp-box-bg" value="#F8FAFC">
        <input type="range" id="rng-box-op" min="0" max="100" value="100">
        <span class="val-pill" id="lbl-box-op">100%</span>
      </label>
      <label class="ctrl"><input type="checkbox" id="chk-grid"> 5mm Grid</label>
    </div>

    <!-- Layer 2: Detached Background Solid Color + Uploaded Image -->
    <div class="layer-card">
      <span class="layer-tag bg">2 · Background Layer</span>
      <label class="ctrl">Solid Color
        <input type="color" id="inp-bg-color" value="#F1F3F5">
        <input type="text" id="txt-bg-hex" class="hex-in" value="#F1F3F5" maxlength="7" spellcheck="false">
      </label>
      <div class="swatches" title="Quick solid background swatches">
        <button type="button" class="swatch" data-bg="#FFFFFF" style="background:#FFFFFF" title="Pure White (#FFFFFF)"></button>
        <button type="button" class="swatch" data-bg="#F1F3F5" style="background:#F1F3F5" title="Light Gray (#F1F3F5)"></button>
        <button type="button" class="swatch" data-bg="#FEF08A" style="background:#FEF08A" title="Soft Yellow (#FEF08A)"></button>
        <button type="button" class="swatch" data-bg="#FBCFE8" style="background:#FBCFE8" title="Soft Pink (#FBCFE8)"></button>
        <button type="button" class="swatch" data-bg="#BAE6FD" style="background:#BAE6FD" title="Ice Cyan (#BAE6FD)"></button>
        <button type="button" class="swatch" data-bg="#BBF7D0" style="background:#BBF7D0" title="Mint Green (#BBF7D0)"></button>
        <button type="button" class="swatch" data-bg="#1E293B" style="background:#1E293B" title="Dark Slate (#1E293B)"></button>
        <button type="button" class="swatch" data-bg="#090A0F" style="background:#090A0F" title="Deep Black (#090A0F)"></button>
      </div>
      <label class="ctrl" title="Leave sticker background transparent (for clear/metallic/holographic vinyl)"><input type="checkbox" id="chk-bg-trans"> Transparent</label>
      <button type="button" id="btn-bg-reset" class="ghost" title="Match background color to current Writings Style">Match Style</button>

      <div class="img-toolbar">
        <input type="file" id="inp-img-file" accept="image/*" style="display:none">
        <button type="button" id="btn-img-upload" class="cyan">Upload Image…</button>
        <button type="button" id="btn-img-clear" class="ghost" disabled>Clear</button>
        <label class="ctrl">Edit
          <select id="sel-img-target" disabled>
            <option value="s1" selected>Sheet 1 (Top Deck)</option>
            <option value="s2">Sheet 2 (Side Walls)</option>
            <option value="both">Both Sheets Together</option>
          </select>
        </label>
        <label class="ctrl" title="Also show uploaded image across the 4 side bands &amp; I/O inserts"><input type="checkbox" id="chk-img-sides" checked disabled> Include Sides</label>
        <label class="ctrl">Zoom
          <input type="range" id="rng-img-zoom" min="15" max="400" value="100" disabled>
          <span class="val-pill" id="lbl-img-zoom">100%</span>
        </label>
        <label class="ctrl">Rotate
          <input type="range" id="rng-img-rot" min="-180" max="180" value="0" disabled>
          <span class="val-pill" id="lbl-img-rot">0°</span>
        </label>
        <button type="button" id="btn-img-rot90" class="ghost" disabled title="Rotate +90°">↻ 90°</button>
        <label class="ctrl"><input type="checkbox" id="chk-img-flip" disabled> Flip H</label>
        <button type="button" id="btn-img-cover" class="ghost" disabled title="Fill sticker area completely">Cover</button>
        <button type="button" id="btn-img-fit" class="ghost" disabled title="Fit entire image inside sticker">Fit</button>
        <button type="button" id="btn-img-center" class="ghost" disabled title="Re-center image (0, 0 mm)">Center</button>
        <label class="ctrl" title="Blend solid background color over the uploaded image to boost text legibility">Tint Veil
          <input type="range" id="rng-bg-veil" min="0" max="85" value="0" disabled>
          <span class="val-pill" id="lbl-bg-veil">0%</span>
        </label>
        <span class="hint-pill" id="lbl-img-status">Drop or upload any image · drag on sheet to move · Alt/Ctrl+Scroll to zoom</span>
      </div>
    </div>
  </div>
</div>

<div class="guide-banner no-print" id="banner-mie-studio">
  <b>Cameo Silhouette Cameo 5 + Silhouette Studio (Free / Basic Edition) on US Letter (<code>8.5 × 11 in</code>):</b><br>
  <b>1. Customize &amp; Export:</b> Pick any <b>Writings Style</b> + <b>Solid Background Color</b> or <b>Upload Image</b> (drag directly on Sheet 1 / Sheet 2 to position, <code>Alt/Ctrl + Scroll</code> or use <b>Zoom</b> to resize, and adjust <b>Tint Veil</b> or <b>Card Backing</b> for contrast). Click <b>PNG 300 DPI (Sheet 1)</b> &amp; <b>(Sheet 2)</b> — cut lines &amp; hole masks are stripped automatically and a <code>300 DPI</code> header is embedded so Silhouette Studio imports at exact <code>240.0 × 154.0 mm</code>!<br>
  <b>2. Pair PNG + DXF at <code>0.00 mm</code> error:</b> In Silhouette Studio, set <code>Letter (8.5 × 11 in) Landscape</code> + <b>Registration Marks: On</b> (default Type 1). Open your exported <code>Sheet 1</code> PNG → <b>Center to Page</b>. Open <code>smcpad_cameo_letter_sheet1_top_cut.dxf</code> → <b><code>Cmd+A → Cmd+G</code> (Group!)</b> → paste &amp; <b>Center to Page</b> → in <b>Send</b>, set PNG &amp; outer <code>240 × 154 mm</code> frame to <b><code>No Cut</code></b>. Print at <b>100% Actual Size</b> (no 6 mm X-offset needed in Silhouette Studio!).
</div>

<div class="guide-banner no-print hidden-layout" id="banner-cameo">
  <b>Cameo Silhouette Cameo 5 + Linux Inkscape (<code>inkscape-silhouette</code>) Workflow:</b><br>
  <b>1. Print (US Letter <code>8.5 × 11 in</code> = <code>215.9 × 279.4 mm</code>):</b> Click <b>Print / Save PDF</b> or print <code>smcpad_cameo_*_print_letter.pdf</code> at <b>100% Actual Size</b>.<br>
  <b>2. Cut from Inkscape:</b> Open <code>smcpad_cameo_*_sheet1_letter.svg</code> → <code>Extensions → Export → Send to Silhouette...</code> → Tool: <code>AutoBlade</code>, Cutting Mat: <code>Portrait 4 8.5x12</code>, <b><code>X-Offset = 6 mm</code></b> → <b>Regmarks tab:</b> check both boxes, <b>change Left &amp; Top from <code>10</code> to <code>0</code> (or <code>15</code>)</b>, and leave X/Y distances at <code>0</code>.
</div>

<div class="guide-banner no-print hidden-layout" id="banner-studio">
  <b>Silhouette Studio (A4 Landscape or 12×12" Wide Roll) Workflow:</b>
  Click <b>PNG 300 DPI</b> above (red cut lines, hole masks &amp; blue align frame are stripped automatically, with <code>300 DPI</code> physical scale embedded), then pair with the matching <code>_cut.dxf</code> alignment frame (<code>250×165 mm</code> for A4 or <code>256×284 mm</code> for 12×12") in Silhouette Studio using <b>Group + Center to Page</b>.
</div>

<!-- Default Mode: Cameo US Letter Landscape (279.4 x 215.9 mm) for Silhouette Studio -->
<div id="wrap-mie-studio">
  <div class="sheet-page letter-ls-mode" id="page-ms1">
    <svg class="reg-overlay no-print" viewBox="0 0 279.4 215.9" xmlns="http://www.w3.org/2000/svg">
      <rect x="0" y="0" width="279.4" height="28.575" fill="#94a3b8" fill-opacity="0.12"/>
      <rect x="0" y="187.325" width="28.575" height="28.575" fill="#94a3b8" fill-opacity="0.12"/>
      <rect x="15.875" y="15.875" width="5" height="5" fill="#000"/>
      <path d="M263.525,15.875 h-12.7 v0.9 h11.8 v11.8 h0.9 z" fill="#000"/>
      <path d="M15.875,200.025 h12.7 v-0.9 h-11.8 v-11.8 h-0.9 z" fill="#000"/>
      <rect x="15.875" y="15.875" width="247.65" height="184.15" fill="none" stroke="#ef4444" stroke-width="0.22" stroke-dasharray="1.5 1.5"/>
    </svg>
    <div class="frame-wrap">__MS_S1__</div>
  </div>
  <div class="sheet-page letter-ls-mode" id="page-ms2">
    <svg class="reg-overlay no-print" viewBox="0 0 279.4 215.9" xmlns="http://www.w3.org/2000/svg">
      <rect x="0" y="0" width="279.4" height="28.575" fill="#94a3b8" fill-opacity="0.12"/>
      <rect x="0" y="187.325" width="28.575" height="28.575" fill="#94a3b8" fill-opacity="0.12"/>
      <rect x="15.875" y="15.875" width="5" height="5" fill="#000"/>
      <path d="M263.525,15.875 h-12.7 v0.9 h11.8 v11.8 h0.9 z" fill="#000"/>
      <path d="M15.875,200.025 h12.7 v-0.9 h-11.8 v-11.8 h-0.9 z" fill="#000"/>
      <rect x="15.875" y="15.875" width="247.65" height="184.15" fill="none" stroke="#ef4444" stroke-width="0.22" stroke-dasharray="1.5 1.5"/>
    </svg>
    <div class="frame-wrap">__MS_S2__</div>
  </div>
</div>

<!-- Alternate Mode: Cameo US Letter Portrait (215.9 x 279.4 mm) for Linux Inkscape Plugin -->
<div id="wrap-cameo" class="hidden-layout">
  <div class="sheet-page letter-mode" id="page-mie-s1">__MIE_S1__</div>
  <div class="sheet-page letter-mode" id="page-mie-s2">__MIE_S2__</div>
</div>

<!-- Alternate Mode A: 2 x A4 Landscape Sheets -->
<div id="wrap-a4" class="hidden-layout">
  <div class="sheet-page" id="page-s1">
    <svg class="reg-overlay no-print" viewBox="0 0 297 210" xmlns="http://www.w3.org/2000/svg">
      <rect x="15.9" y="15.9" width="5" height="5" fill="#000"/>
      <path d="M281.1,15.9 h-12.7 v1.1 h11.6 v11.6 h1.1 z" fill="#000"/>
      <path d="M15.9,194.1 h12.7 v-1.1 h-11.6 v-11.6 h-1.1 z" fill="#000"/>
      <rect x="15.9" y="15.9" width="265.2" height="178.2" fill="none" stroke="#94a3b8" stroke-width="0.2" stroke-dasharray="1.5 1.5"/>
    </svg>
    <div class="frame-wrap">__S1_SVG__</div>
  </div>

  <div class="sheet-page" id="page-s2">
    <svg class="reg-overlay no-print" viewBox="0 0 297 210" xmlns="http://www.w3.org/2000/svg">
      <rect x="15.9" y="15.9" width="5" height="5" fill="#000"/>
      <path d="M281.1,15.9 h-12.7 v1.1 h11.6 v11.6 h1.1 z" fill="#000"/>
      <path d="M15.9,194.1 h12.7 v-1.1 h-11.6 v-11.6 h-1.1 z" fill="#000"/>
      <rect x="15.9" y="15.9" width="265.2" height="178.2" fill="none" stroke="#94a3b8" stroke-width="0.2" stroke-dasharray="1.5 1.5"/>
    </svg>
    <div class="frame-wrap">__S2_SVG__</div>
  </div>
</div>

<!-- Alternate Mode B: Single 12x12" (304.8 x 304.8 mm) Wide Sheet -->
<div id="wrap-single" class="hidden-layout">
  <div class="sheet-page single-mode" id="page-sg">
    <svg class="reg-overlay no-print" viewBox="0 0 304.8 304.8" xmlns="http://www.w3.org/2000/svg">
      <rect x="10.0" y="10.0" width="5" height="5" fill="#000"/>
      <path d="M294.8,10.0 h-12.7 v1.1 h11.6 v11.6 h1.1 z" fill="#000"/>
      <path d="M10.0,294.8 h12.7 v-1.1 h-11.6 v-11.6 h-1.1 z" fill="#000"/>
    </svg>
    <div class="frame-wrap">__SG_SVG__</div>
  </div>
</div>

<script>
(function() {
  const THEMES = __THEMES_JSON__;
  const DEFAULT_THEME = __DEFAULT_THEME__;
  const SVG_NS = 'http://www.w3.org/2000/svg';
  const XLINK_NS = 'http://www.w3.org/1999/xlink';

  const state = {
    theme: DEFAULT_THEME,
    fgInkCustom: false,
    fgInk: THEMES[DEFAULT_THEME].ink,
    fgStepCustom: false,
    fgStep: THEMES[DEFAULT_THEME].step,
    fgPatCustom: false,
    fgPat: THEMES[DEFAULT_THEME].pat,
    fgWarnCustom: false,
    fgWarn: THEMES[DEFAULT_THEME].warn,
    fgOkCustom: false,
    fgOk: THEMES[DEFAULT_THEME].ok,
    boxBgCustom: false,
    boxBg: THEMES[DEFAULT_THEME].box_bg,
    boxOpacityUserSet: false,
    boxOpacity: 100,
    showGrid: false,
    bgCustom: false,
    bgColor: THEMES[DEFAULT_THEME].bg,
    bgTransparent: false,
    veilOpacity: 0,
    showHoleMask: false,
  };

  const imgState = {
    dataUrl: null,
    fileName: '',
    natW: 1,
    natH: 1,
    srcVer: 0,
    includeSides: true,
    slots: {
      s1: { scale: 1.0, dx: 0.0, dy: 0.0, rot: 0.0, flipH: false },
      s2: { scale: 1.0, dx: 0.0, dy: 0.0, rot: 0.0, flipH: false },
    },
  };

  // DOM Elements
  const selTheme = document.getElementById('sel-theme');
  const selLayout = document.getElementById('sel-layout');
  const pageStyle = document.getElementById('page-size-style');
  const chkCut = document.getElementById('chk-cut');
  const chkAlign = document.getElementById('chk-align');
  const chkHoles = document.getElementById('chk-holes');
  const inpFgInk = document.getElementById('inp-fg-ink');
  const inpFgStep = document.getElementById('inp-fg-step');
  const inpFgPat = document.getElementById('inp-fg-pat');
  const inpFgWarn = document.getElementById('inp-fg-warn');
  const inpFgOk = document.getElementById('inp-fg-ok');
  const btnFgReset = document.getElementById('btn-fg-reset');
  const inpBoxBg = document.getElementById('inp-box-bg');
  const rngBoxOp = document.getElementById('rng-box-op');
  const lblBoxOp = document.getElementById('lbl-box-op');
  const chkGrid = document.getElementById('chk-grid');

  const inpBgColor = document.getElementById('inp-bg-color');
  const txtBgHex = document.getElementById('txt-bg-hex');
  const chkBgTrans = document.getElementById('chk-bg-trans');
  const btnBgReset = document.getElementById('btn-bg-reset');

  const inpImgFile = document.getElementById('inp-img-file');
  const btnImgUpload = document.getElementById('btn-img-upload');
  const btnImgClear = document.getElementById('btn-img-clear');
  const selImgTarget = document.getElementById('sel-img-target');
  const chkImgSides = document.getElementById('chk-img-sides');
  const rngImgZoom = document.getElementById('rng-img-zoom');
  const lblImgZoom = document.getElementById('lbl-img-zoom');
  const rngImgRot = document.getElementById('rng-img-rot');
  const lblImgRot = document.getElementById('lbl-img-rot');
  const btnImgRot90 = document.getElementById('btn-img-rot90');
  const chkImgFlip = document.getElementById('chk-img-flip');
  const btnImgCover = document.getElementById('btn-img-cover');
  const btnImgFit = document.getElementById('btn-img-fit');
  const btnImgCenter = document.getElementById('btn-img-center');
  const rngBgVeil = document.getElementById('rng-bg-veil');
  const lblBgVeil = document.getElementById('lbl-bg-veil');
  const lblImgStatus = document.getElementById('lbl-img-status');

  function normHex(v) {
    const s = String(v || '').trim();
    if (/^#[0-9a-fA-F]{6}$/.test(s)) return s.toUpperCase();
    if (/^[0-9a-fA-F]{6}$/.test(s)) return '#' + s.toUpperCase();
    if (/^#[0-9a-fA-F]{3}$/.test(s)) {
      return ('#' + s[1] + s[1] + s[2] + s[2] + s[3] + s[3]).toUpperCase();
    }
    return null;
  }

  function contrastTextFor(hex) {
    const h = normHex(hex) || '#111318';
    const r = parseInt(h.slice(1, 3), 16) / 255;
    const g = parseInt(h.slice(3, 5), 16) / 255;
    const b = parseInt(h.slice(5, 7), 16) / 255;
    const lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    return lum > 0.48 ? '#090A0C' : '#FFFFFF';
  }

  function buildDynamicCss() {
    const t = THEMES[state.theme] || THEMES.light;
    const ink = state.fgInkCustom ? state.fgInk : t.ink;
    const muted = state.fgInkCustom ? state.fgInk : t.muted;
    const mutedOp = state.fgInkCustom ? 'fill-opacity: 0.78;' : '';
    const rule = state.fgInkCustom ? state.fgInk : t.rule;
    const ruleHi = state.fgInkCustom ? state.fgInk : t.rule_hi;
    const trace = state.fgInkCustom ? state.fgInk : t.trace;
    const gridMaj = state.fgInkCustom ? state.fgInk : t.grid_maj;
    const gridMinColor = state.fgInkCustom ? state.fgInk : t.grid_min;

    const step = state.fgStepCustom ? state.fgStep : t.step;
    const pat = state.fgPatCustom ? state.fgPat : t.pat;
    const warn = state.fgWarnCustom ? state.fgWarn : t.warn;
    const ok = state.fgOkCustom ? state.fgOk : t.ok;
    const stepFg = state.fgStepCustom ? contrastTextFor(step) : t.step_fg;

    const boxFill = state.boxBgCustom ? state.boxBg : t.box_bg;
    const boxAltFill = state.boxBgCustom ? state.boxBg : t.box_alt;
    const boxOp = state.boxOpacity / 100;
    const boxFillDecl = (state.boxOpacity <= 0)
      ? 'fill: none;'
      : `fill: ${boxFill}; fill-opacity: ${boxOp.toFixed(3)};`;
    const boxAltDecl = (state.boxOpacity <= 0)
      ? 'fill: none;'
      : `fill: ${boxAltFill}; fill-opacity: ${boxOp.toFixed(3)};`;

    const bgDecl = state.bgTransparent ? 'fill: none;' : `fill: ${state.bgColor};`;
    const paperDecl = state.bgTransparent ? 'fill: none;' : 'fill: #ffffff;';
    const veilOp = state.bgTransparent ? 0 : (state.veilOpacity / 100);

    return `
      .cut-line { fill: none; stroke: #ff0000; stroke-width: 0.12; stroke-linecap: round; stroke-linejoin: round; }
      .align-frame { fill: none; stroke: #0066ff; stroke-width: 0.10; stroke-dasharray: 2.0 1.2; }
      .sheet-paper-bg { ${paperDecl} stroke: #cbd5e1; stroke-width: 0.16; }
      .sheet-annotations text { fill: #334155 !important; }
      .sheet-ruler .box-bg { fill: #ffffff !important; fill-opacity: 1 !important; stroke: #0f172a !important; }
      .sheet-ruler .ink, .sheet-ruler .muted, .sheet-ruler .step-c, .sheet-ruler .pat-c, .sheet-ruler .warn-c { fill: #0f172a !important; fill-opacity: 1 !important; }
      .sheet-ruler .sc-rule, .sheet-ruler .sc-rule-hi { stroke: #0f172a !important; }
      .cutout-preview-mask { display: ${state.showHoleMask ? 'inline' : 'none'}; fill: #0b0e14; fill-opacity: 0.78; stroke: #94a3b8; stroke-width: 0.15; pointer-events: none; }
      .seam-tick { fill: none; stroke: ${warn}; stroke-width: 0.20; stroke-dasharray: 0.9 0.7; }
      .bg-base { ${bgDecl} }
      .bg-veil { fill: ${state.bgColor}; fill-opacity: ${veilOp.toFixed(3)}; pointer-events: none; }
      .panel-sub { ${bgDecl} }
      .box-bg { ${boxFillDecl} stroke: ${rule}; stroke-width: 0.16; }
      .box-alt { ${boxAltDecl} stroke: ${rule}; stroke-width: 0.15; }
      .cad-grid-lines { display: ${state.showGrid ? 'inline' : 'none'}; }
      .grid-min { stroke: ${gridMinColor}; stroke-width: 0.07; fill: none; }
      .grid-maj { stroke: ${gridMaj}; stroke-width: 0.12; fill: none; }
      .sc-trace { stroke: ${trace}; stroke-width: 0.18; fill: none; stroke-linecap: round; stroke-linejoin: round; }
      .sc-rule { stroke: ${rule}; fill: none; }
      .sc-rule-hi { stroke: ${ruleHi}; fill: none; }
      .ink { fill: ${ink}; }
      .muted { fill: ${muted}; ${mutedOp} }
      .step-c { fill: ${step}; }
      .step-bg { fill: ${step}; }
      .step-fg { fill: ${stepFg}; }
      .pat-c { fill: ${pat}; }
      .warn-c { fill: ${warn}; }
      .warn-stroke { stroke: ${warn}; fill: none; }
      .ok-c { fill: ${ok}; }
      .f-cond { font-family: "Roboto Condensed", "Avenir Next Condensed", "Bahnschrift", "Liberation Sans Narrow", "Arial Narrow", sans-serif; }
      .f-mono { font-family: "Noto Sans Mono", "JetBrains Mono", "SF Mono", "DejaVu Sans Mono", "Liberation Mono", monospace; }
    `;
  }

  function syncControlsFromState() {
    const t = THEMES[state.theme] || THEMES.light;
    const effInk = state.fgInkCustom ? state.fgInk : t.ink;
    const effStep = state.fgStepCustom ? state.fgStep : t.step;
    const effPat = state.fgPatCustom ? state.fgPat : t.pat;
    const effWarn = state.fgWarnCustom ? state.fgWarn : t.warn;
    const effOk = state.fgOkCustom ? state.fgOk : t.ok;
    inpFgInk.value = normHex(effInk) || '#0F172A';
    inpFgStep.value = normHex(effStep) || '#EA580C';
    inpFgPat.value = normHex(effPat) || '#0284C7';
    inpFgWarn.value = normHex(effWarn) || '#CA8A04';
    inpFgOk.value = normHex(effOk) || '#16A34A';
    inpBoxBg.value = normHex(state.boxBgCustom ? state.boxBg : t.box_bg) || '#F8FAFC';
    rngBoxOp.value = String(state.boxOpacity);
    lblBoxOp.textContent = state.boxOpacity + '%';
    chkGrid.checked = state.showGrid;
    inpBgColor.value = normHex(state.bgColor) || '#F1F3F5';
    txtBgHex.value = normHex(state.bgColor) || '#F1F3F5';
    chkBgTrans.checked = state.bgTransparent;
    chkHoles.checked = state.showHoleMask;
    rngBgVeil.value = String(state.veilOpacity);
    lblBgVeil.textContent = state.veilOpacity + '%';

    const hasImg = Boolean(imgState.dataUrl);
    [
      btnImgClear, selImgTarget, chkImgSides, rngImgZoom, rngImgRot,
      btnImgRot90, chkImgFlip, btnImgCover, btnImgFit, btnImgCenter, rngBgVeil
    ].forEach(el => { el.disabled = !hasImg; });
    document.body.classList.toggle('has-bg-image', hasImg);

    const activeSlot = (selImgTarget.value === 's2') ? 's2' : 's1';
    const xf = imgState.slots[activeSlot];
    const zPct = Math.round(xf.scale * 100);
    rngImgZoom.value = String(Math.max(15, Math.min(400, zPct)));
    lblImgZoom.textContent = zPct + '%';
    rngImgRot.value = String(Math.round(xf.rot));
    lblImgRot.textContent = Math.round(xf.rot) + '°';
    chkImgFlip.checked = xf.flipH;
    chkImgSides.checked = imgState.includeSides;

    if (hasImg) {
      const tag = selImgTarget.value === 'both' ? 'S1+S2' : activeSlot.toUpperCase();
      lblImgStatus.textContent =
        `${imgState.fileName} (${tag}: ${zPct}% · X ${xf.dx >= 0 ? '+' : ''}${xf.dx.toFixed(1)}, Y ${xf.dy >= 0 ? '+' : ''}${xf.dy.toFixed(1)} mm) — drag sheet to move`;
    } else {
      lblImgStatus.textContent = 'Drop or upload any image · drag on sheet to move · Alt/Ctrl+Scroll to zoom';
    }
  }

  function applyThemeStyles() {
    const css = buildDynamicCss();
    document.querySelectorAll('style.theme-style').forEach(st => { st.textContent = css; });
    syncControlsFromState();
  }

  function renderBgImages() {
    const layers = document.querySelectorAll('.piece-bg-layer');
    layers.forEach(layer => {
      const slotGroup = layer.querySelector('.bg-image-slot');
      if (!slotGroup) return;
      const sheetKey = layer.getAttribute('data-sheet') || 's1';
      const pieceKind = layer.getAttribute('data-piece') || 'top';
      const showHere = Boolean(imgState.dataUrl) && (pieceKind === 'top' || imgState.includeSides);
      if (!showHere) {
        if (slotGroup.firstChild) slotGroup.innerHTML = '';
        return;
      }

      const rx = parseFloat(layer.getAttribute('data-rx'));
      const ry = parseFloat(layer.getAttribute('data-ry'));
      const rw = parseFloat(layer.getAttribute('data-rw'));
      const rh = parseFloat(layer.getAttribute('data-rh'));
      const xf = imgState.slots[sheetKey] || imgState.slots.s1;

      const coverScale = Math.max(rw / imgState.natW, rh / imgState.natH);
      const w = imgState.natW * coverScale * xf.scale;
      const h = imgState.natH * coverScale * xf.scale;
      const cx = rx + rw / 2 + xf.dx;
      const cy = ry + rh / 2 + xf.dy;
      const ix = cx - w / 2;
      const iy = cy - h / 2;

      let imgEl = slotGroup.querySelector('image');
      if (!imgEl) {
        imgEl = document.createElementNS(SVG_NS, 'image');
        imgEl.setAttribute('preserveAspectRatio', 'none');
        slotGroup.appendChild(imgEl);
      }
      if (imgEl.getAttribute('data-ver') !== String(imgState.srcVer)) {
        imgEl.setAttribute('href', imgState.dataUrl);
        imgEl.setAttributeNS(XLINK_NS, 'xlink:href', imgState.dataUrl);
        imgEl.setAttribute('data-ver', String(imgState.srcVer));
      }
      imgEl.setAttribute('x', ix.toFixed(3));
      imgEl.setAttribute('y', iy.toFixed(3));
      imgEl.setAttribute('width', w.toFixed(3));
      imgEl.setAttribute('height', h.toFixed(3));
      if (xf.rot !== 0 || xf.flipH) {
        imgEl.setAttribute(
          'transform',
          `translate(${cx.toFixed(3)},${cy.toFixed(3)}) rotate(${xf.rot.toFixed(2)}) scale(${xf.flipH ? -1 : 1},1) translate(${(-cx).toFixed(3)},${(-cy).toFixed(3)})`
        );
      } else {
        imgEl.removeAttribute('transform');
      }
    });
    syncControlsFromState();
  }

  function targetSlotKeys() {
    if (selImgTarget.value === 'both') return ['s1', 's2'];
    return [selImgTarget.value === 's2' ? 's2' : 's1'];
  }

  function detachCardBackingIfUntouched() {
    if (!state.boxOpacityUserSet) {
      state.boxOpacity = 0;
    }
  }

  function resetFgColorsToTheme(t) {
    state.fgInkCustom = false;
    state.fgInk = t.ink;
    state.fgStepCustom = false;
    state.fgStep = t.step;
    state.fgPatCustom = false;
    state.fgPat = t.pat;
    state.fgWarnCustom = false;
    state.fgWarn = t.warn;
    state.fgOkCustom = false;
    state.fgOk = t.ok;
  }

  // ---- Foreground Controls ----
  selTheme.addEventListener('change', () => {
    state.theme = selTheme.value;
    const t = THEMES[state.theme] || THEMES.light;
    resetFgColorsToTheme(t);
    if (!state.boxBgCustom) state.boxBg = t.box_bg;
    if (!state.bgCustom && !imgState.dataUrl) {
      state.bgColor = t.bg;
      if (!state.boxOpacityUserSet) {
        state.boxOpacity = 100;
      }
    }
    applyThemeStyles();
  });

  inpFgInk.addEventListener('input', () => {
    state.fgInkCustom = true;
    state.fgInk = inpFgInk.value;
    applyThemeStyles();
  });

  inpFgStep.addEventListener('input', () => {
    state.fgStepCustom = true;
    state.fgStep = inpFgStep.value;
    applyThemeStyles();
  });

  inpFgPat.addEventListener('input', () => {
    state.fgPatCustom = true;
    state.fgPat = inpFgPat.value;
    applyThemeStyles();
  });

  inpFgWarn.addEventListener('input', () => {
    state.fgWarnCustom = true;
    state.fgWarn = inpFgWarn.value;
    applyThemeStyles();
  });

  inpFgOk.addEventListener('input', () => {
    state.fgOkCustom = true;
    state.fgOk = inpFgOk.value;
    applyThemeStyles();
  });

  btnFgReset.addEventListener('click', () => {
    const t = THEMES[state.theme] || THEMES.light;
    resetFgColorsToTheme(t);
    applyThemeStyles();
  });

  inpBoxBg.addEventListener('input', () => {
    state.boxBgCustom = true;
    state.boxBg = inpBoxBg.value;
    if (state.boxOpacity === 0 && !state.boxOpacityUserSet) {
      state.boxOpacity = 75;
    }
    applyThemeStyles();
  });

  rngBoxOp.addEventListener('input', () => {
    state.boxOpacityUserSet = true;
    state.boxOpacity = parseInt(rngBoxOp.value, 10) || 0;
    applyThemeStyles();
  });

  chkGrid.addEventListener('change', () => {
    state.showGrid = chkGrid.checked;
    applyThemeStyles();
  });

  // ---- Background Color Controls ----
  function setSolidBgColor(hex) {
    const clean = normHex(hex);
    if (!clean) return;
    state.bgCustom = true;
    state.bgTransparent = false;
    state.bgColor = clean;
    detachCardBackingIfUntouched();
    applyThemeStyles();
  }

  inpBgColor.addEventListener('input', () => setSolidBgColor(inpBgColor.value));
  txtBgHex.addEventListener('change', () => {
    const clean = normHex(txtBgHex.value);
    if (clean) setSolidBgColor(clean);
    else txtBgHex.value = normHex(state.bgColor) || '#F1F3F5';
  });
  document.querySelectorAll('.swatch').forEach(btn => {
    btn.addEventListener('click', () => setSolidBgColor(btn.getAttribute('data-bg')));
  });

  chkBgTrans.addEventListener('change', () => {
    state.bgTransparent = chkBgTrans.checked;
    if (state.bgTransparent) detachCardBackingIfUntouched();
    applyThemeStyles();
  });

  btnBgReset.addEventListener('click', () => {
    const t = THEMES[state.theme] || THEMES.light;
    state.bgCustom = false;
    state.bgTransparent = false;
    state.bgColor = t.bg;
    if (!imgState.dataUrl && !state.boxOpacityUserSet) {
      state.boxOpacity = 100;
    }
    applyThemeStyles();
  });

  // ---- Background Image Upload, Drag, Wheel-Zoom & Transform Controls ----
  function loadImageFile(file) {
    if (!file || !file.type || !file.type.startsWith('image/')) return;
    const reader = new FileReader();
    reader.onload = ev => {
      const dataUrl = String(ev.target.result || '');
      const probe = new Image();
      probe.onload = () => {
        imgState.dataUrl = dataUrl;
        imgState.fileName = file.name || 'uploaded-image';
        imgState.natW = Math.max(1, probe.naturalWidth || probe.width || 1);
        imgState.natH = Math.max(1, probe.naturalHeight || probe.height || 1);
        imgState.srcVer += 1;
        imgState.slots.s1 = { scale: 1.0, dx: 0.0, dy: 0.0, rot: 0.0, flipH: false };
        imgState.slots.s2 = { scale: 1.0, dx: 0.0, dy: 0.0, rot: 0.0, flipH: false };
        detachCardBackingIfUntouched();
        state.showHoleMask = true;
        applyThemeStyles();
        renderBgImages();
      };
      probe.src = dataUrl;
    };
    reader.readAsDataURL(file);
  }

  btnImgUpload.addEventListener('click', () => inpImgFile.click());
  inpImgFile.addEventListener('change', () => {
    if (inpImgFile.files && inpImgFile.files[0]) {
      loadImageFile(inpImgFile.files[0]);
      inpImgFile.value = '';
    }
  });

  btnImgClear.addEventListener('click', () => {
    imgState.dataUrl = null;
    imgState.fileName = '';
    state.veilOpacity = 0;
    state.showHoleMask = false;
    if (!state.bgCustom && !state.boxOpacityUserSet) {
      state.boxOpacity = 100;
    }
    applyThemeStyles();
    renderBgImages();
  });

  selImgTarget.addEventListener('change', () => syncControlsFromState());

  chkImgSides.addEventListener('change', () => {
    imgState.includeSides = chkImgSides.checked;
    renderBgImages();
  });

  rngImgZoom.addEventListener('input', () => {
    const s = Math.max(0.10, parseInt(rngImgZoom.value, 10) / 100);
    targetSlotKeys().forEach(k => { imgState.slots[k].scale = s; });
    renderBgImages();
  });

  rngImgRot.addEventListener('input', () => {
    const r = parseFloat(rngImgRot.value) || 0;
    targetSlotKeys().forEach(k => { imgState.slots[k].rot = r; });
    renderBgImages();
  });

  btnImgRot90.addEventListener('click', () => {
    targetSlotKeys().forEach(k => {
      let nr = imgState.slots[k].rot + 90;
      if (nr > 180) nr -= 360;
      imgState.slots[k].rot = nr;
    });
    renderBgImages();
  });

  chkImgFlip.addEventListener('change', () => {
    targetSlotKeys().forEach(k => { imgState.slots[k].flipH = chkImgFlip.checked; });
    renderBgImages();
  });

  btnImgCover.addEventListener('click', () => {
    targetSlotKeys().forEach(k => {
      imgState.slots[k].scale = 1.0;
      imgState.slots[k].dx = 0.0;
      imgState.slots[k].dy = 0.0;
    });
    renderBgImages();
  });

  function representativeRefRect(slotKey) {
    const mode = selLayout.value;
    const wrapId = mode === 'cameo_studio' ? 'wrap-mie-studio'
      : mode === 'cameo' ? 'wrap-cameo'
      : mode === 'a4' ? 'wrap-a4' : 'wrap-single';
    const el = document.querySelector(`#${wrapId} .piece-bg-layer[data-sheet="${slotKey}"]`)
      || document.querySelector(`.piece-bg-layer[data-sheet="${slotKey}"]`);
    if (!el) return { rx: 0, ry: 0, rw: 225, rh: 145 };
    return {
      rx: parseFloat(el.getAttribute('data-rx')),
      ry: parseFloat(el.getAttribute('data-ry')),
      rw: parseFloat(el.getAttribute('data-rw')),
      rh: parseFloat(el.getAttribute('data-rh')),
    };
  }

  btnImgFit.addEventListener('click', () => {
    targetSlotKeys().forEach(k => {
      const { rw, rh } = representativeRefRect(k);
      const cover = Math.max(rw / imgState.natW, rh / imgState.natH);
      const contain = Math.min(rw / imgState.natW, rh / imgState.natH);
      imgState.slots[k].scale = contain / cover;
      imgState.slots[k].dx = 0.0;
      imgState.slots[k].dy = 0.0;
    });
    renderBgImages();
  });

  btnImgCenter.addEventListener('click', () => {
    targetSlotKeys().forEach(k => {
      imgState.slots[k].dx = 0.0;
      imgState.slots[k].dy = 0.0;
    });
    renderBgImages();
  });

  rngBgVeil.addEventListener('input', () => {
    state.veilOpacity = parseInt(rngBgVeil.value, 10) || 0;
    applyThemeStyles();
  });

  // Full-window drag-and-drop image support
  let dragDepth = 0;
  window.addEventListener('dragenter', e => {
    if (e.dataTransfer && Array.from(e.dataTransfer.types || []).includes('Files')) {
      dragDepth += 1;
      document.body.classList.add('drag-file');
    }
  });
  window.addEventListener('dragover', e => {
    if (e.dataTransfer && Array.from(e.dataTransfer.types || []).includes('Files')) {
      e.preventDefault();
    }
  });
  window.addEventListener('dragleave', () => {
    dragDepth = Math.max(0, dragDepth - 1);
    if (dragDepth === 0) document.body.classList.remove('drag-file');
  });
  window.addEventListener('drop', e => {
    dragDepth = 0;
    document.body.classList.remove('drag-file');
    if (e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0]) {
      e.preventDefault();
      loadImageFile(e.dataTransfer.files[0]);
    }
  });

  // Direct on-sheet pointer drag + modifier-wheel zoom
  function resolveBgLayerFromEvent(svg, e) {
    const printGrp = e.target.closest && e.target.closest('g[id$="-print"]');
    if (printGrp) {
      const found = printGrp.querySelector('.piece-bg-layer');
      if (found) return found;
    }
    return svg.querySelector('.piece-bg-layer');
  }

  function clientToLayerMm(layer, clientX, clientY) {
    const ctm = layer.getScreenCTM();
    if (!ctm) return { x: clientX, y: clientY };
    const inv = ctm.inverse();
    return {
      x: inv.a * clientX + inv.c * clientY + inv.e,
      y: inv.b * clientX + inv.d * clientY + inv.f,
    };
  }

  let dragCtx = null;
  document.querySelectorAll('svg.sheet-svg').forEach(svg => {
    svg.addEventListener('pointerdown', e => {
      if (!imgState.dataUrl || e.button !== 0) return;
      const layer = resolveBgLayerFromEvent(svg, e);
      if (!layer) return;
      const slot = layer.getAttribute('data-sheet') || 's1';
      if (slot === 's2' && !imgState.includeSides) return;
      if (selImgTarget.value !== 'both') {
        selImgTarget.value = slot;
      }
      const pt = clientToLayerMm(layer, e.clientX, e.clientY);
      dragCtx = {
        pointerId: e.pointerId,
        svg,
        layer,
        slots: (selImgTarget.value === 'both') ? ['s1', 's2'] : [slot],
        startX: pt.x,
        startY: pt.y,
        orig: {
          s1: { dx: imgState.slots.s1.dx, dy: imgState.slots.s1.dy },
          s2: { dx: imgState.slots.s2.dx, dy: imgState.slots.s2.dy },
        },
      };
      document.body.classList.add('dragging-img');
      try { svg.setPointerCapture(e.pointerId); } catch (_) {}
      e.preventDefault();
      syncControlsFromState();
    });

    svg.addEventListener('pointermove', e => {
      if (!dragCtx || e.pointerId !== dragCtx.pointerId) return;
      const pt = clientToLayerMm(dragCtx.layer, e.clientX, e.clientY);
      const ddx = pt.x - dragCtx.startX;
      const ddy = pt.y - dragCtx.startY;
      dragCtx.slots.forEach(k => {
        imgState.slots[k].dx = dragCtx.orig[k].dx + ddx;
        imgState.slots[k].dy = dragCtx.orig[k].dy + ddy;
      });
      renderBgImages();
    });

    const endDrag = e => {
      if (!dragCtx || (e && e.pointerId !== undefined && e.pointerId !== dragCtx.pointerId)) return;
      try { svg.releasePointerCapture(dragCtx.pointerId); } catch (_) {}
      dragCtx = null;
      document.body.classList.remove('dragging-img');
    };
    svg.addEventListener('pointerup', endDrag);
    svg.addEventListener('pointercancel', endDrag);

    svg.addEventListener('wheel', e => {
      if (!imgState.dataUrl || !(e.ctrlKey || e.metaKey || e.altKey || e.shiftKey)) return;
      const layer = resolveBgLayerFromEvent(svg, e);
      if (!layer) return;
      const slot = layer.getAttribute('data-sheet') || 's1';
      if (slot === 's2' && !imgState.includeSides) return;
      e.preventDefault();
      if (selImgTarget.value !== 'both') selImgTarget.value = slot;
      const pt = clientToLayerMm(layer, e.clientX, e.clientY);
      const rx = parseFloat(layer.getAttribute('data-rx'));
      const ry = parseFloat(layer.getAttribute('data-ry'));
      const rw = parseFloat(layer.getAttribute('data-rw'));
      const rh = parseFloat(layer.getAttribute('data-rh'));
      const refCx = rx + rw / 2;
      const refCy = ry + rh / 2;
      const factor = Math.exp(-e.deltaY * 0.0025);
      const keys = (selImgTarget.value === 'both') ? ['s1', 's2'] : [slot];
      keys.forEach(k => {
        const oldS = imgState.slots[k].scale;
        const newS = Math.max(0.15, Math.min(5.0, oldS * factor));
        const ratio = newS / oldS;
        if (k === slot) {
          imgState.slots[k].dx = (pt.x - refCx) * (1 - ratio) + imgState.slots[k].dx * ratio;
          imgState.slots[k].dy = (pt.y - refCy) * (1 - ratio) + imgState.slots[k].dy * ratio;
        }
        imgState.slots[k].scale = newS;
      });
      renderBgImages();
    }, { passive: false });
  });

  // Keyboard arrow-key precision nudge (0.5 mm, Shift = 5.0 mm) when an image is loaded
  window.addEventListener('keydown', e => {
    if (!imgState.dataUrl) return;
    const tag = (e.target && e.target.tagName) ? e.target.tagName.toLowerCase() : '';
    if (tag === 'input' || tag === 'select' || tag === 'textarea') return;
    const step = e.shiftKey ? 5.0 : 0.5;
    let nx = 0, ny = 0;
    if (e.key === 'ArrowLeft') nx = -step;
    else if (e.key === 'ArrowRight') nx = step;
    else if (e.key === 'ArrowUp') ny = -step;
    else if (e.key === 'ArrowDown') ny = step;
    else return;
    e.preventDefault();
    targetSlotKeys().forEach(k => {
      imgState.slots[k].dx += nx;
      imgState.slots[k].dy += ny;
    });
    renderBgImages();
  });

  // ---- Layout & Guide Toggles ----
  selLayout.addEventListener('change', () => {
    const mode = selLayout.value;
    const isTwoSheetStudio = (mode === 'cameo_studio' || mode === 'a4');
    document.getElementById('wrap-mie-studio').classList.toggle('hidden-layout', mode !== 'cameo_studio');
    document.getElementById('wrap-cameo').classList.toggle('hidden-layout', mode !== 'cameo');
    document.getElementById('wrap-a4').classList.toggle('hidden-layout', mode !== 'a4');
    document.getElementById('wrap-single').classList.toggle('hidden-layout', mode !== 'single');
    document.getElementById('banner-mie-studio').classList.toggle('hidden-layout', mode !== 'cameo_studio');
    document.getElementById('banner-cameo').classList.toggle('hidden-layout', mode !== 'cameo');
    document.getElementById('banner-studio').classList.toggle('hidden-layout', mode !== 'a4' && mode !== 'single');
    document.getElementById('lbl-align').style.display = (mode === 'cameo') ? 'none' : '';
    document.getElementById('btn-png1').style.display = isTwoSheetStudio ? '' : 'none';
    document.getElementById('btn-png2').style.display = isTwoSheetStudio ? '' : 'none';
    document.getElementById('btn-png-single').style.display = (mode === 'single') ? '' : 'none';
    document.getElementById('btn-print').style.display = (mode === 'cameo') ? '' : 'none';
    document.getElementById('btn-svg').textContent = (mode === 'cameo')
      ? 'Download Inkscape 3-Layer SVGs'
      : 'Download Print+Cut SVGs';
    if (mode === 'cameo_studio') {
      pageStyle.textContent = '@page { size: 279.4mm 215.9mm; margin: 0; }';
    } else if (mode === 'cameo') {
      pageStyle.textContent = '@page { size: 215.9mm 279.4mm; margin: 0; }';
    } else if (mode === 'a4') {
      pageStyle.textContent = '@page { size: 297mm 210mm; margin: 0; }';
    } else {
      pageStyle.textContent = '@page { size: 304.8mm 304.8mm; margin: 0; }';
    }
  });

  chkCut.addEventListener('change', () => {
    document.body.classList.toggle('hide-cut', !chkCut.checked);
  });
  chkAlign.addEventListener('change', () => {
    document.body.classList.toggle('hide-align', !chkAlign.checked);
  });
  chkHoles.addEventListener('change', () => {
    state.showHoleMask = chkHoles.checked;
    applyThemeStyles();
  });

  // ---- 300 DPI PNG pHYs Metadata Injector (so Silhouette Studio imports at exact 1:1 mm!) ----
  const CRC_TABLE = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
      t[n] = c >>> 0;
    }
    return t;
  })();

  function crc32Bytes(u8) {
    let c = 0xFFFFFFFF;
    for (let i = 0; i < u8.length; i++) c = CRC_TABLE[(c ^ u8[i]) & 0xFF] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
  }

  async function injectPngDpi300(pngBlob) {
    const buf = await pngBlob.arrayBuffer();
    const src = new Uint8Array(buf);
    if (src.length < 33 || src[0] !== 0x89 || src[1] !== 0x50) return pngBlob;
    const dv = new DataView(buf);
    const ihdrLen = dv.getUint32(8, false);
    const insertAt = 8 + 12 + ihdrLen;
    // Build 21-byte pHYs chunk: 11811 px/m (300.00 DPI), unit = 1 (meter)
    const phys = new Uint8Array(21);
    const pv = new DataView(phys.buffer);
    pv.setUint32(0, 9, false);
    phys[4] = 0x70; phys[5] = 0x48; phys[6] = 0x59; phys[7] = 0x73; // 'pHYs'
    pv.setUint32(8, 11811, false);
    pv.setUint32(12, 11811, false);
    phys[16] = 1;
    pv.setUint32(17, crc32Bytes(phys.subarray(4, 17)), false);

    const out = new Uint8Array(src.length + phys.length);
    out.set(src.subarray(0, insertAt), 0);
    out.set(phys, insertAt);
    out.set(src.subarray(insertAt), insertAt + phys.length);
    return new Blob([out], { type: 'image/png' });
  }

  function serializeSvg(svgId, hideCutForPrintPng) {
    const orig = document.getElementById(svgId);
    const clone = orig.cloneNode(true);
    const w = parseFloat(orig.getAttribute('data-w'));
    const h = parseFloat(orig.getAttribute('data-h'));
    const isInkscape = orig.getAttribute('data-inkscape') === '1';
    clone.removeAttribute('id');
    clone.removeAttribute('class');
    clone.removeAttribute('data-w');
    clone.removeAttribute('data-h');
    clone.removeAttribute('data-inkscape');
    clone.setAttribute('width', w.toFixed(2) + 'mm');
    clone.setAttribute('height', h.toFixed(2) + 'mm');
    clone.setAttribute('xmlns', SVG_NS);
    clone.setAttribute('xmlns:xlink', XLINK_NS);
    if (isInkscape) {
      clone.setAttribute('xmlns:inkscape', 'http://www.inkscape.org/namespaces/inkscape');
      clone.setAttribute('xmlns:sodipodi', 'http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd');
    }
    // Always strip on-screen cutout hole preview masks from exports
    clone.querySelectorAll('.cutout-preview-mask').forEach(el => el.remove());
    if (hideCutForPrintPng) {
      clone.querySelectorAll('.cut-line, .align-frame').forEach(el => el.remove());
    }
    return { xml: '<?xml version="1.0" encoding="UTF-8"?>\\n' + clone.outerHTML, w, h };
  }

  function exportPng300(svgId, outName) {
    const { xml, w, h } = serializeSvg(svgId, true);
    const pxW = Math.round(w / 25.4 * 300);
    const pxH = Math.round(h / 25.4 * 300);
    const blob = new Blob([xml], { type: 'image/svg+xml;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const img = new Image();
    img.onload = async function() {
      try { if (img.decode) await img.decode(); } catch (_) {}
      const canvas = document.createElement('canvas');
      canvas.width = pxW;
      canvas.height = pxH;
      const ctx = canvas.getContext('2d');
      if (!state.bgTransparent) {
        ctx.fillStyle = '#ffffff';
        ctx.fillRect(0, 0, pxW, pxH);
      }
      ctx.drawImage(img, 0, 0, pxW, pxH);
      URL.revokeObjectURL(url);
      canvas.toBlob(async rawBlob => {
        if (!rawBlob) return;
        const dpiBlob = await injectPngDpi300(rawBlob);
        const a = document.createElement('a');
        a.href = URL.createObjectURL(dpiBlob);
        a.download = outName;
        a.click();
        setTimeout(() => URL.revokeObjectURL(a.href), 2000);
      }, 'image/png');
    };
    img.src = url;
  }

  function downloadSvgFile(svgId, outName) {
    const { xml } = serializeSvg(svgId, false);
    const blob = new Blob([xml], { type: 'image/svg+xml;charset=utf-8' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = outName;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  }

  function exportSlug() {
    let s = state.theme;
    if (imgState.dataUrl) s += '_customimg';
    else if (state.bgCustom) s += '_' + (normHex(state.bgColor) || '#000000').slice(1).toLowerCase();
    return s;
  }

  document.getElementById('btn-png1').addEventListener('click', () => {
    if (selLayout.value === 'cameo_studio') {
      exportPng300('svg-mie-studio1', `smcpad_cameo_${exportSlug()}_letter_sheet1_300dpi.png`);
    } else {
      exportPng300('svg-sheet1', `smcpad_${exportSlug()}_sheet1_top_250x165mm_300dpi.png`);
    }
  });
  document.getElementById('btn-png2').addEventListener('click', () => {
    if (selLayout.value === 'cameo_studio') {
      exportPng300('svg-mie-studio2', `smcpad_cameo_${exportSlug()}_letter_sheet2_300dpi.png`);
    } else {
      exportPng300('svg-sheet2', `smcpad_${exportSlug()}_sheet2_sides_250x165mm_300dpi.png`);
    }
  });
  document.getElementById('btn-png-single').addEventListener('click', () => {
    exportPng300('svg-single', `smcpad_${exportSlug()}_single_256x284mm_300dpi.png`);
  });
  document.getElementById('btn-svg').addEventListener('click', () => {
    const mode = selLayout.value;
    const slug = exportSlug();
    if (mode === 'cameo_studio') {
      downloadSvgFile('svg-mie-studio1', `smcpad_cameo_${slug}_letter_sheet1_studio.svg`);
      setTimeout(() => downloadSvgFile('svg-mie-studio2', `smcpad_cameo_${slug}_letter_sheet2_studio.svg`), 250);
    } else if (mode === 'cameo') {
      downloadSvgFile('svg-mie-s1', `smcpad_cameo_${slug}_sheet1_letter.svg`);
      setTimeout(() => downloadSvgFile('svg-mie-s2', `smcpad_cameo_${slug}_sheet2_letter.svg`), 250);
    } else if (mode === 'single') {
      downloadSvgFile('svg-single', `smcpad_${slug}_single_12x12.svg`);
    } else {
      downloadSvgFile('svg-sheet1', `smcpad_${slug}_sheet1_top.svg`);
      setTimeout(() => downloadSvgFile('svg-sheet2', `smcpad_${slug}_sheet2_sides.svg`), 250);
    }
  });

  // Expose tiny test hook for automated verification
  window.__overlayStudio = {
    state, imgState, setSolidBgColor, applyThemeStyles, renderBgImages, serializeSvg, injectPngDpi300,
  };

  applyThemeStyles();
})();
</script>
</body>
</html>
"""


def build_studio_html(default_theme="light"):
    ms_s1 = build_cameo_studio_sheet1_svg(theme=default_theme, cut_only=False, standalone=False)
    ms_s2 = build_cameo_studio_sheet2_svg(theme=default_theme, cut_only=False, standalone=False)
    mie_s1 = build_cameo_letter_sheet1_svg(theme=default_theme, include_cut=True, standalone=False)
    mie_s2 = build_cameo_letter_sheet2_svg(theme=default_theme, include_cut=True, standalone=False)
    s1_svg = build_sheet1_svg(theme=default_theme, cut_only=False, standalone=False)
    s2_svg = build_sheet2_svg(theme=default_theme, cut_only=False, standalone=False)
    sg_svg = build_single_12x12_svg(theme=default_theme, cut_only=False, standalone=False)

    opts_html = "\n".join(
        f'<option value="{k}" {"selected" if k == default_theme else ""}>{esc(v["label"])}</option>'
        for k, v in THEMES.items()
    )
    out = STUDIO_HTML_TEMPLATE
    for tok, val in (
        ("__THEMES_JSON__", json.dumps(THEMES)),
        ("__DEFAULT_THEME__", json.dumps(default_theme)),
        ("__OPTS_HTML__", opts_html),
        ("__MS_S1__", ms_s1),
        ("__MS_S2__", ms_s2),
        ("__MIE_S1__", mie_s1),
        ("__MIE_S2__", mie_s2),
        ("__S1_SVG__", s1_svg),
        ("__S2_SVG__", s2_svg),
        ("__SG_SVG__", sg_svg),
    ):
        out = out.replace(tok, val)
    return out


def find_chrome():
    for candidate in (
        "/usr/bin/google-chrome",
        "/usr/bin/google-chrome-stable",
        "/usr/bin/chromium",
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    ):
        if os.path.exists(candidate) or shutil.which(candidate):
            return candidate
    return None


def render_pdf_from_html_str(html_str, pdf_path):
    import tempfile
    chrome = find_chrome()
    if not chrome:
        raise RuntimeError("Chrome/Chromium not found for --pdf rendering")
    with tempfile.NamedTemporaryFile("w", suffix=".html", encoding="utf-8", delete=False) as tf:
        tf.write(html_str)
        tmp_path = tf.name
    try:
        subprocess.run(
            [chrome, "--headless", "--disable-gpu", "--no-sandbox", "--no-pdf-header-footer",
             f"--print-to-pdf={pdf_path}", "file://" + tmp_path],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
    print(pdf_path)


def render_png300_from_svg_str(svg_str, png_path):
    """Render exact 300 DPI Print-Only PNG (with pHYs 300 DPI chunk) via Inkscape CLI if available."""
    import tempfile
    inkscape = shutil.which("inkscape")
    if not inkscape:
        return False
    with tempfile.NamedTemporaryFile("w", suffix=".svg", encoding="utf-8", delete=False) as tf:
        tf.write(svg_str)
        tmp_svg = tf.name
    try:
        subprocess.run(
            [inkscape, tmp_svg, "--export-type=png", "--export-dpi=300",
             "--export-background=#ffffff", "--export-background-opacity=1.0",
             f"--export-filename={png_path}"],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
    finally:
        if os.path.exists(tmp_svg):
            os.remove(tmp_svg)
    print(png_path)
    return True


def write_out(filename, content):
    p = os.path.join(OUT_DIR, filename)
    with open(p, "w", encoding="utf-8") as f:
        f.write(content)
    print(p)
    return p


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--theme", choices=tuple(THEMES.keys()), default="light",
                    help="default theme baked into standalone SVGs/PNGs/PDF (default: light)")
    ap.add_argument("--pdf", action="store_true", help="also render 1:1 vector PDFs via headless Chrome")
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    xml_hdr = '<?xml version="1.0" encoding="UTF-8"?>\n'

    write_out("smcpad_milspec_overlay.html", build_studio_html(args.theme))

    # Cameo US Letter (8.5 x 11 in Landscape, 240.0 x 154.0 mm frame) Silhouette Studio DXFs + SVGs + 300 DPI PNGs
    write_out("smcpad_cameo_sheet0_roll_to_letter.dxf", build_cameo_roll_to_letter_dxf())
    write_out("smcpad_cameo_letter_sheet1_top_cut.dxf", build_cameo_letter_sheet1_dxf())
    write_out("smcpad_cameo_letter_sheet2_sides_cut.dxf", build_cameo_letter_sheet2_dxf())
    write_out("smcpad_cameo_milspec_letter_sheet1_studio.svg", xml_hdr + build_cameo_studio_sheet1_svg(args.theme))
    write_out("smcpad_cameo_milspec_letter_sheet2_studio.svg", xml_hdr + build_cameo_studio_sheet2_svg(args.theme))

    s1_print = xml_hdr + build_cameo_studio_sheet1_svg(args.theme, cut_only=False, include_cut=False, standalone=True, include_align=False)
    s2_print = xml_hdr + build_cameo_studio_sheet2_svg(args.theme, cut_only=False, include_cut=False, standalone=True, include_align=False)
    render_png300_from_svg_str(s1_print, os.path.join(OUT_DIR, "smcpad_cameo_milspec_letter_sheet1_300dpi.png"))
    render_png300_from_svg_str(s2_print, os.path.join(OUT_DIR, "smcpad_cameo_milspec_letter_sheet2_300dpi.png"))

    # Cameo US Letter (215.9 x 279.4 mm) 3-Layer Inkscape `inkscape-silhouette` SVGs
    write_out("smcpad_cameo_sheet0_roll_to_letter.svg", build_cameo_roll_to_letter_svg())
    write_out("smcpad_cameo_milspec_sheet1_letter.svg", xml_hdr + build_cameo_letter_sheet1_svg(args.theme, include_cut=True, standalone=True))
    write_out("smcpad_cameo_milspec_sheet2_letter.svg", xml_hdr + build_cameo_letter_sheet2_svg(args.theme, include_cut=True, standalone=True))

    # Step-0 Roll -> A4 Sheet Cutter (alternate Silhouette Studio workflow)
    write_out("smcpad_milspec_sheet0_roll_to_a4.svg", build_roll_to_a4_svg())
    write_out("smcpad_milspec_sheet0_roll_to_a4.dxf", build_roll_to_a4_dxf())

    # A4 Landscape Sheet 1: Top Panel (250 x 165 mm frame)
    write_out("smcpad_milspec_sheet1_top.svg", xml_hdr + build_sheet1_svg(args.theme, cut_only=False, standalone=True))
    write_out("smcpad_milspec_sheet1_top_cut.svg", xml_hdr + build_sheet1_svg(args.theme, cut_only=True, standalone=True))
    write_out("smcpad_milspec_sheet1_top_cut.dxf", build_sheet1_dxf())

    # A4 Landscape Sheet 2: 4 Pinwheel Side Bands + 2x Rear I/O Inserts (250 x 165 mm frame)
    write_out("smcpad_milspec_sheet2_sides.svg", xml_hdr + build_sheet2_svg(args.theme, cut_only=False, standalone=True))
    write_out("smcpad_milspec_sheet2_sides_cut.svg", xml_hdr + build_sheet2_svg(args.theme, cut_only=True, standalone=True))
    write_out("smcpad_milspec_sheet2_sides_cut.dxf", build_sheet2_dxf())

    # Single-sheet 12x12" / A3 nested full kit (256 x 284 mm frame)
    write_out("smcpad_milspec_single_12x12.svg", xml_hdr + build_single_12x12_svg(args.theme, cut_only=False, standalone=True))
    write_out("smcpad_milspec_single_12x12_cut.svg", xml_hdr + build_single_12x12_svg(args.theme, cut_only=True, standalone=True))
    write_out("smcpad_milspec_single_12x12_cut.dxf", build_single_dxf())

    if args.pdf:
        render_pdf_from_html_str(build_cameo_print_html(args.theme), os.path.join(OUT_DIR, "smcpad_cameo_milspec_print_letter.pdf"))
        render_pdf_from_html_str(build_a4_print_html(args.theme), os.path.join(OUT_DIR, "smcpad_milspec_overlay.pdf"))


if __name__ == "__main__":
    main()
