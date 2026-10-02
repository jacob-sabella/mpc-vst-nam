"""Post-build pass over the generated NAM skin:
- replaces the stock knob filmstrips with the synthwave knob (270-degree track, lit magenta value arc
  with a soft glow, dark face, cyan pointer), with fewer frames where 128 would make a strip taller
  than MPC draws correctly (16384 px), and numFrames in TUI.json to match;
- hides the Q-Link zone outline on every page (see hide_qlink_bounds()).

Runs after gen_vst.py (vst/gen_skin.sh does both):  python3 vst/skin_post.py <Plugin Skins dir>
Each strip keeps the generator's own geometry (square frames stacked vertically, frame count from the
image shape unless capped).
"""
import json
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

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
    """numFrames of every Knob whose filmStrip is in frames (name -> frame count) = count - 1, as the generator writes it."""
    path = os.path.join(skin, "TUI.json")
    tui = json.load(open(path))
    hit = [0]

    def walk(o):
        if isinstance(o, dict):
            if o.get("filmStrip") in frames and "numFrames" in o:
                o["numFrames"] = frames[o["filmStrip"]] - 1
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


def main(skin):
    hide_qlink_bounds(skin)
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
