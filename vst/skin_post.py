"""Post-build pass over the generated NAM skin:
- replaces the stock knob filmstrips with the synthwave knob (270-degree track, lit magenta value arc
  with a soft glow, dark face, cyan pointer), with fewer frames where 128 would make a strip taller
  than MPC draws correctly (16384 px), and numFrames in TUI.json to match;
- hides the Q-Link zone outline on every page (see hide_qlink_bounds());
- enlarges the value text of readouts, steppers and list rows (see scale_box_text());
- redraws the push buttons at their full size (see redraw_buttons()).

Runs after gen_vst.py (vst/gen_skin.sh does both):  python3 vst/skin_post.py <Plugin Skins dir>
Each strip keeps the generator's own geometry (square frames stacked vertically, frame count from the
image shape unless capped).
"""
import json
import math
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SS = 4                       # supersampling factor
BOX = (0x1e, 0x15, 0x33)     # theme_box: the frame fill the knob sits on
TRACK = (0x33, 0x27, 0x5a)
ARC = (0xff, 0x2e, 0x88)     # theme_accent
FACE = (0x24, 0x1a, 0x3d)    # theme_knob_face
FACE_EDGE = (0x3a, 0x2d, 0x5e)
POINTER = (0x24, 0xe0, 0xff) # theme_accent_hi
START, SWEEP = -135.0, 270.0 # degrees clockwise from 12 o'clock


def arc_box(c, rad):
    return (c - rad, c - rad, c + rad, c + rad)


def frame(size, t):
    s = size * SS
    c = s / 2
    ring_r = c - 7 * SS                 # arc centreline, clear of the frame edge and the glow
    ring_w = 7 * SS
    face_r = ring_r - 13 * SS
    # PIL angles: 0 = 3 o'clock, clockwise. Ours: 0 = 12 o'clock.
    a0 = START - 90
    a1 = a0 + SWEEP * t

    img = Image.new("RGB", (s, s), BOX)

    glow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    if t > 0.004:
        ImageDraw.Draw(glow).arc(arc_box(c, ring_r), a0, a1, fill=ARC + (150,), width=ring_w + 6 * SS)
        glow = glow.filter(ImageFilter.GaussianBlur(5 * SS))
        img.paste(glow, (0, 0), glow)

    d = ImageDraw.Draw(img)
    d.arc(arc_box(c, ring_r), a0, a0 + SWEEP, fill=TRACK, width=ring_w)
    if t > 0.004:
        d.arc(arc_box(c, ring_r), a0, a1, fill=ARC, width=ring_w)
        for a in (a0, a1):   # round caps
            x = c + (ring_r - ring_w / 2) * math.cos(math.radians(a))
            y = c + (ring_r - ring_w / 2) * math.sin(math.radians(a))
            d.ellipse((x - ring_w / 2, y - ring_w / 2, x + ring_w / 2, y + ring_w / 2), fill=ARC)

    d.ellipse(arc_box(c, face_r), fill=FACE_EDGE)
    d.ellipse(arc_box(c, face_r - 2 * SS), fill=FACE)

    ang = math.radians(START + SWEEP * t)
    px, py = math.sin(ang), -math.cos(ang)
    r_in, r_out, pw = face_r * 0.30, face_r * 0.82, 6 * SS
    x0, y0, x1, y1 = c + px * r_in, c + py * r_in, c + px * r_out, c + py * r_out
    d.line((x0, y0, x1, y1), fill=POINTER, width=pw)
    for x, y in ((x0, y0), (x1, y1)):
        d.ellipse((x - pw / 2, y - pw / 2, x + pw / 2, y + pw / 2), fill=POINTER)

    return img.resize((size, size), Image.LANCZOS)


MAX_STRIP = 16384            # MPC draws an image taller than this wrongly (mpc-vst-plugins catalog_check.py)


def rebuild_strip(path):
    """Redraw one filmstrip in place; returns its frame count, capped so the strip stays under MAX_STRIP px
    (the generator's 128 frames of a 130 px knob are 16640 px): the caller fixes numFrames in TUI.json."""
    w, h = Image.open(path).size
    n = min(h // w, MAX_STRIP // w)
    strip = Image.new("RGB", (w, w * n))
    for k in range(n):
        strip.paste(frame(w, k / (n - 1)), (0, k * w))
    strip.save(path, optimize=True)
    print("knob strip %s: %d frames of %dpx" % (os.path.basename(path), n, w))
    return n


def set_num_frames(skin, frames):
    """numFrames of every Knob whose filmStrip is in frames (name -> frame count) = the frame count. MPC cuts a strip
    into numFrames slices of image height / numFrames (measured on a Key 37, 2026-10-03), so count - 1 slices a
    126-frame strip 1 px per frame off and shows a mid-range knob split across two frames."""
    path = os.path.join(skin, "TUI.json")
    tui = json.load(open(path))
    hit = [0]

    def walk(o):
        if isinstance(o, dict):
            if o.get("filmStrip") in frames and "numFrames" in o:
                o["numFrames"] = frames[o["filmStrip"]]
                hit[0] += 1
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)
    walk(tui)
    json.dump(tui, open(path, "w"), indent=4)
    print("numFrames set on %d knobs" % hit[0])


