#!/usr/bin/env python3
"""Drumbox: writes params.json (what MPC sees), src/params_def.h (the engine's table, same order), layout.conf (the
screen) and src/builtin_kits.h (the built-in kits, from kits/).

    python3 mpc/gen.py            (paths are relative to this file)

The parameter order is the VST index MPC saves projects and Q-Links by: append new parameters at the end.
The defaults are nb_tonic's default kit (kits/nb_tonic/default.tkit), turned into knob values with the same curves as
src/voice.c, so a fresh Drumbox is the Init kit.
"""
import json
import math
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
NV = 8
# the eight lanes' colours (sequencer rows, voice pages): kick red, then round the colour wheel
LANES = ["ff5a4e", "ff9f2e", "f2c934", "7ed957", "2ec4b6", "3fa7ff", "9b7bff", "ff6fb5"]
BG = "14161c"


def mix(a, b, t):   # colour a over b, t of a
    ca, cb = [int(a[i:i + 2], 16) for i in (0, 2, 4)], [int(b[i:i + 2], 16) for i in (0, 2, 4)]
    return "%02x%02x%02x" % tuple(round(x * t + y * (1 - t)) for x, y in zip(ca, cb))


def svg_box(w, h, fill, stroke, sw, rx, extra=""):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'
            '<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="#%s" stroke="#%s" stroke-width="%g"/>%s</svg>\n'
            % (w, h, w, h, sw / 2, sw / 2, w - sw, h - sw, rx, fill, stroke, sw, extra))


def lane_art():
    """images/lane<v>_*.svg: the sequencer's step tiles and mute tile (dark tint off, the colour lit) and the voice
    pages' panels (a tinted panel, a border and a bar in the colour)."""
    out = os.path.join(ROOT, "images")
    os.makedirs(out, exist_ok=True)
    files = {}
    for v, c in enumerate(LANES, 1):
        files["lane%d_step.svg" % v] = svg_box(66, 44, mix(c, BG, 0.16), mix(c, BG, 0.45), 1.5, 5)
        files["lane%d_step_on.svg" % v] = svg_box(66, 44, mix(c, "000000", 0.82), mix(c, "ffffff", 0.7), 2, 5)
        files["lane%d_mute.svg" % v] = svg_box(112, 44, mix(c, BG, 0.10), mix(c, BG, 0.35), 1.5, 5)
        files["lane%d_mute_on.svg" % v] = svg_box(112, 44, mix(c, "000000", 0.82), mix(c, "ffffff", 0.7), 2, 5)
        for w, h in ((470, 604), (380, 604), (394, 604)):
            bar = '<rect x="10" y="3" width="%d" height="5" rx="2.5" fill="#%s"/>' % (w - 20, c)
            files["lane%d_panel_%dx%d.svg" % (v, w, h)] = svg_box(w, h, mix(c, BG, 0.07), mix(c, BG, 0.75), 2, 10, bar)
    for name, text in files.items():
        path = os.path.join(out, name)
        if not os.path.exists(path) or open(path).read() != text:
            open(path, "w").write(text)
