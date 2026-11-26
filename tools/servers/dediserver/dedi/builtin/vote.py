"""Vote for a map: !vote <city> [mp [0-3]].

  !vote <city>           the city's full single-player city map
  !vote <city> mp        the city's multiplayer map, keeping the current arena
  !vote <city> mp 0-3    explicitly select a multiplayer arena (2/3: Vegas only)
  !vote mp [0-3]         vote for a multiplayer map in the current city
  !vote                  show the vote in progress
  !vote cancel           admins: stop the vote

`!vote city <city> ...` is still accepted (the `city` word is optional). On
passage, the map mode, city and arena are saved and advertised; connected
players reconnect because the game cannot change levels under a live session.
"""
import time

DESCRIPTION = "!vote <city> [mp [0-3]] - vote for the city or its multiplayer map"
COLOR = "#FFD24A"
CITIES = ("chicago", "havana", "vegas", "rio")
from dedi.mapnames import where as map_where
ALIASES = {"las vegas": 2, "lasvegas": 2, "las_vegas": 2}
USAGE = "!vote <city> [mp [0-3]]"
START_HINT = USAGE + " (or !vote mp [0-3]) starts one"
DEFAULT_COOLDOWN_S = 15
now = time.monotonic

_server = None
_vote = None  # {"city", "mode", "arena", "until", "yes"}
_cooldown_until = 0.0


def on_load(server):
    global _server, _vote, _cooldown_until
    _server = server
    _vote = None
    _cooldown_until = 0.0
    server.register_command(
        "vote", cmd_vote, console=True,
        help="!vote <city> [mp [0-3]]: a city name is the single-player map, mp = multiplayer; "
             "!vote mp [0-3] changes the current city's multiplayer map; admins: !vote cancel")
    server.register_command(
        "endvote", cmd_endvote, admin=True, console=True,
        help="!endvote - cancel the active map vote")
    server.register_command(
        "map", cmd_map, admin=True, console=True,
        help="!map <city> [mp [0-3]] | !map mp [0-3] [save] - switch map immediately")


def city_index(text):
    t = str(text).strip().lower()
    if t.isdigit() and 0 <= int(t) <= 3:
        return int(t)
    if t in CITIES:
        return CITIES.index(t)
    return ALIASES.get(t, -1)


def _key(player):
    return player.player_id or ("slot%d" % player.id)


def _cfg_float(key, default):
    try:
        return float(_server.config_get(key, default))
    except (TypeError, ValueError):
        return float(default)


def _where(city, mode, arena):
    if mode == "city":
        return "%s city (single-player)" % CITIES[city].upper()
    return map_where(city, "mp", arena)


def _mp_value(words):
    if not words:
        return None
    if words[0] not in ("0", "1", "2", "3"):
        raise ValueError("mp is 0 or 1 (Vegas: 0-3), not '%s'" % words[0])
    if len(words) > 1:
        raise ValueError("unknown option '%s' -- %s" % (" ".join(words[1:]), USAGE))
    return int(words[0])


def parse_target(text):
    """Return (city, mode, arena); None means keep the current value.

    `<city>` targets the full city; appending `mp` targets multiplayer.
    `mp [0-3]` targets multiplayer in the current city.
    The optional `city` keyword (`!vote city <city>`) is still accepted.
    """
    words = str(text).strip().lower().split()
    if not words:
        raise ValueError("usage: %s" % USAGE)

    if words[0] == "mp":
        return None, "mp", _mp_value(words[1:])

    if words[0] == "city":
        if len(words) < 2:
            raise ValueError("usage: !vote <city> [mp [0-3]]")
        name, rest = words[1], words[2:]
    else:
        name, rest = words[0], words[1:]  # !vote <city> [mp ...]

    city = city_index(name)
    if city < 0:
        raise ValueError("unknown city '%s' -- 0-3 or %s" % (name, "/".join(CITIES)))

    if not rest:
        return city, "city", None
    if rest[0] != "mp":
        raise ValueError("unknown option '%s' -- %s" % (rest[0], USAGE))
    return city, "mp", _mp_value(rest[1:])


def _needed():
    return int(len(_server.players()) * _cfg_float("vote_threshold", 0.5)) + 1


def _tally():
    return "%s: %d/%d yes" % (
        _where(_vote["city"], _vote["mode"], _vote["arena"]),
        len(_vote["yes"]), _needed())


def _agree(city, mode, arena):
    if mode == "city":
        return "!vote %s" % CITIES[city]
    return "!vote %s mp %d" % (CITIES[city], arena)


