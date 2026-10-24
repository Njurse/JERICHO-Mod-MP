"""vote -- !vote city <city> [mp 0|1] / !vote mp 0|1: players vote the server
onto another map.

  !vote city <city> [mp 0|1]
                  start a vote for that city and, optionally, one of its two
                  multiplayer maps (city 0..3 or chicago / havana / vegas / rio,
                  the engine's order; mp 0/1 is the game's own -mp 0 / -mp 1, and
                  0 is the take-a-ride default map)
  !vote mp 0|1    start a vote to change only the multiplayer map, same city
  !vote           show the vote in progress
  !vote cancel    admins: stop the vote

`!vote <city>` without the word `city` still works (the old form). One vote per
player id (the slot for an old client with no id). A vote passes as soon as MORE
than `vote_threshold` (default 0.5) of the players connected have voted yes; it
fails after `vote_duration` seconds (default 30). Another vote can start
`vote_cooldown` seconds (default 60) after one ends. "vote": false in
dediserver.json switches the command off.

On a pass the city and the multiplayer map are saved to dediserver.json and become
the session settings (the welcome and start messages carry them, and the listing is
refreshed), and every player is kicked with the KICK rejoin flag: the game has no
way to change level under a live session, so current clients reconnect by
themselves (keeping their car) and older ones rejoin from the server browser into
the new map.
"""
import time

DESCRIPTION = "!vote city <city> [mp 0|1] - players vote the server onto another map"
COLOR = "#FFD24A"
CITIES = ("chicago", "havana", "vegas", "rio")
ALIASES = {"las vegas": 2, "lasvegas": 2, "las_vegas": 2}
USAGE = "!vote city <city> [mp 0|1]"
START_HINT = USAGE + " (or !vote mp 0|1) starts one"
now = time.monotonic          # tests replace this

_server = None
_vote = None                  # {"city", "arena", "until", "yes": set(voter keys)}
_cooldown_until = 0.0


def on_load(server):
    global _server, _vote, _cooldown_until
    _server = server
    _vote = None
    _cooldown_until = 0.0
    server.register_command("vote", cmd_vote,
                            help="!vote city <city> [mp 0|1] - vote to switch city (0-3 or "
                                 "chicago/havana/vegas/rio) and/or one of its two multiplayer "
                                 "maps; !vote mp 0|1 changes only the map; admins: !vote cancel")


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


def _where(city, arena):
    """A map the way the commands name it: "HAVANA mp 1"."""
    return "%s mp %d" % (CITIES[city].upper(), arena)


def _mp_value(words):
    if not words:
        raise ValueError("mp needs a value: 0 or 1 (the city's two multiplayer maps)")
    if words[0] not in ("0", "1"):
        raise ValueError("mp is 0 or 1 (the city's two multiplayer maps), not '%s'" % words[0])
    if len(words) > 1:
        raise ValueError("unknown option '%s' -- %s" % (" ".join(words[1:]), USAGE))
    return int(words[0])


def parse_target(text):
    """(city, arena) from a vote line; either is None when the caller did not name
    it ("keep the current one"). Raises ValueError with a player-facing message
    for anything that is not `city <city> [mp 0|1]`, `<city> [mp 0|1]` or
    `mp 0|1`."""
    words = str(text).strip().lower().split()
    if not words:
        raise ValueError("usage: %s" % USAGE)
    if words[0] == "mp":
        return None, _mp_value(words[1:])

    if words[0] == "city":
        if len(words) < 2:
            raise ValueError("usage: !vote city <city> [mp 0|1]")
        name, rest = words[1], words[2:]
    else:
        name, rest = words[0], words[1:]          # the old `!vote <city>` form

    city = city_index(name)
    if city < 0:
        raise ValueError("unknown city '%s' -- 0-3 or %s" % (name, "/".join(CITIES)))

    arena = None
    if rest:
        if rest[0] != "mp":
            raise ValueError("unknown option '%s' -- %s" % (rest[0], USAGE))
        arena = _mp_value(rest[1:])
    return city, arena


