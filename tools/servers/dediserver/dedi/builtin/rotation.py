"""Map rotation: after `rounds_per_map` rounds, switch to the next stop.

Keys (read with defaults in code, never written back -- a rotation step does
NOT save dediserver.json, so a restart begins where the operator configured):

  "rounds_per_map": 0     rounds on one map before the next stop; 0 = off
  "rotation": [...]       the stops, in order. Each is a string
                          ("chicago mp 0", "havana mp", "vegas city") or an
                          object {"city": 1, "map_mode": "mp", "arena": 0,
                          "area": -1}. Default: every city's two multiplayer
                          maps, city by city (areas within a city first, then
                          the next city).

`area` is reserved: a stop inside one map (an arena region of a big city
map) for the arena work. Today a stop is a level: city + map mode + arena, and
`area` must be -1. The game's listen-host rotation (mp.ini
`rounds_per_map`) uses the same order (mp_session.c MpRotationNext).
"""
import time

DESCRIPTION = "rounds_per_map / rotation - cycle maps after N rounds"
COLOR = "#FFD24A"
CITIES = ("chicago", "havana", "vegas", "rio")
SWITCH_DELAY = 4.0          # seconds after the round's end: let the result show
now = time.monotonic

_server = None
_state = None


def default_rotation():
    """Every city's multiplayer maps, areas within a city before the next city."""
    return [{"city": c, "map_mode": "mp", "arena": a, "area": -1}
            for c in range(len(CITIES)) for a in (0, 1)]


def parse_stop(stop):
    """A stop from config -> {"city", "map_mode", "arena", "area"}. ValueError."""
    if isinstance(stop, dict):
        city = stop.get("city", 0)
        if isinstance(city, str) and not city.isdigit():
            city = CITIES.index(city.strip().lower()) if city.strip().lower() in CITIES else -1
        city = int(city)
        mode = str(stop.get("map_mode", "mp")).strip().lower()
        arena = int(stop.get("arena", 0))
        area = int(stop.get("area", -1))
    else:
        words = str(stop).strip().lower().split()
        if not words:
            raise ValueError("empty stop")
        w = words[0]
        city = int(w) if w.isdigit() else (CITIES.index(w) if w in CITIES else -1)
        mode = words[1] if len(words) > 1 else "mp"
        arena = int(words[2]) if len(words) > 2 else 0
        area = -1
    if not 0 <= city < len(CITIES):
        raise ValueError("unknown city in stop %r" % (stop,))
    if mode not in ("mp", "city"):
        raise ValueError("map mode is mp or city in stop %r" % (stop,))
    if arena not in (0, 1):
        raise ValueError("arena is 0 or 1 in stop %r" % (stop,))
    if mode == "city":
        arena = 0
    return {"city": city, "map_mode": mode, "arena": arena, "area": area}


def stops_from(cfg_value):
    if not cfg_value:
        return default_rotation()
    out = []
    for s in cfg_value:
        out.append(parse_stop(s))
    return out or default_rotation()


def next_stop(stops, city, mode, arena):
    """The stop after the current level; the first stop when the current one is
    not in the list."""
    cur = None
    for i, s in enumerate(stops):
        if s["city"] == city and s["map_mode"] == mode and (mode == "city" or s["arena"] == arena):
            cur = i
            break
    return stops[0] if cur is None else stops[(cur + 1) % len(stops)]


def _where(s):
    if s["map_mode"] == "city":
        return "%s city" % CITIES[s["city"]].upper()
    return "%s mp %d" % (CITIES[s["city"]].upper(), s["arena"])


def _current():
    return (int(_server.config_get("city", 0)), str(_server.config_get("map_mode", "mp")),
            int(_server.config_get("arena", 0)))


def on_load(server):
    global _server, _state
    _server = server
    _state = {"level": None, "seen": None, "count": 0, "switch_at": None, "to": None}


def _rounds_per_map():
    try:
        return max(0, int(_server.config_get("rounds_per_map", 0)))
    except (TypeError, ValueError):
        return 0


def on_tick():
    n = _rounds_per_map()
    level = _current()
    if level != _state["level"]:            # a vote, an admin, or our own switch
        _state.update(level=level, count=0, switch_at=None, to=None)
    number, ended = _server.round_info()
    if ended and number != _state["seen"]:
        _state["seen"] = number
        _state["count"] += 1
        if n > 0 and _state["count"] >= n and _server.players():
            try:
                stops = stops_from(_server.config_get("rotation", None))
            except ValueError as e:
                _server.log.warning("rotation: %s - using the default order" % e)
                stops = default_rotation()
            to = next_stop(stops, *level)
            if (to["city"], to["map_mode"], to["arena"]) != level:
                _state["to"] = to
                _state["switch_at"] = now() + SWITCH_DELAY
                _server.broadcast("%d round%s on this map - next: %s"
                                  % (n, "" if n == 1 else "s", _where(to)), COLOR)
    if _state["switch_at"] is not None and now() >= _state["switch_at"]:
        to = _state["to"]
        _state.update(switch_at=None, to=None)
        _switch(to)


def _switch(to):
    where = _where(to)
    _server.log.info("rotation: -> city %d, mode %s, arena %d (%s); not saved"
                     % (to["city"], to["map_mode"], to["arena"], where))
    _server.set_city(to["city"])
    _server.set_map_mode(to["map_mode"])
    if to["map_mode"] == "mp":
        _server.set_arena(to["arena"])
    for p in _server.players():
        _server.kick(p, "map rotation: %s - rejoin" % where, by="rotation",
                     announce=False, rejoin=True)