NOTE_NAMES = ["%s%d" % (("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")[n % 12], n // 12 - 2)
              for n in range(128)]   # MPC's names: 60 = C3, 36 = C1

BASE_NOTES = ["C0", "C1", "G#1", "C2", "C3"]   # src/drumbox.c BASE_NOTE: 24, 36, 44, 48, 60

# ---- the knob curves (src/voice.c) ----------------------------------------------------------------------------
def clamp(x, lo=0.0, hi=100.0): return max(lo, min(hi, x))
def i_freq(hz): return clamp(math.log(max(hz, 20.0) / 20.0) / math.log(1000.0) * 100)
def i_time(ms): return clamp((max(ms, 0.0) / 10000.0) ** (1 / 3) * 100)
def i_modamt(st):
    x = math.sqrt(min(abs(st), 96.0) / 96.0)
    return (-x if st < 0 else x) * 50 + 50
def i_modrate(hz): return 0.0 if hz <= 0.1 else clamp(math.log(hz / 0.1) / math.log(200000.0) * 100, 0.01, 100)
def i_q(q): return clamp(math.log(max(q, 0.1) / 0.1) / math.log(100000.0) * 100)
def i_db(db): return clamp((db + 60.0) / 0.72)
def i_eqgain(db): return clamp((db + 40.0) / 0.8)

# a voice's parameters, in order (src/drumbox.h VP_*): key, screen name, kind, extra
#   kind "k": knob 0..100 with an engine curve (its text from the engine); "n": plain number (min, max); "o": options
VOICE = [
    ("osc_wave", "OSC WAVE", "o", ["SINE", "TRI", "SAW"]),
    ("osc_freq", "OSC FREQ", "k", None),
    ("osc_atk", "OSC ATTACK", "k", None),
    ("osc_dcy", "OSC DECAY", "k", None),
    ("mod_mode", "MOD MODE", "o", ["DECAY", "SINE", "NOISE"]),
    ("mod_amt", "MOD AMOUNT", "k", None),
    ("mod_rate", "MOD RATE", "k", None),
    ("n_filter", "NOISE FILTER", "o", ["LP", "BP", "HP"]),
    ("n_freq", "NOISE FREQ", "k", None),
    ("n_q", "NOISE Q", "k", None),
    ("n_env", "NOISE ENV", "o", ["EXP", "LIN", "CLAP"]),
    ("n_atk", "NOISE ATTACK", "k", None),
    ("n_dcy", "NOISE DECAY", "k", None),
    ("n_stereo", "NOISE STEREO", "o", ["OFF", "ON"]),
    ("mix", "MIX", "k", None),
    ("dist", "DIST", "n", (0, 100)),
    ("eq_freq", "EQ FREQ", "k", None),
    ("eq_gain", "EQ GAIN", "k", None),
    ("level", "LEVEL", "k", None),
    ("pan", "PAN", "k", None),
    ("osc_vel", "OSC VEL", "n", (0, 200)),
    ("n_vel", "NOISE VEL", "n", (0, 200)),
    ("mod_vel", "MOD VEL", "n", (0, 200)),
    ("choke", "CHOKE", "o", ["OFF", "1", "2", "3"]),
]
RANGES = {"pan": (-100, 100), "mix": (0, 100)}   # "k" knobs with their own range (text from the engine)


def read_tkit(path):
    """nb_tonic kit -> 8 dicts of knob values (the same conversion as src/kits.c parse_tkit)."""
    text = open(path).read()
    tables = re.split(r"-- Table: \{(\d+)\}", text)
    voices = [None] * NV
    groups = [100 + v for v in range(NV)]
    for i in range(1, len(tables), 2):
        v = int(tables[i]) - 2
        if not 0 <= v < NV:
            continue
        d = {k: float(x) for k, x in re.findall(r'\["(\w+)"\]=([-0-9.e]+)', tables[i + 1])}
        p = {
            "osc_wave": d["osc_wav"] - 1, "osc_freq": i_freq(d["osc_freq"]),
            "osc_atk": i_time(d["osc_attack"] * 1000), "osc_dcy": i_time(d["osc_decay"] * 1000),
            "mod_mode": d["mod_mode"] - 1, "mod_amt": i_modamt(d["mod_amt"]), "mod_rate": i_modrate(d["mod_rate"]),
            "n_filter": d["noise_mode"] - 1, "n_freq": i_freq(d["noise_freq"]), "n_q": i_q(d["noise_q"]),
            "n_env": d["noise_env"] - 1, "n_atk": i_time(d["noise_attack"] * 1000),
            "n_dcy": i_time(d["noise_decay"] * 1000), "n_stereo": d["noise_stereo"] - 1,
            "mix": clamp((1 - d["mix"]) * 50), "dist": clamp(d["dist"] * 100), "eq_freq": i_freq(d["eq_freq"]),
            "eq_gain": i_eqgain(d["eq_gain"] * 2),
            "level": 0.0 if d["level"] <= 1e-6 else i_db(20 * math.log10(d["level"])),
            "pan": clamp(d["pan"] * 100, -100, 100), "osc_vel": d["osc_vel"] * 100,
            "n_vel": d["noise_vel"] * 100, "mod_vel": d["mod_vel"] * 100, "choke": 0,
        }
        groups[v] = int(d.get("choke_group", 100 + v))
        voices[v] = p
    nxt = 1
    for v in range(NV):
        if sum(g == groups[v] for g in groups) > 1:
            prev = [voices[u]["choke"] for u in range(v) if groups[u] == groups[v]]
            if prev:
                voices[v]["choke"] = prev[0]
            elif nxt <= 3:
                voices[v]["choke"] = nxt
                nxt += 1
    return voices


INIT = read_tkit(os.path.join(ROOT, "kits", "nb_tonic", "default.tkit"))


def params():
    P = []
    # a long list: browsed with the arrows, the PRESET menu or the pads (PAD BROWSE), not a Q-Link (with QLINK_TRAVEL a
    # Q-Link event is 1/128 of the range, 16 kits)
    P.append(dict(key="kit", name="KIT", type="stepper", min=0, max=2047, step=1, default=0, display="int", clamped=True))
    P.append(dict(key="kit_prev", name="PREV KIT", momentary=True, step_of="kit", step_delta=-1))
    P.append(dict(key="kit_next", name="NEXT KIT", momentary=True, step_of="kit", step_delta=1))
    P.append(dict(key="kit_name", name="KIT NAME", min=0, max=0, display="string", type="readout"))
    P.append(dict(key="volume", name="VOLUME", min=0, max=100, default=round(i_db(0.0), 3), dynamic_display=True))
    P.append(dict(key="lpf_cut", name="LPF CUTOFF", min=0, max=100, default=100, dynamic_display=True))
    P.append(dict(key="lpf_res", name="LPF RESO", min=0, max=100, default=0))
    # the note of V1 (V2-V8 the next ones): a Force's / MPC's drum pads start at C1 (36); G#1 = pads 9-16
    P.append(dict(key="base_note", name="BASE NOTE", options=BASE_NOTES, default="C1"))
    P.append(dict(key="slot", name="SLOT", type="stepper", min=1, max=32, step=1, default=1, display="int"))
    P.append(dict(key="slot_prev", name="PREV SLOT", momentary=True, step_of="slot", step_delta=-1))
    P.append(dict(key="slot_next", name="NEXT SLOT", momentary=True, step_of="slot", step_delta=1))
    P.append(dict(key="slot_name", name="SLOT NAME", min=0, max=0, display="string", type="readout"))
    P.append(dict(key="save", name="SAVE", momentary=True))
    for v in range(1, NV + 1):
        init = INIT[v - 1]
        for key, name, kind, extra in VOICE:
            d = dict(key="v%d_%s" % (v, key), name="V%d %s" % (v, name))
            if kind == "o":
                d.update(options=extra, default=extra[int(init[key])])
            elif kind == "n":
                d.update(min=extra[0], max=extra[1], default=round(init[key], 3), display="int")
            else:
                lo, hi = RANGES.get(key, (0, 100))
                d.update(min=lo, max=hi, default=round(init[key], 3), dynamic_display=True)
            P.append(d)
    # 0.2.0: the preset browser (PREVIEW: pattern A when a kit is picked; FAVORIT) and the SOUNDS page (.mtdrum)
    P.append(dict(key="preview", name="PREVIEW", options=["OFF", "ON"], default="ON"))
    P.append(dict(key="preview_play", name="PLAY PREVIEW", momentary=True))
    P.append(dict(key="fav", name="FAVORIT", momentary=True))
    P.append(dict(key="info", name="INFO", min=0, max=0, display="string", type="readout"))
    P.append(dict(key="drum", name="SOUND", type="stepper", min=0, max=4095, step=1, default=0, display="int",
                  clamped=True))
    P.append(dict(key="drum_prev", name="PREV SOUND", momentary=True, step_of="drum", step_delta=-1))
    P.append(dict(key="drum_next", name="NEXT SOUND", momentary=True, step_of="drum", step_delta=1))
    P.append(dict(key="drum_name", name="SOUND NAME", min=0, max=0, display="string", type="readout"))
    P.append(dict(key="drum_voice", name="TO VOICE", options=["V%d" % v for v in range(1, NV + 1)], default="V1"))
    P.append(dict(key="drum_take", name="TAKE", momentary=True))
    P.append(dict(key="drum_play", name="PLAY SOUND", momentary=True))
    # pads 9-16 (BASE NOTE + 8 .. + 15) browse while the Force is stopped: kit -/+, FAVORIT, PLAY, sound -/+, TAKE,
    # next pattern (pad 16 also while it plays)
    P.append(dict(key="pad_browse", name="PAD BROWSE", options=["OFF", "ON"], default="OFF"))
    # 0.3.0: the sequencer (patterns A-L, played with the Force's song position) and a note per voice
    P.append(dict(key="seq", name="SEQ", options=["OFF", "ON"], default="OFF"))
    P.append(dict(key="pattern", name="PATTERN", options=[chr(65 + i) for i in range(12)], default="A"))
    P.append(dict(key="hold", name="HOLD", options=["OFF", "KIT", "PATTERN"], default="OFF"))
    P.append(dict(key="pat_len", name="LENGTH", min=1, max=16, default=16, display="int"))
    P.append(dict(key="chain", name="CHAIN", options=["OFF", "ON"], default="ON"))
    P.append(dict(key="swing", name="SWING", min=0, max=100, default=0, dynamic_display=True))
    for i in range(1, NV * 16 + 1):   # the grid: voice (row) x step (column); a tap: off -> on -> accent -> off
        P.append(dict(key="st%d_%d" % ((i - 1) // 16 + 1, (i - 1) % 16 + 1), name="V%d STEP %d" % ((i - 1) // 16 + 1, (i - 1) % 16 + 1), min=0, max=1,
                      display="string", type="slot"))
    for v in range(1, NV + 1):        # lit = plays; a tap mutes / unmutes
        P.append(dict(key="mute%d_1" % v, name="V%d MUTE" % v, min=0, max=1, display="string", type="slot"))
    for i in range(1, 17):            # the step playing
        P.append(dict(key="ph_%d" % i, name="STEP %d LIGHT" % i, min=0, max=1, display="string", type="slot"))
    for v in range(1, NV + 1):        # the MIDI note that plays the voice (V1 NOTE on MASTER sets all eight)
        P.append(dict(key="v%d_note" % v, name="V%d NOTE" % v, options=NOTE_NAMES, default=NOTE_NAMES[35 + v]))
        P.append(dict(key="v%d_note_prev" % v, name="V%d NOTE -" % v, momentary=True, step_of="v%d_note" % v, step_delta=-1))
        P.append(dict(key="v%d_note_next" % v, name="V%d NOTE +" % v, momentary=True, step_of="v%d_note" % v, step_delta=1))
    P.append(dict(key="pat_len_prev", name="LENGTH -", momentary=True, step_of="pat_len", step_delta=-1))
    P.append(dict(key="pat_len_next", name="LENGTH +", momentary=True, step_of="pat_len", step_delta=1))
    P.append(dict(key="swing_prev", name="SWING -", momentary=True, step_of="swing", step_delta=-5))
    P.append(dict(key="swing_next", name="SWING +", momentary=True, step_of="swing", step_delta=5))
    # 0.4.0: EXPLORE (similar sounds and grooves, MUTATE, UNDO)
    for i in range(1, 7):
        P.append(dict(key="ss_%d" % i, name="SIMILAR SOUND %d" % i, min=0, max=1, display="string", type="slot"))
    for i in range(1, 7):
        P.append(dict(key="sg_%d" % i, name="SIMILAR GROOVE %d" % i, min=0, max=1, display="string", type="slot"))
    P.append(dict(key="search", name="SEARCH HERE", momentary=True))
    P.append(dict(key="mut_target", name="MUTATE VOICES", options=["ALL"] + ["V%d" % v for v in range(1, NV + 1)],
                  default="ALL"))
    P.append(dict(key="mut_amt", name="AMOUNT", min=5, max=100, default=25, display="int"))
    P.append(dict(key="mut_amt_prev", name="AMOUNT -", momentary=True, step_of="mut_amt", step_delta=-5))
    P.append(dict(key="mut_amt_next", name="AMOUNT +", momentary=True, step_of="mut_amt", step_delta=5))
    P.append(dict(key="mut_sound", name="MUTATE SOUND", momentary=True))
    P.append(dict(key="mut_groove", name="MUTATE GROOVE", momentary=True))
    P.append(dict(key="undo", name="UNDO", momentary=True))
    P.append(dict(key="ex_info", name="EXPLORE INFO", min=0, max=0, display="string", type="readout"))
    return P


def params_def(P):
    out = ["/* generated by mpc/gen.py: do not edit (change gen.py and run it) */", "#pragma once",
           "/* kind: 0 = number (min..max), 1 = option list (value = index), 2 = kit / slot number, 3 = not stored */",
           "typedef struct { const char *key; int kind; int nopts; float def, min, max; } pdef_t;",
           "static const pdef_t PDEF[] = {"]
    for p in P:
        if "options" in p:
            kind, n, df, lo, hi = 1, len(p["options"]), p["options"].index(p["default"]), 0, len(p["options"]) - 1
        elif p["key"] in ("kit", "slot", "drum"):
            kind, n, df, lo, hi = 2, 0, p["default"], p["min"], p["max"]
        elif p.get("momentary") or p.get("display") == "string":
            kind, n, df, lo, hi = 3, 0, 0, 0, 0
        else:
            kind, n, df, lo, hi = 0, 0, p["default"], p["min"], p["max"]
        out.append('    {"%s", %d, %d, %.4ff, %.1ff, %.1ff},' % (p["key"], kind, n, df, lo, hi))
    out.append("};")
    out.append("enum {")
    for i, p in enumerate(P):
        out.append("    P_%s = %d," % (p["key"].upper(), i))
    out.append("    NP = %d" % len(P))
    out.append("};")
    out.append("#define VSTRIDE %d   /* v<n>_osc_wave is P_V1_OSC_WAVE + VSTRIDE * (n - 1) */" % len(VOICE))
    return "\n".join(out) + "\n"


# ---- the screen ------------------------------------------------------------------------------------------------
THEME = """# Drumbox, generated by mpc/gen.py (do not edit by hand: change gen.py and run it again).
theme_bg=101114
theme_panel=181a1f
theme_line=33363f
theme_ink=eef0f4
theme_ink_dim=a3a8b4
theme_ink_faint=666b78
theme_accent=ff8a3d
theme_accent_hi=ffc08f
theme_box=0c0d10
theme_lcd=08090b
theme_seg_active=ff8a3d
theme_seg_active_tx=101114
theme_seg_inactive=262931
theme_btn_bg=2a2d35
theme_btn_text=eef0f4
theme_knob_face=2b2e36
theme_knob_ring=41454f
theme_knob_dot=ff8a3d
theme_display_bezel=050506
theme_display_bg=0b0c0e
theme_display_cell=1d1309
theme_display_ink=ffb066
theme_display_off=2a1a0c
art_css=drumbox.css
label_scale=1
focus_ring=1
"""


BTN_L = "img=images/btn_220x70.png img_on=images/btn_220x70_on.png"   # big touch buttons (mpc/art.py)
BTN_S = "img=images/btn_180x64.png img_on=images/btn_180x64_on.png"
BTN_M = "img=images/btn_150x56.png img_on=images/btn_150x56_on.png"
BTN_U = "img=images/btn_120x56.png img_on=images/btn_120x56_on.png"


def layout():
    L = [THEME]
    # MASTER
    L.append("[tab MASTER]")
    L.append('frame x=16 y=98 w=1248 h=236 title="KIT"')
    L.append("stepper cx=640 cy=172 w=1000 h=74 label=\"\" key=kit get=kit_name style=dotmatrix")
    L.append('enum_h cx=190 cy=280 label="PREVIEW" key=preview sw=80 sh=48')
    L.append('button cx=400 cy=280 w=180 h=64 label="PLAY" key=preview_play %s' % BTN_S)
    L.append('button cx=600 cy=280 w=180 h=64 label="FAVORIT" key=fav %s' % BTN_S)
    L.append('readout cx=970 cy=280 w=500 h=50 key=info')
    L.append('frame x=16 y=346 w=480 h=356 title="MASTER"')
    L.append('knob cx=110 cy=500 r=44 label="VOLUME" key=volume')
    L.append('knob cx=256 cy=500 r=44 label="LPF CUTOFF" key=lpf_cut')
    L.append('knob cx=402 cy=500 r=44 label="LPF RESO" key=lpf_res')
    L.append('frame x=510 y=346 w=290 h=356 title="PADS"')
    L.append('enum_v cx=655 cy=492 label="V1 NOTE" key=base_note sw=150 sh=40')
    L.append('enum_h cx=655 cy=652 label="PADS 9-16: BROWSE" key=pad_browse sw=80 sh=44')
    L.append('frame x=814 y=346 w=450 h=356 title="USER SLOTS"')
    L.append("stepper cx=1039 cy=440 w=410 h=70 label=\"\" key=slot get=slot_name style=dotmatrix")
    L.append('button cx=1039 cy=560 w=220 h=70 label="SAVE" key=save %s' % BTN_L)
    L.append('text cx=1039 cy=660 label="SAVE: THE VOICES AS THEY ARE" size=1.5')
    L.append('qlinks "MASTER" = volume,lpf_cut,lpf_res,-,slot')
    # PATTERN: the sequencer, all eight voices at a glance
    L.append("[tab PATTERN]")
    L.append('enum_h cx=96 cy=146 label="SEQ" key=seq sw=70 sh=46')
    L.append('enum_h cx=356 cy=146 label="HOLD" key=hold sw=108 sh=46')
    L.append("stepper cx=630 cy=146 w=200 h=46 label=\"LENGTH\" key=pat_len")
    L.append('enum_h cx=826 cy=146 label="CHAIN" key=chain sw=70 sh=46')
    L.append('stepper cx=1110 cy=146 w=240 h=46 label="SWING" key=swing')
    L.append("list x=140 y=204 w=1124 h=18 key=ph cols=16 rows=1 th=18 gap=4")
    for v in range(1, NV + 1):   # a lane per voice, in its colour (lane_art)
        y = 228 + 48 * (v - 1)
        L.append("list x=16 y=%d w=112 h=44 key=mute%d cols=1 rows=1 th=44 gap=4 tsize=20 talign=center tweight=bold "
                 "tcolor=ffffff img=images/lane%d_mute.svg img_on=images/lane%d_mute_on.svg" % (y, v, v, v))
        L.append("list x=140 y=%d w=1124 h=44 key=st%d cols=16 rows=1 th=44 gap=4 tsize=20 talign=center tweight=bold "
                 "tcolor=ffffff img=images/lane%d_step.svg img_on=images/lane%d_step_on.svg" % (y, v, v, v))
    L.append('enum_h cx=496 cy=668 label="" key=pattern sw=76 sh=56')
    L.append('button cx=1060 cy=668 w=150 h=56 label="MUTATE" key=mut_groove %s' % BTN_M)
    L.append('button cx=1196 cy=668 w=120 h=56 label="UNDO" key=undo %s' % BTN_U)
    L.append('qlinks "PATTERN" = swing,pat_len')
    # EXPLORE: presets with a similar sound / groove (a tap takes it), MUTATE, UNDO
    L.append("[tab EXPLORE]")
    L.append('frame x=16 y=98 w=616 h=420 title="SIMILAR SOUND (TAP: TAKE THE SOUNDS)"')
    L.append("list x=32 y=146 w=584 h=356 key=ss cols=1 rows=6 th=54 gap=6 tsize=20")
    L.append('frame x=648 y=98 w=616 h=420 title="SIMILAR GROOVE (TAP: TAKE THE PATTERNS)"')
    L.append("list x=664 y=146 w=584 h=356 key=sg cols=1 rows=6 th=54 gap=6 tsize=20")
    L.append('button cx=136 cy=578 w=220 h=70 label="SEARCH HERE" key=search %s' % BTN_L)
    L.append('enum_h cx=596 cy=578 label="MUTATE VOICES" key=mut_target sw=60 sh=48')
    L.append('stepper cx=1110 cy=578 w=240 h=46 label="AMOUNT" key=mut_amt')
    L.append('button cx=136 cy=666 w=220 h=70 label="UNDO" key=undo %s' % BTN_L)
    L.append('button cx=380 cy=666 w=220 h=70 label="MUTATE SOUND" key=mut_sound %s' % BTN_L)
    L.append('button cx=624 cy=666 w=220 h=70 label="MUTATE GROOVE" key=mut_groove %s' % BTN_L)
    L.append("readout cx=1010 cy=666 w=480 h=50 key=ex_info")
    L.append('qlinks "EXPLORE" = mut_amt')
    # NOTES: a MIDI note per voice, for the Force's own sequencer (or pads laid out differently)
    L.append("[tab NOTES]")
    L.append('frame x=16 y=98 w=1248 h=604 title="THE NOTE THAT PLAYS EACH VOICE"')
    for v in range(1, NV + 1):
        col, row = (v - 1) // 4, (v - 1) % 4
        L.append("stepper cx=%d cy=%d w=460 h=74 label=\"V%d NOTE\" key=v%d_note style=dotmatrix"
                 % (340 + 600 * col, 210 + 128 * row, v, v))
    # SOUNDS: .mtdrum files, one voice at a time
    L.append("[tab SOUNDS]")
    L.append('frame x=16 y=98 w=1248 h=604 title="MICROTONIC DRUM SOUNDS (.MTDRUM)"')
    L.append("stepper cx=640 cy=200 w=1100 h=80 label=\"\" key=drum get=drum_name style=dotmatrix")
    L.append('text cx=640 cy=275 label="EACH STEP PLAYS THE SOUND ON ITS OWN" size=1.5')
    L.append('enum_h cx=640 cy=410 label="TO VOICE" key=drum_voice sw=120 sh=48')
    L.append('button cx=480 cy=530 w=220 h=70 label="PLAY" key=drum_play %s' % BTN_L)
    L.append('button cx=800 cy=530 w=220 h=70 label="TAKE" key=drum_take %s' % BTN_L)
    L.append('readout cx=640 cy=630 w=900 h=50 key=info')
    # the voices: OSC | NOISE | OUT, two rows of knobs each, the switches above them
    for v in range(1, NV + 1):
        k = lambda s: "v%d_%s" % (v, s)
        L.append("[tab V%d]" % v)
        ink = LANES[v - 1]
        L.append('frame x=8 y=98 w=470 h=604 title="V%d  OSCILLATOR" img=images/lane%d_panel_470x604.svg' % (v, v))
        L.append('enum_h cx=243 cy=192 label="WAVE" key=%s sw=120 sh=44' % k("osc_wave"))
        for i, (s, n) in enumerate((("osc_freq", "FREQ"), ("osc_atk", "ATTACK"), ("osc_dcy", "DECAY"), ("osc_vel", "VEL"))):
            L.append('knob cx=%d cy=322 r=40 label="%s" key=%s bw=108 ink=%s' % (69 + 116 * i, n, k(s), ink))
        L.append('enum_h cx=243 cy=460 label="PITCH MOD" key=%s sw=120 sh=44' % k("mod_mode"))
        for i, (s, n) in enumerate((("mod_amt", "AMOUNT"), ("mod_rate", "RATE"), ("mod_vel", "VEL"))):
            L.append('knob cx=%d cy=600 r=40 label="%s" key=%s bw=108 ink=%s' % (127 + 116 * i, n, k(s), ink))
        L.append('frame x=488 y=98 w=380 h=604 title="NOISE" img=images/lane%d_panel_380x604.svg' % v)
        L.append('enum_h cx=618 cy=192 label="FILTER" key=%s sw=80 sh=44' % k("n_filter"))
        L.append('enum_h cx=800 cy=192 label="STEREO" key=%s sw=56 sh=44' % k("n_stereo"))
        for i, (s, n) in enumerate((("n_freq", "FREQ"), ("n_q", "Q"), ("n_vel", "VEL"))):
            L.append('knob cx=%d cy=322 r=40 label="%s" key=%s bw=108 ink=%s' % (558 + 120 * i, n, k(s), ink))
        L.append('enum_h cx=678 cy=460 label="ENVELOPE" key=%s sw=100 sh=44' % k("n_env"))
        for i, (s, n) in enumerate((("n_atk", "ATTACK"), ("n_dcy", "DECAY"))):
            L.append('knob cx=%d cy=600 r=40 label="%s" key=%s bw=108 ink=%s' % (618 + 120 * i, n, k(s), ink))
        L.append('frame x=878 y=98 w=394 h=604 title="OUT" img=images/lane%d_panel_394x604.svg' % v)
        L.append('enum_h cx=1075 cy=192 label="CHOKE" key=%s sw=80 sh=44' % k("choke"))
        for i, (s, n) in enumerate((("mix", "MIX"), ("dist", "DIST"), ("level", "LEVEL"))):
            L.append('knob cx=%d cy=322 r=40 label="%s" key=%s bw=108 ink=%s' % (950 + 125 * i, n, k(s), ink))
        for i, (s, n) in enumerate((("eq_freq", "EQ FREQ"), ("eq_gain", "EQ GAIN"), ("pan", "PAN"))):
            L.append('knob cx=%d cy=520 r=40 label="%s" key=%s bw=108 ink=%s' % (950 + 125 * i, n, k(s), ink))
        L.append('qlinks "V%d OSC" = %s' % (v, ",".join(k(s) for s in (
            "osc_freq", "osc_atk", "osc_dcy", "osc_vel", "mod_amt", "mod_rate", "mod_vel"))))
        L.append('qlinks "V%d NOISE" = %s' % (v, ",".join(k(s) for s in ("n_freq", "n_q", "n_vel", "n_atk", "n_dcy"))))
        L.append('qlinks "V%d OUT" = %s' % (v, ",".join(k(s) for s in (
            "mix", "dist", "level", "eq_freq", "eq_gain", "pan"))))
    return "\n".join(L) + "\n"


# ---- built-in kits ---------------------------------------------------------------------------------------------
BUILTIN = [   # (name in the list, file); the first is the Init kit (the parameters' defaults)
    ("Init", "nb_tonic/default.tkit"),
    ("nb ARCHER", "nb_tonic/ARCHER.tkit"),
    ("nb DISTEC", "nb_tonic/DISTEC.tkit"),
    ("nb KROUMN", "nb_tonic/KROUMN.tkit"),
    ("nb LODEKA", "nb_tonic/LODEKA.tkit"),
    ("PA Unmouth Sphing", "patternarium/Unmouth_Sphing_139.mtpreset"),
    ("PA Isober Bewee", "patternarium/Isober_Bewee_114.mtpreset"),
    ("PA Apolid Pseudosteo", "patternarium/Apolid_Pseudosteoarspher_125.mtpreset"),
    ("PA Perlike Epithel", "patternarium/Perlike_Epithel_120.mtpreset"),
]


def c_string(text):
    out = []
    for line in text.splitlines(True):
        out.append('    "' + line.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t").replace("\n", "\\n") + '"')
    return "\n".join(out)


def builtin_kits():
    out = ["/* generated by mpc/gen.py from kits/: do not edit */", "#pragma once",
           "typedef struct { const char *name; int tkit; const char *text; } builtin_kit_t;",
           "static const builtin_kit_t BUILTIN_KITS[] = {"]
    for name, rel in BUILTIN:
        text = open(os.path.join(ROOT, "kits", rel)).read()
        out.append("    {%s, %d,\n%s},"
                   % (json.dumps(name), int(rel.endswith(".tkit")), c_string(text)))
    out.append("};")
    out.append("#define NBUILTIN_KITS ((int)(sizeof BUILTIN_KITS / sizeof BUILTIN_KITS[0]))")
    return "\n".join(out) + "\n"


def main():
    P = params()
    json.dump({"name": "Drumbox", "params": P}, open(os.path.join(ROOT, "params.json"), "w"), indent=1)
    open(os.path.join(ROOT, "src", "params_def.h"), "w").write(params_def(P))
    open(os.path.join(ROOT, "layout.conf"), "w").write(layout())
    open(os.path.join(ROOT, "src", "builtin_kits.h"), "w").write(builtin_kits())
    lane_art()
    print("%d parameters, %d built-in kits" % (len(P), len(BUILTIN)))


if __name__ == "__main__":
    main()
