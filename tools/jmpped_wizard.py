#!/usr/bin/env python3
"""jmpped_wizard.py - small console wizard for the JMPPED toolkit.

The primed bake workspace and the source export are separate: this wizard never
re-exports into the bake. Bake/register only read the selected bake workspace;
"Refresh source" writes the separate source workspace; "Prime" is the explicit
copy from source to bake.
"""

from __future__ import annotations

import os
import subprocess
import sys

import jmpped_common as jc
import jmpped_entries as je
import jmpped_import as ji
import jmpped_workspace as jws


HERE = os.path.dirname(os.path.abspath(__file__))


def _py(script: str) -> list[str]:
    return [sys.executable, os.path.join(HERE, script)]


def _run(cmd: list[str]) -> int:
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=os.path.dirname(HERE)).returncode


def _print_status() -> None:
    bake = jws.bake_root()
    source = jws.source_root()
    st = jws.status(bake)
    print("\nBake workspace : %s" % bake)
    print("Source export  : %s%s" % (source, "" if os.path.isdir(source) else " (missing)"))
    print("Manifest       : %s" % ("yes" if st["manifest"] else "no"))
    print("Entries        : %s" % (", ".join(st["entries"]) if st["entries"] else "none"))
    if st["base"]:
        print("Base archive   : %s (%s)" % (st["base"], "ok" if st["base_ok"] else "MISSING/CHANGED"))
    print()


def _ask(prompt: str) -> str:
    return input(prompt).strip()


def _register() -> None:
    folder = _ask("Character folder to import (contains ped.json): ").strip('"')
    if not folder:
        return
    try:
        _, data = ji._doc(folder)
        items = data["entries"] if isinstance(data.get("entries"), list) else [data]
        names = [n for n in (it.get("name") or it.get("label") or it.get("ped")
                             for it in items) if n]
        have = {e.get("name") for e in je.load_manifest(jws.bake_root()).get("entries", [])}
        clashes = [n for n in names if n in have]
        if clashes:
            print("This import will replace: %s" % ", ".join(clashes))
            if _ask("Replace them? [y/N] ").lower() not in ("y", "yes"):
                print("Import cancelled.")
                return
    except (jc.JMPPEDError, OSError):
        pass
    _run(_py("jmpped_register.py") + ["--folder", folder,
                                      "--out", jws.bake_root()])


def _add_entry() -> None:
    name = _ask("New entry display name: ")
    if not name:
        return
    _run(_py("jmpped_bake.py") + ["add-entry", "--name", name,
                                  "--out", jws.bake_root()])


def _split_crops() -> None:
    entry = _ask("Entry name (default Jericho Driv3r): ") or "Jericho Driv3r"
    _run(_py("jmpped_crops.py") + ["--out", jws.bake_root(),
                                   "--entry", entry])


def _refresh_source() -> None:
    print("\nRefreshing writes the SOURCE workspace only; the primed bake stays put.")
    if _ask("Type REFRESH to export from the Driver 2 level: ") != "REFRESH":
        return
    _run(_py("jmpped_export.py") + ["--out", jws.source_root(), "--reset"])


def _prime() -> None:
    script = os.path.join(HERE, "jmpped_prime.py")
    if not os.path.isfile(script):
        print("Prime is not available yet (jmpped_prime.py missing).")
        return
    if _ask("Type PRIME to copy source assets into the bake workspace: ") != "PRIME":
        return
    _run(_py("jmpped_prime.py") + ["--source", jws.source_root(),
                                   "--out", jws.bake_root()])


def _change_workspace() -> None:
    which = _ask("Change [b]ake or [s]ource workspace? ").lower()
    if which == "b":
        path = _ask("Bake workspace path: ").strip('"')
        if path:
            jws.set_bake(os.path.abspath(path))
    elif which == "s":
        path = _ask("Source workspace path: ").strip('"')
        if path:
            jws.set_source(os.path.abspath(path))
    else:
        print("No change.")


def main() -> int:
    if "--status" in sys.argv:
        _print_status()
        return 0

    while True:
        _print_status()
        print("1) Bake the primed workspace")
        print("2) Import/register a character folder")
        print("3) Add a new entry")
        print("4) Split a bindpose")
        print("5) Split part textures into per-material crops")
        print("6) Preview")
        print("7) Refresh source export (does not touch the bake)")
        print("8) Prime bake from source")
        print("9) Change bake/source workspace")
        print("10) Show status")
        print("0) Quit")
        choice = _ask("Choice: ")

        if choice == "0":
            return 0
        if choice == "1":
            _run(_py("jmpped_bake.py") + ["build", "--out", jws.bake_root(),
                                          "--copy"])
        elif choice == "2":
            _register()
        elif choice == "3":
            _add_entry()
        elif choice == "4":
            variant = _ask("Variant [jericho/tanner] (default jericho): ") or "jericho"
            _run(_py("jmpped_split.py") + ["--variant", variant,
                                           "--out", jws.bake_root()])
        elif choice == "5":
            _split_crops()
        elif choice == "6":
            _run(_py("jmpped_bake.py") + ["preview", "--out", jws.bake_root()])
        elif choice == "7":
            _refresh_source()
        elif choice == "8":
            _prime()
        elif choice == "9":
            _change_workspace()
        elif choice == "10":
            _print_status()
        else:
            print("Unknown choice.")


if __name__ == "__main__":
    raise SystemExit(main())
