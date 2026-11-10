#!/usr/bin/env python3
"""Build and run the game-mode registry check (test_mp_mode.cpp + mp_mode.c).

    python tools/mode_check/run_mode_check.py [--cxx "clang++"]

The compiler is --cxx, else $CXX, else the first of c++ / clang++ / g++ on PATH,
else `python -m ziglang c++` when the ziglang package is installed. Exit 0 = every
mode-0 answer is what the session did before modes existed.

Also checks, without a compiler, that the mode names in mp_mode.c match the Python
servers' mirror (tools/mp_test.py GAMEMODE_NAMES).
"""
import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MP = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(MP, "tools"))


def c_mode_names():
    """{id: name} from the kModes table in mp_mode.c (id macro resolved via mp_proto.h)."""
    proto = open(os.path.join(MP, "mp_proto.h"), encoding="utf-8").read()
    consts = {}
    m = re.search(r"enum\s*\{([^}]*MP_GAMEMODE_TAKEADRIDE[^}]*)\}", proto)
    if m:
        n = 0
        for part in m.group(1).split(","):
            part = re.sub(r"/\*.*?\*/", "", part, flags=re.S).strip()
            if not part:
                continue
            if "=" in part:
                k, v = (s.strip() for s in part.split("=", 1))
                n = int(v, 0)
            else:
                k = part
            consts[k] = n
            n += 1
    src = open(os.path.join(MP, "mp_mode.c"), encoding="utf-8").read()
    rows = re.findall(r"\{\s*(MP_GAMEMODE_\w+|\d+)\s*,\s*/\*\s*id\s*\*/\s*\"([^\"]*)\"", src)
    return {consts.get(i, None) if not i.isdigit() else int(i): name for i, name in rows}


def check_names():
    import mp_test
    c = c_mode_names()
    py = dict(mp_test.GAMEMODE_NAMES)
    ok = bool(c) and None not in c and c == py
    print("names: mp_mode.c %r vs mp_test.GAMEMODE_NAMES %r -> %s" % (c, py, "OK" if ok else "MISMATCH"), flush=True)
    ok = ok and mp_test.mode_name(0) == "TAKE A RIDE"
    return ok


def find_cxx(arg):
    if arg:
        return shlex.split(arg)
    if os.environ.get("CXX"):
        return shlex.split(os.environ["CXX"])
    for c in ("c++", "clang++", "g++"):
        if shutil.which(c):
            return [c]
    try:
        import ziglang  # noqa: F401
        return [sys.executable, "-m", "ziglang", "c++"]
    except ImportError:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx")
    ap.add_argument("--names-only", action="store_true")
    a = ap.parse_args()

    ok = check_names()
    if a.names_only:
        return 0 if ok else 1

    cxx = find_cxx(a.cxx)
    if cxx is None:
        print("no C++ compiler found (set CXX or --cxx); the C check did not run", flush=True)
        return 2

    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_mp_mode" + (".exe" if os.name == "nt" else ""))
        cmd = cxx + ["-std=gnu++14", "-I", MP, "-x", "c++",
                     os.path.join(HERE, "test_mp_mode.cpp"),
                     os.path.join(MP, "mp_mode.c"), "-o", exe]
        print("build:", " ".join(cmd), flush=True)
        if subprocess.call(cmd) != 0:
            print("build FAILED", flush=True)
            return 1
        rc = subprocess.call([exe])
    return 0 if (ok and rc == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