def _needed():
    """Yes votes needed: more than threshold x the players connected."""
    n = len(_server.players())
    frac = _cfg_float("vote_threshold", 0.5)
    return int(n * frac) + 1


def _tally():
    return "%s: %d/%d yes" % (_where(_vote["city"], _vote["arena"]),
                              len(_vote["yes"]), _needed())


def _agree(city, arena):
    return "!vote city %s mp %d" % (CITIES[city], arena)


def cmd_vote(caller, args):
    global _vote, _cooldown_until
    if not _server.config_get("vote", True):
        return "voting is off on this server"
    arg = (args or "").strip()
    t = now()

    if arg.lower() == "cancel":
        if not caller.admin:
            return "only an admin can cancel a vote"
        if _vote is None:
            return "no vote in progress"
        _server.broadcast("Vote for %s cancelled by an admin"
                          % _where(_vote["city"], _vote["arena"]), COLOR)
        who = _server.describe(caller) if getattr(caller, "conn", None) is not None else "the console"
        _server.log.info("vote cancelled by %s" % who)
        _vote = None
        _cooldown_until = t + _cfg_float("vote_cooldown", 60)
        return None

    if not arg:
        return ("vote in progress -- %s, %ds left" % (_tally(), max(0, int(_vote["until"] - t)))
                if _vote else "no vote in progress; %s" % START_HINT)

    if getattr(caller, "conn", None) is None:
        return ("the console cannot vote; set \"city\" and \"arena\" in "
                "dediserver.json instead")

    try:
        city, arena = parse_target(arg)
    except ValueError as e:
        return str(e)

    # An argument the caller did not name keeps its current value, so the vote's
    # target is always a whole map (city + arena) even for `!vote mp 1`.
    cur_city = int(_server.config_get("city", 0))
    cur_arena = int(_server.config_get("arena", 0))
    city = cur_city if city is None else city
    arena = cur_arena if arena is None else arena

    if _vote is None:
        if t < _cooldown_until:
            return "next vote in %ds" % int(_cooldown_until - t + 0.999)
        if (city, arena) == (cur_city, cur_arena):
            return "the server is already on %s" % _where(city, arena)
        _vote = {"city": city, "arena": arena,
                 "until": t + _cfg_float("vote_duration", 30), "yes": set()}
        _server.broadcast("%s started a vote to switch to %s -- type %s to agree (%ds)"
                          % (caller.name, _where(city, arena), _agree(city, arena),
                             int(_cfg_float("vote_duration", 30))),
                          COLOR)
    elif (city, arena) != (_vote["city"], _vote["arena"]):
        return "a vote for %s is running; wait for it to end" % _where(_vote["city"], _vote["arena"])

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
        city, arena = _vote["city"], _vote["arena"]
        _vote = None
        _cooldown_until = now() + _cfg_float("vote_cooldown", 60)
        _pass(city, arena)
    elif now() >= _vote["until"]:
        _server.broadcast("Vote for %s failed -- %s"
                          % (_where(_vote["city"], _vote["arena"]), _tally()), COLOR)
        _vote = None
        _cooldown_until = now() + _cfg_float("vote_cooldown", 60)


def _pass(city, arena):
    where = _where(city, arena)
    _server.broadcast("Vote passed: switching to %s -- reconnecting everyone" % where, COLOR)
    _server.log.info("vote passed: city -> %d, arena -> %d (%s)" % (city, arena, where))
    _server.set_city(city)
    _server.set_arena(arena)
    try:
        _server.save_config("city", "arena")
    except OSError as e:
        _server.log.warning("could not save the map: %s" % e)
    for p in _server.players():
        _server.kick(p, "server switched to %s - rejoin" % where, by="vote", announce=False,
                         rejoin=True)


def on_tick():
    _check()


def on_player_leave(player):
    # a leaver's vote no longer counts toward a majority of those still here
    if _vote is not None:
        _vote["yes"].discard(_key(player))
