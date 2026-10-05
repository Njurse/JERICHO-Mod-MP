#!/usr/bin/env python3
"""mpgif.py - turn mpshots.py's captures into an annotated GIF you can actually watch.

mpshots.py already photographs the right thing at the right moment: every window a seat
owns (the game AND its -vramview) each time a car change appears in that seat's log, with
index.txt correlating each capture to the line that triggered it and the latest VRAM line
seen. What it does not do is put those frames in motion, or say on the picture what the
numbers mean.

This does both, from data the run already produced - nothing here invents a metric:

  trigger line   which seat and which car change caused this frame   (index.txt)
  paging         the latest VRAM/pool line seen in that seat's log   (index.txt)
  imports        "import: slot N <- model M from CITY" from the log   (--log, optional)

    python mpgif.py from-dir SHOTS_DIR --out run.gif
    python mpgif.py from-dir SHOTS_DIR --out run.gif --log path/to/JERICHO.log --ms 700

It is a separate tool on purpose: mpshots.py works and a live two-seat run is expensive to
set up, so nothing here touches it. Any shots directory it has ever written can be turned
into a GIF afterwards.
"""
import argparse
import io
import os
import re
import sys


def read_index(shots_dir):
    """Parse index.txt into [{time, seat, n, trigger, vram, files}]."""
    path = os.path.join(shots_dir, "index.txt")
    if not os.path.exists(path):
        return []
    text = io.open(path, encoding="utf-8", errors="replace").read()
    out = []
    for block in [b for b in text.split("\n\n") if b.strip()]:
        lines = [l for l in block.splitlines() if l.strip()]
        if not lines:
            continue
        # "<time>  <seat>#<n>  <trigger...>"
        m = re.match(r"^(\S+)\s+(\S+?)#(\d+)\s+(.*)$", lines[0])
        if not m:
            continue
        rec = {"time": m.group(1), "seat": m.group(2), "n": int(m.group(3)),
               "trigger": m.group(4).strip(), "vram": "", "files": []}
        for l in lines[1:]:
            s = l.strip()
            if s.startswith("vram:"):
                rec["vram"] = s[5:].strip()
            elif s.startswith("files:"):
                rec["files"] = [f.strip() for f in s[6:].split(",") if f.strip()]
        out.append(rec)
    return out


def read_imports(log_path):
    """The session's imports, as the module prints them."""
    if not log_path or not os.path.exists(log_path):
        return []
    seen, out = set(), []
    for line in io.open(log_path, encoding="utf-8", errors="replace"):
        if "import: slot" in line and "<-" in line:
            txt = line.strip()
            # "[carhacks] import: slot 5 <- model 1 from FRISCO"
            m = re.search(r"slot\s+(\d+)\s+<-\s+model\s+(\d+)\s+from\s+(\S+)", txt)
            if m and txt not in seen:
                seen.add(txt)
                out.append("slot %s: %s %s" % (m.group(1), m.group(3), m.group(2)))
    return out


def compose(frame_path, rec, imports, size, head_h):
    """One frame: the capture, scaled, above a caption strip."""
    from PIL import Image, ImageDraw
    img = Image.open(frame_path).convert("RGB")
    w, h = size
    avail = h - head_h
    # fit inside the frame box, preserving aspect
    s = min(w / img.width, avail / img.height)
    img = img.resize((max(1, int(img.width * s)), max(1, int(img.height * s))))
    canvas = Image.new("RGB", (w, h), (0, 0, 0))
    canvas.paste(img, ((w - img.width) // 2, head_h + (avail - img.height) // 2))

    d = ImageDraw.Draw(canvas)
    d.rectangle([0, 0, w, head_h - 1], fill=(16, 16, 24))
    y = 4
    d.text((6, y), "%s  %s#%d" % (rec["time"], rec["seat"], rec["n"]), fill=(255, 255, 255))
    y += 13
    trig = rec["trigger"][:96]
    d.text((6, y), "car: %s" % trig, fill=(255, 220, 120))
    y += 13
    if imports:
        d.text((6, y), "imported: %s" % (", ".join(imports))[:110], fill=(140, 220, 255))
        y += 13
    if rec["vram"]:
        d.text((6, y), "paging: %s" % rec["vram"][:110], fill=(160, 255, 160))
    elif y < head_h - 12:
        d.text((6, y), "paging: (no vram line seen yet)", fill=(160, 160, 160))
    return canvas


def main(argv=None):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    fd = sub.add_parser("from-dir", help="build a GIF from an mpshots.py shots directory")
    fd.add_argument("shots_dir")
    fd.add_argument("--out", required=True, metavar="FILE")
    fd.add_argument("--log", default=None,
                    help="a seat's JERICHO.log, for the imported-car line")
    fd.add_argument("--seat", default=None, help="only this seat's frames (default: all)")
    fd.add_argument("--ms", type=int, default=700, help="per-frame duration (default 700)")
    fd.add_argument("--width", type=int, default=720)
    fd.add_argument("--height", type=int, default=560)
    fd.add_argument("--head", type=int, default=56, help="caption strip height")
    a = ap.parse_args(argv)

    try:
        from PIL import Image, ImageDraw            # noqa: F401
    except ImportError:
        print("needs Pillow (pip install pillow)")
        return 2

    recs = read_index(a.shots_dir)
    if not recs:
        print("no index.txt in %s - nothing to build" % a.shots_dir)
        return 2
    if a.seat:
        recs = [r for r in recs if r["seat"] == a.seat]
    imports = read_imports(a.log)

    frames, kept = [], 0
    for rec in recs:
        # one frame per capture: the seat's own windows, as mpshots recorded them
        for f in rec["files"]:
            path = os.path.join(a.shots_dir, f)
            if os.path.basename(f) == "index.txt" or not os.path.exists(path):
                continue
            if not f.lower().endswith((".png", ".jpg", ".bmp")):
                continue
            frames.append(compose(path, rec, imports, (a.width, a.height), a.head))
            kept += 1
            break            # prefer one picture per event, not one per window

    if not frames:
        print("index.txt lists %d capture(s) but no usable image files were found" % len(recs))
        return 3

    frames[0].save(a.out, save_all=True, append_images=frames[1:],
                   duration=a.ms, loop=0, optimize=True)
    print("wrote %s: %d frame(s) from %d capture(s)%s"
          % (a.out, len(frames), len(recs),
             (", imports: %s" % ", ".join(imports)) if imports else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
