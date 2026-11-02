"""Regression tests for city-vs-multiplayer vote target semantics."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from dedi.builtin import vote


def check(name, condition):
    print("[%s] %s" % ("PASS" if condition else "FAIL", name))
    return 0 if condition else 1


def main():
    fails = 0
    cases = [
        ("city chicago", (0, "city", None)),
        ("city vegas", (2, "city", None)),
        ("city chicago mp", (0, "mp", None)),
        ("city chicago mp 0", (0, "mp", 0)),
        ("city chicago mp 1", (0, "mp", 1)),
        ("mp", (None, "mp", None)),
        ("mp 1", (None, "mp", 1)),
        ("vegas", (2, "city", None)),  # legacy spelling means full city
        ("vegas mp", (2, "mp", None)),
    ]
    for command, expected in cases:
        try:
            actual = vote.parse_target(command)
        except Exception as exc:
            actual = repr(exc)
        fails += check("parse %r -> %r" % (command, expected), actual == expected)

    for command in ("city", "city chicago mp 2", "city chicago nonsense", "mp 3"):
        try:
            vote.parse_target(command)
            valid = False
        except ValueError:
            valid = True
        fails += check("reject invalid target %r" % command, valid)

    print("\nvote target tests: %s" % ("PASS" if not fails else "FAIL (%d)" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
