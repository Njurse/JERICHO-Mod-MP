#!/usr/bin/env python3
"""proto_check.py -- fail loudly when the Python wire mirror drifts from mp_proto.h.

`mp_test.py` is the single Python source of truth for the mp wire format, and it is a
*hand-maintained* mirror of `JERICHO/MODS/mp/mp_proto.h`. Hand-maintained mirrors
drift, and when this one did it broke a real path silently:

    v11 added `uint8_t rules` to `MP_WELCOME` in C (commit "Host Game session
    settings"), making the struct 18 bytes. The Python side kept packing the old
    17-byte layout, so the mock host and the dedicated server sent a WELCOME the
    game refuses outright -- `MpHandleWelcome` starts with
    `if (len < sizeof(MP_WELCOME)) return;` and just drops it. Nothing logged; the
    client simply sat in the handshake until it timed out.

This module reads the ACTUAL `static_assert(sizeof(MP_X) == N, ...)` lines out of the
C header (so the header stays the source of truth, not a number copied here), derives
the same sizes from the `struct.Struct` formats in `mp_test.py`, and compares them,
plus the shared constants (`MP_PROTO_VERSION`, `MP_UDP_MAGIC`, `MP_MAX_PLAYERS`, ...).

Both servers call `assert_ok()` at startup so a stale checkout fails at once rather
than after a mysterious client timeout; the CI "Checks" job runs it directly.

    python JERICHO/MODS/mp/tools/servers/proto_check.py

Exit 0 = in sync, 1 = drift (it prints the struct and both numbers).
"""
from __future__ import annotations

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)          # .../JERICHO/MODS/mp/tools
MP_DIR = os.path.dirname(TOOLS)        # .../JERICHO/MODS/mp
PROTO_H = os.path.join(MP_DIR, "mp_proto.h")

sys.path.insert(0, TOOLS)
import mp_test as m  # noqa: E402  (path insert must come first)

# C struct -> the object in mp_test.py that must mirror it byte for byte.
MIRROR = {
    "MP_ENVELOPE": "ENV",
    "MP_BEACON": "BEACON",
    "MP_MOD_INFO": "MODI",
    "MP_HELLO": "HELLO",
    "MP_WELCOME": "WELCOME",
    "MP_REJECT": "REJECT",
    "MP_SESSION": "SESSION",
    "MP_INPUT": "INPUT",
    "MP_PLAYER_INPUT": "PLAYER_INPUT",
    "MP_CARSTATE": "CARSTATE",
    "MP_CARSTATE_ENTRY": "CARSTATE_ENTRY",
    "MP_ROSTER_ENTRY": "ROSTER_ENTRY",
    "MP_CHANNEL": "CHANNEL",
    "MP_HIT": "HIT",
    "MP_PEDSTATE_ENTRY": "PEDSTATE_ENTRY",
    "MP_COLOR_ENTRY": "COLOR_ENTRY",
    "MP_STATS_ENTRY": "STATS_ENTRY",
}

# C #define -> the Python constant that must carry the same value.
CONSTANTS = {
    "MP_PROTO_VERSION": "PROTO",
    "MP_UDP_MAGIC": "UDP_MAGIC",
    "MP_MAX_PLAYERS": "MAX_PLAYERS",
    "MP_NAME_MAX": "NAME_MAX",
    "MP_MOD_ID_MAX": "MOD_ID_MAX",
    "MP_MOD_VER_MAX": "MOD_VER_MAX",
    "MP_CARSTATE_LEFT_CAR": "CARSTATE_LEFT_CAR",
}


def _read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def c_static_sizes(text):
    """{type name: size} from every `static_assert(sizeof(MP_X) == N, ...)`."""
    out = {}
    for mm in re.finditer(
        r"static_assert\(\s*sizeof\(\s*(MP_[A-Z0-9_]+)\s*\)\s*==\s*(\d+)\s*,", text
    ):
        out[mm.group(1)] = int(mm.group(2))
    return out


def c_define(name, text):
    """Integer value of `#define <name> <int>`, or None (hex accepted)."""
    mm = re.search(r"#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)\b" % re.escape(name), text)
    if not mm:
        return None
    return int(mm.group(1), 0)


def check(proto_text=None, tool=None):
    """Return a list of human-readable drift problems ([] == in sync)."""
    if proto_text is None:
        proto_text = _read(PROTO_H)
    if tool is None:
        tool = m
    problems = []

    sizes = c_static_sizes(proto_text)
    for cname, pyname in sorted(MIRROR.items()):
        csize = sizes.get(cname)
        if csize is None:
            problems.append(
                "%s is no longer sized by a static_assert in mp_proto.h -- "
                "the Python mirror cannot be verified" % cname
            )
            continue
        obj = getattr(tool, pyname, None)
        if obj is None:
            problems.append("mp_test.py defines no %s (mirrors %s)" % (pyname, cname))
            continue
        if obj.size != csize:
            problems.append(
                "%s is %d bytes in mp_proto.h but mp_test.py %s is %d (%r)"
                % (cname, csize, pyname, obj.size, obj.format)
            )

    for cname, pyname in sorted(CONSTANTS.items()):
        cval = c_define(cname, proto_text)
        if cval is None:
            problems.append("mp_proto.h no longer #defines %s" % cname)
            continue
        pval = getattr(tool, pyname, None)
        if pval is None:
            problems.append("mp_test.py defines no %s (mirrors %s)" % (pyname, cname))
            continue
        if pval != cval:
            problems.append(
                "%s is %s in mp_proto.h but mp_test.py %s is %s"
                % (cname, cval, pyname, pval)
            )

    return problems


def assert_ok(tool=None):
    """Raise SystemExit(1) with a clear message if the mirror has drifted.

    Servers call this at startup: a stale checkout should fail immediately and
    legibly, not produce a handshake that the game silently drops.
    """
    problems = check(tool=tool)
    if problems:
        sys.stderr.write(
            "proto_check: FATAL -- the Python wire mirror drifted from mp_proto.h:\n"
        )
        for p in problems:
            sys.stderr.write("  - %s\n" % p)
        sys.stderr.write(
            "Update mp_test.py (the single source of truth for the tools), then retry.\n"
        )
        raise SystemExit(1)


def main():
    if not os.path.exists(PROTO_H):
        print("proto_check: cannot find %s -- run from a full checkout." % PROTO_H)
        return 1
    problems = check()
    if problems:
        print("proto_check: FAIL -- the Python wire mirror drifted from mp_proto.h:")
        for p in problems:
            print("  - " + p)
        return 1
    print(
        "proto_check: OK -- %d mirrored struct size(s) and %d shared constant(s) match "
        "mp_proto.h" % (len(MIRROR), len(CONSTANTS))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
