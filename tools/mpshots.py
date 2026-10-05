"""Screenshots of the running game, taken WHEN SOMETHING HAPPENS.

Written for one job: watch a session's logs for a car change and photograph every window
that seat owns at that moment - the game itself AND the VRAM viewer (-vramview) - so the
picture and the log line that caused it land in the same place with the same number.

Two verbs:

  python mpshots.py watch --shots DIR --seat host=LOG[:PID] --seat client=LOG[:PID] [--seconds N]
  python mpshots.py grab  --pid 1234 --out shot.png [--title VRAM]

The index it writes into DIR - index.txt - is the point of it: one row per capture, with
the line that triggered it and the latest VRAM line seen in that same log, so a reader can
put "the car went invisible" next to "pages 30 used, 4 reclaimed".

Why a window grab rather than a screenshot of the whole desktop: a run has two seats plus a
VRAM window, and a whole-screen shot answers "which of these am I looking at" with "all of
them, at whatever size they happen to be". Windows are found by PID, so a seat's own
windows are never mixed up with the other seat's.
"""

import argparse
import ctypes
import ctypes.wintypes as wintypes
import os
import re
import sys
import time

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

# The lines that mean "a car changed, look now". Every one of these is a real line from
# either the mp mod or carhacks' hot-load path; the set is deliberately the CHANGE events
# only (not every mention of a car), or the run would photograph itself every frame.
CHANGE_PATTERNS = (
    "car status:",             # mp: driving model N (resident R, source C); mesh ...
    "change car:",             # the pause menu / lever applying a pick
    "hot-loaded",              # carhacks: built a car into a resident slot
    "rebuilt player",          # mp: rebuilt a peer's car on a slot
    "car chosen:",             # carhacks: the pick was served
    "-> resident slot",        # carhacks/net: this peer's car landed on slot N
)

# The VRAM state side of the correlation. Taken from the same log, the most recent one.
#
# Deliberately NOT any line containing "resident slot": the change lines themselves say
# "-> resident slot N", so including it had the correlation reporting the car change as the
# VRAM state - the thing it is supposed to sit next to. This list is the pool/watermark
# reporting only.
VRAM_PATTERNS = (
    "JERICHO-VRAM",
    "pool -",
    "pages ",
    "row(s) reclaimed",
    "CLUT watermark",
)

MIN_W = 80
MIN_H = 60


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


def window_image(hwnd, box):
    """Ask the WINDOW to draw itself, rather than photographing that patch of screen.

    The difference matters: a run has three windows stacked on one desktop, and
    ImageGrab answers with whatever is on top of that rectangle - which is how this first
    produced three nearly-black PNGs. PrintWindow works for a window behind another one,
    and PW_RENDERFULLCONTENT (2) is the flag a Direct3D-rendered window needs.

    Returns a PIL image, or None if the window refused (then the caller may fall back).
    """
    from PIL import Image

    w = box[2] - box[0]
    h = box[3] - box[1]

    if w <= 0 or h <= 0:
        return None

    hdc = user32.GetWindowDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mdc, bmp)

    ok = user32.PrintWindow(hwnd, mdc, 2)

    info = BITMAPINFO()
    info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    info.bmiHeader.biWidth = w
    info.bmiHeader.biHeight = -h            # negative = top-down rows
    info.bmiHeader.biPlanes = 1
    info.bmiHeader.biBitCount = 32
    info.bmiHeader.biCompression = 0        # BI_RGB

    buf = ctypes.create_string_buffer(w * h * 4)
    lines = gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(info), 0)

    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(hwnd, hdc)

    if not ok or lines == 0:
        return None

    return Image.frombuffer("RGB", (w, h), buf, "raw", "BGRX", 0, 1)


def windows_of(pid):
    """Every visible, plausibly-sized top-level window owned by this PID."""
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _lparam):
        owner = wintypes.DWORD()

        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))

        if owner.value == pid and user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)

            r = RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(r))

            if (r.right - r.left) >= MIN_W and (r.bottom - r.top) >= MIN_H:
                found.append((hwnd, buf.value, (r.left, r.top, r.right, r.bottom)))

        return True

    user32.EnumWindows(cb, 0)
    return found


def safe(name, fallback):
    out = re.sub(r"[^A-Za-z0-9._-]+", "_", (name or "").strip()).strip("_")
    return out[:40] or fallback


def grab(pid, out, title=None, all_screens=True):
    """Photograph this PID's windows. Returns the list of files written."""
    from PIL import ImageGrab

    wins = windows_of(pid)
    files = []

    for i, (hwnd, name, box) in enumerate(wins):
        if title is not None and title.lower() not in name.lower():
            continue

        if len(wins) == 1:
            path = out
        else:
            base, ext = os.path.splitext(out)
            path = "%s-%s%s" % (base, safe(name, "win%d" % i), ext or ".png")

        try:
            img = window_image(hwnd, box)

            if img is None:                 # refused: fall back to the screen patch
                img = ImageGrab.grab(bbox=box, all_screens=all_screens)

            img.save(path)
            files.append(path)
        except Exception as e:                                  # noqa: BLE001
            sys.stderr.write("mpshots: %s: %s\n" % (name or hwnd, e))

    return files


