"""Map rotation: stop parsing, the default order and the next stop."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from dedi.builtin import rotation as r


def check(name, condition):
    print("[%s] %s" % ("PASS" if condition else "FAIL", name))
    return 0 if condition else 1


def main():
    fails = 0
    d = r.default_rotation()
    fails += check("default: 8 stops, city 0 mp 0 then mp 1 then city 1",
                   [(s["city"], s["arena"]) for s in d[:3]] == [(0, 0), (0, 1), (1, 0)] and len(d) == 8)
    fails += check("parse string", r.parse_stop("havana mp 1") == {"city": 1, "map_mode": "mp", "arena": 1, "area": -1})
    fails += check("parse city", r.parse_stop("vegas city")["map_mode"] == "city")
    fails += check("parse dict", r.parse_stop({"city": "rio", "arena": 1})["city"] == 3)
    for bad in ("paris mp", "chicago mp 2", "chicago space"):
        try:
            r.parse_stop(bad)
            ok = False
        except ValueError:
            ok = True
        fails += check("reject %r" % bad, ok)
    fails += check("next after chicago mp 0", r.next_stop(d, 0, "mp", 0) == d[1])
    fails += check("next after rio mp 1 wraps", r.next_stop(d, 3, "mp", 1) == d[0])
    fails += check("not in list -> first", r.next_stop(d, 2, "city", 0) == d[0])
    fails += check("empty config -> default", r.stops_from(None) == d and r.stops_from([]) == d)

    # the plugin over a fake server: counts ended rounds, switches, never saves
    class Fake:
        def __init__(self):
            self.cfg = {"city": 0, "map_mode": "mp", "arena": 0, "rounds_per_map": 2}
            self.rnd = (0, False)
            self.kicked = []
            self.saved = False
            self.log = type("L", (), {"info": lambda *a: None, "warning": lambda *a: None})()
        def config_get(self, k, d=None): return self.cfg.get(k, d)
        def round_info(self): return self.rnd
        def players(self): return ["p"]
        def broadcast(self, *a): pass
        def set_city(self, c): self.cfg["city"] = c
        def set_map_mode(self, m): self.cfg["map_mode"] = m
        def set_arena(self, a): self.cfg["arena"] = a
        def kick(self, p, *a, **k): self.kicked.append(k.get("rejoin"))
        def save_config(self, *k): self.saved = True
    f = Fake()
    r.on_load(f)
    r.SWITCH_DELAY = 0
    r.on_tick()
    f.rnd = (1, True); r.on_tick(); r.on_tick()
    fails += check("one round: no switch", f.cfg["arena"] == 0 and not f.kicked)
    f.rnd = (2, False); r.on_tick()
    f.rnd = (2, True); r.on_tick()
    fails += check("two rounds: chicago mp 1, rejoin kick, not saved",
                   f.cfg["arena"] == 1 and f.kicked == [True] and not f.saved)
    f.cfg["rounds_per_map"] = 0
    f.rnd = (3, True); r.on_tick(); f.rnd = (4, True); r.on_tick()
    fails += check("rounds_per_map 0: off", f.cfg["arena"] == 1 and len(f.kicked) == 1)

    print("\nrotation tests: %s" % ("PASS" if not fails else "FAIL (%d)" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