def cmd_vote(caller, args):
    global _vote, _cooldown_until
    if not _server.config_get("vote", True):
        return "voting is off on this server"
    arg = (args or "").strip()
    t = now()

    if arg.lower() == "cancel":
        return _cancel_vote(caller)

    if not arg:
        return ("vote in progress -- %s, %ds left" % (_tally(), max(0, int(_vote["until"] - t)))
                if _vote else "no vote in progress; %s" % START_HINT)

    if getattr(caller, "conn", None) is None:
        return ("the console cannot vote; set \"city\", \"map_mode\" and \"arena\" "
                "in dediserver.json instead")

    try:
        city, mode, arena = parse_target(arg)
    except ValueError as e:
        return str(e)

    cur_city = int(_server.config_get("city", 0))
    cur_arena = int(_server.config_get("arena", 0))
    cur_mode = str(_server.config_get("map_mode", "mp"))
    city = cur_city if city is None else city
    arena = (cur_arena if arena is None else arena) if mode == "mp" else cur_arena

    if mode == "mp" and not _server.arena_ok(city, arena):
        if arena >= (4 if city == 2 else 2):
            return "%s has only mp 0 and mp 1" % CITIES[city].upper()
        return "not everyone has %s installed (BAKE_D1_CARS builds Vegas MP 2/3)" % _where(city, mode, arena)

    if _vote is None:
        if t < _cooldown_until:
            return "next vote in %ds" % int(_cooldown_until - t + 0.999)
        if (city, mode, arena) == (cur_city, cur_mode, cur_arena):
            return "the server is already on %s" % _where(city, mode, arena)
        _vote = {"city": city, "mode": mode, "arena": arena,
                 "until": t + _cfg_float("vote_duration", 30), "yes": set()}
        _server.broadcast("%s started a vote to switch to %s -- type %s to agree (%ds)"
                          % (caller.name, _where(city, mode, arena), _agree(city, mode, arena),
                             int(_cfg_float("vote_duration", 30))), COLOR)
    elif (city, mode, arena) != (_vote["city"], _vote["mode"], _vote["arena"]):
        return "a vote for %s is running; wait for it to end" % _where(
            _vote["city"], _vote["mode"], _vote["arena"])

    key = _key(caller)
    if key in _vote["yes"]:
        return "you already voted (%s)" % _tally()
    _vote["yes"].add(key)
    _server.broadcast("%s voted -- %s" % (caller.name, _tally()), COLOR)
    _check()
    return None


def _check():
    global _vote, _cooldown_until
    if _vote is None:
        return
    if len(_vote["yes"]) >= _needed():
        city, mode, arena = _vote["city"], _vote["mode"], _vote["arena"]
        _vote = None
        _cooldown_until = now() + _cfg_float("vote_cooldown", DEFAULT_COOLDOWN_S)
        _pass(city, mode, arena)
    elif now() >= _vote["until"]:
        _server.broadcast("Vote for %s failed -- %s"
                          % (_where(_vote["city"], _vote["mode"], _vote["arena"]), _tally()), COLOR)
        _vote = None
        _cooldown_until = now() + _cfg_float("vote_cooldown", DEFAULT_COOLDOWN_S)


def _cancel_vote(caller):
    global _vote, _cooldown_until
    if not caller.admin:
        return "only an admin can cancel a vote"
    if _vote is None:
        return "no vote in progress"
    _server.broadcast("Vote for %s cancelled by an admin"
                      % _where(_vote["city"], _vote["mode"], _vote["arena"]), COLOR)
    who = _server.describe(caller) if getattr(caller, "conn", None) is not None else "the console"
    _server.log.info("vote cancelled by %s" % who)
    _vote = None
    _cooldown_until = now() + _cfg_float("vote_cooldown", DEFAULT_COOLDOWN_S)
    return "vote cancelled" if getattr(caller, "conn", None) is None else None


def cmd_endvote(caller, _args):
    return _cancel_vote(caller)


def _switch(city, mode, arena, by, announce, save=True):
    where = _where(city, mode, arena)
    _server.broadcast(announce % where, COLOR)
    _server.log.info("%s: city -> %d, mode -> %s, arena -> %d (%s)"
                     % (by, city, mode, arena, where))
    _server.set_city(city)
    _server.set_map_mode(mode)
    if mode == "mp":
        _server.set_arena(arena)
    if save:
        try:
            _server.save_config("city", "map_mode", "arena")
        except OSError as e:
            _server.log.warning("could not save the map: %s" % e)
    for p in _server.players():
        _server.kick(p, "server switched to %s - rejoin" % where, by=by,
                     announce=False, rejoin=True)
    return where


def _pass(city, mode, arena):
    _switch(city, mode, arena, by="vote",
            announce="Vote passed: switching to %s -- reconnecting everyone", save=True)


def cmd_map(caller, args):
    global _vote
    words = [w for w in (args or "").split() if w]
    cur_city = int(_server.config_get("city", 0))
    cur_arena = int(_server.config_get("arena", 0))
    cur_mode = str(_server.config_get("map_mode", "mp"))
    if not words:
        return ("current map: %s -- usage: !map <city> [mp [0-3]] | !map mp [0-3] [save]"
                % _where(cur_city, cur_mode, cur_arena))
    save = words[-1].lower() == "save"
    if save:
        words = words[:-1]
    if not words:
        return "usage: !map <city> [mp [0-3]] | !map mp [0-3] [save]"
    try:
        city, mode, arena = parse_target(" ".join(words))
    except ValueError as e:
        return str(e)
    city = cur_city if city is None else city
    arena = (cur_arena if arena is None else arena) if mode == "mp" else cur_arena
    if mode == "mp" and not _server.arena_ok(city, arena):
        if arena >= (4 if city == 2 else 2):
            return "%s has only mp 0 and mp 1" % CITIES[city].upper()
        return "not everyone has %s installed (BAKE_D1_CARS builds Vegas MP 2/3)" % _where(city, mode, arena)
    if (city, mode, arena) == (cur_city, cur_mode, cur_arena):
        return "the server is already on %s" % _where(city, mode, arena)
    _vote = None
    is_console = getattr(caller, "conn", None) is None
    who = "console" if is_console else ("admin %s" % caller.name)
    where = _switch(city, mode, arena, by=who,
                    announce="Map changed to %%s by %s -- reconnecting everyone" % who,
                    save=save)
    return "switched to %s%s" % (where, " (saved)" if save else "")


def on_tick():
    _check()


def on_player_leave(player):
    if _vote is not None:
        _vote["yes"].discard(_key(player))