def read_new(path, state):
    """New lines since last call. Tolerates the log being truncated on open."""
    if not os.path.exists(path):
        return []

    size = os.path.getsize(path)

    if size < state["pos"]:                 # truncated / rewritten: start over
        state["pos"] = 0

    if size == state["pos"]:
        return []

    with open(path, "rb") as f:
        f.seek(state["pos"])
        data = f.read()
        state["pos"] = f.tell()

    return [l.rstrip() for l in data.decode("utf-8", "replace").splitlines() if l.strip()]


def watch(seats, shots_dir, seconds, poll=0.2, quiet=False):
    """seats: {name: (log_path, pid)}. Poll the logs; photograph on every car change."""
    os.makedirs(shots_dir, exist_ok=True)

    index = os.path.join(shots_dir, "index.txt")
    state = {n: {"pos": 0, "n": 0, "vram": "", "last": 0.0} for n in seats}
    rows = []
    end = time.time() + seconds

    while time.time() < end:
        for name, (logpath, pid) in seats.items():
            st = state[name]

            for line in read_new(logpath, st):
                if any(p in line for p in VRAM_PATTERNS):
                    st["vram"] = line.strip()

                if any(p in line for p in CHANGE_PATTERNS):
                    # One switch writes several matching lines within the same frame
                    # (hot-load, cosmetics, the rebuild, the pick, the status), so
                    # photograph the FIRST of the group and let the rest only update the
                    # running VRAM line - otherwise one car change leaves five sets of
                    # near-identical pictures to read past.
                    if time.time() - st["last"] < 0.6:
                        continue

                    st["last"] = time.time()
                    st["n"] += 1

                    out = os.path.join(shots_dir, "%s-%02d.png" % (name, st["n"]))
                    files = grab(pid, out)

                    rows.append("%s  %s#%d  %s\n    vram: %s\n    files: %s" % (
                        time.strftime("%H:%M:%S"), name, st["n"], line.strip(),
                        st["vram"] or "(none seen yet)",
                        ", ".join(os.path.basename(f) for f in files) or "(no window!)"))

                    if not quiet:
                        print("[shots] %s#%d %s -> %d file(s)" % (
                            name, st["n"], line.strip()[:80], len(files)))

        time.sleep(poll)

    with open(index, "w", encoding="utf-8") as f:
        f.write("# car changes, newest last; the line that triggered each one, and the\n"
                "# VRAM line in the same log at that moment\n\n")
        f.write("\n\n".join(rows) if rows else "(no car change seen)\n")

    if not quiet:
        print("[shots] %d capture(s) -> %s" % (len(rows), index))

    return len(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="verb", required=True)

    w = sub.add_parser("watch", help="photograph on every car change in these logs")
    w.add_argument("--shots", required=True, metavar="DIR")
    w.add_argument("--seat", action="append", default=[], metavar="NAME=LOG[:PID]",
                   help="repeatable, e.g. --seat host=.../a/JERICHO.log:1234")
    w.add_argument("--seconds", type=int, default=60)
    w.add_argument("--quiet", action="store_true")

    g = sub.add_parser("grab", help="one shot, right now")
    g.add_argument("--pid", type=int, required=True)
    g.add_argument("--out", required=True, metavar="FILE")
    g.add_argument("--title", default=None,
                   help="only windows whose title contains this (e.g. VRAM)")

    a = ap.parse_args()

    if a.verb == "grab":
        files = grab(a.pid, a.out, a.title)

        if not files:
            print("mpshots: no window of pid %d matched (is the game running?)" % a.pid)
            return 1

        for f in files:
            print(f)
        return 0

    seats = {}

    for spec in a.seat:
        if "=" not in spec:
            print("mpshots: --seat wants NAME=LOG[:PID], got %r" % spec)
            return 2

        name, rest = spec.split("=", 1)
        log, _, pid = rest.rpartition(":")

        if not log:                     # no PID given: the log path is the whole thing
            log, pid = rest, "0"

        seats[name] = (log, int(pid) if pid.isdigit() else 0)

        if not seats[name][1]:
            sys.stderr.write("mpshots: %s has no pid - windows cannot be found for it\n" % name)

    if not seats:
        print("mpshots: give at least one --seat")
        return 2

    watch(seats, a.shots, a.seconds, quiet=a.quiet)
    return 0


if __name__ == "__main__":
    sys.exit(main())