def hide_qlink_bounds(skin):
    """Hide the orange Q-Link box and "Q" badge. layout.conf's hide_qlink_bounds=1 flags only the page
    components; the nested per-control components keep hideQLinkBounds=false and each page keeps its
    zone rectangle (qlinkBoundsData), so MPC can still draw the box. Every tab has one Q-Link bank, so
    the box carries no information: flag every component and give each page an empty zone."""
    path = os.path.join(skin, "TUI.json")
    tui = json.load(open(path))
    flags = [0]

    def walk(o):
        if isinstance(o, dict):
            if "hideQLinkBounds" in o:
                o["hideQLinkBounds"] = True
                flags[0] += 1
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)
    walk(tui)
    tabs = tui["pageData"]["tabs"]
    for t in tabs:
        t["qlinkBoundsData"] = ["0 0 0 0"]
    json.dump(tui, open(path, "w"), indent=4)
    print("q-link zone hidden: %d components flagged, %d pages emptied" % (flags[0], len(tabs)))


TEXT_SCALE = 1.4             # MPC draws a Label well under its nominal height (26 reads about 14 px tall)
BOX_TEXT = ("shReadout_", "shStepText_", "shRow_", "shMenu_", "shPopField_")


def scale_box_text(skin):
    """Value text of readouts, steppers, list rows, menus and popup fields x TEXT_SCALE, kept within the
    label's own height (MPC clips a Label's text rather than shrinking it)."""
    path = os.path.join(skin, "TUI.json")
    tui = json.load(open(path))
    n = 0
    for d in tui["pageData"]["componentDefinitions"]["localComponentDefinitions"]:
        if not d["key"].startswith(BOX_TEXT):
            continue
        for c in d["value"]["componentsData"]:
            if c["componentData"]["type"] != "Label":
                continue
            font = c["componentData"]["data"]["textStyle"]["font"]
            h = float(c["bounds"]["bounds"].split()[3])
            font["height"] = min(font["height"] * TEXT_SCALE, h)
            n += 1
    json.dump(tui, open(path, "w"), indent=4)
    print("box text scaled on %d labels" % n)


BTN_LINE = (0x46, 0x30, 0x6a)    # theme_line: the 2 px ring
BTN_TEXT = (0xfd, 0xf3, 0xea)    # the generator's title-font button label colour


def redraw_buttons(skin, layout):
    """The generator draws a title-font button's body sized from a blank placeholder label, so it comes out
    about 60 px wide inside a 131-154 px image with the label spilling over it. Redraw each on/off image at
    its full size: the ring, the body in the generator's own fill (sampled from the image) and the label in
    the title font (SHADOW_TITLE_FONT, as gen_skin.sh sets it)."""
    font_path = os.environ.get("SHADOW_TITLE_FONT")
    if not font_path:
        raise SystemExit("skin_post: SHADOW_TITLE_FONT is not set")
    labels = {}
    for line in open(layout):
        if line.startswith("button "):
            key, lab = re.search(r"\bkey=(\S+)", line), re.search(r'\blabel="([^"]*)"', line)
            if key and lab:
                labels[key.group(1)] = lab.group(1)
    n = 0
    for f in sorted(os.listdir(skin)):
        m = re.match(r"sh_btn_(.+)_(on|off)\.png$", f)
        key = m and next((k for k in labels if m.group(1).startswith(k + "_")), None)
        if not key:
            continue
        path = os.path.join(skin, f)
        im = Image.open(path).convert("RGBA")
        w, h = im.size
        under, fill = im.getpixel((0, 0)), im.getpixel((w // 2, 6))
        big = Image.new("RGBA", (w * SS, h * SS), under)
        dr = ImageDraw.Draw(big)
        dr.rounded_rectangle((0, 0, w * SS - 1, h * SS - 1), 10 * SS, fill=BTN_LINE)
        dr.rounded_rectangle((2 * SS, 2 * SS, (w - 2) * SS - 1, (h - 2) * SS - 1), 8 * SS, fill=fill)
        im = big.resize((w, h), Image.LANCZOS)
        dr = ImageDraw.Draw(im)
        font = ImageFont.truetype(font_path, max(10, int(h * 0.42)))
        tb = dr.textbbox((0, 0), labels[key], font=font)
        dr.text(((w - (tb[2] - tb[0])) / 2 - tb[0], (h - (tb[3] - tb[1])) / 2 - tb[1]), labels[key], font=font,
                fill=BTN_TEXT)
        im.convert("RGB").save(path)
        n += 1
    print("buttons redrawn: %d images" % n)


def main(skin):
    hide_qlink_bounds(skin)
    scale_box_text(skin)
    redraw_buttons(skin, os.path.join(os.path.dirname(os.path.abspath(__file__)), "layout.conf"))
    frames = {}
    for root, _dirs, files in os.walk(skin):
        for f in sorted(files):
            if f.startswith("sh_knob_r") and f.endswith(".png"):
                frames[f] = rebuild_strip(os.path.join(root, f))
    if not frames:
        raise SystemExit("skin_post: no sh_knob_r*.png filmstrips found under " + skin)
    set_num_frames(skin, frames)


if __name__ == "__main__":
    main(sys.argv[1])
