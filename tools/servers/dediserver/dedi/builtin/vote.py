"""vote -- !vote <city>: players vote the server onto another city.

  !vote <city>    start a vote for that city, or vote yes in the one running
                  (city 0..3 or chicago / havana / vegas / rio, the engine's order)
  !vote           show the vote in progress
  !vote cancel    admins: stop the vote

One vote per player id (the slot for an old client with no id). A vote passes as
soon as MORE than `vote_threshold` (default 0.5) of the players connected have
voted yes; it fails after `vote_duration` seconds (default 30). Another vote can
start `vote_cooldown` seconds (default 60) after one ends. "vote": false in
dediserver.json switches the command off.

On a pass the city is saved to dediserver.json and becomes the session city (the
welcome and start messages carry it, and the listing is refreshed), and every
player is kicked with the KICK rejoin flag: the game has no way to change level
under a live session, so current clients reconnect by themselves (keeping their
car) and older ones rejoin from the server browser into the new city.
"""
import time

DESCRIPTION = "!vote <city> - players vote the server onto another city"
COLOR = "#FFD24A"
CITIES = ("chicago", "havana", "vegas", "rio")
ALIASES = {"las vegas": 2, "lasvegas": 2, "las_vegas": 2}
now = time.monotonic          # tests replace this

_server = None
_vote = None                  # {"city", "until", "yes": set(voter keys), "by"}
_cooldown_until = 0.0


def on_load(server):
    global _server, _vote, _cooldown_until
    _server = server
    _vote = None
    _cooldown_until = 0.0
    server.register_command("vote", cmd_vote,
                            help="!vote <city> - vote to switch city (0-3 or chicago/havana/vegas/rio); "
                                 "admins: !vote cancel")


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


def _needed():
    """Yes votes needed: more than threshold x the players connected."""
    n = len(_server.players())
    frac = _cfg_float("vote_threshold", 0.5)
    return int(n * frac) + 1


def _tally():
    return "%s: %d/%d yes" % (CITIES[_vote["city"]].upper(), len(_vote["yes"]), _needed())


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
        _server.broadcast("Vote for %s cancelled by an admin" % CITIES[_vote["city"]].upper(), COLOR)
        who = _server.describe(caller) if getattr(caller, "conn", None) is not None else "the console"
        _server.log.info("vote cancelled by %s" % who)
        _vote = None
        _cooldown_until = t + _cfg_float("vote_cooldown", 60)
        return None

    if not arg:
        return ("vote in progress -- %s, %ds left" % (_tally(), max(0, int(_vote["until"] - t)))
                if _vote else "no vote in progress; !vote <city> starts one")

    if getattr(caller, "conn", None) is None:
        return "the console cannot vote; set \"city\" in dediserver.json instead"

    city = city_index(arg)
    if city < 0:
        return "unknown city '%s' -- 0-3 or %s" % (arg, "/".join(CITIES))

    if _vote is None:
        if t < _cooldown_until:
            return "next vote in %ds" % int(_cooldown_until - t + 0.999)
        if city == int(_server.config_get("city", 0)):
            return "the server is already on %s" % CITIES[city].upper()
        _vote = {"city": city, "until": t + _cfg_float("vote_duration", 30), "yes": set()}
        _server.broadcast("%s started a vote to switch to %s -- type !vote %s to agree (%ds)"
                          % (caller.name, CITIES[city].upper(), CITIES[city], int(_cfg_float("vote_duration", 30))),
                          COLOR)
    elif city != _vote["city"]:
        return "a vote for %s is running; wait for it to end" % CITIES[_vote["city"]].upper()

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
        city = _vote["city"]
        _vote = None
        _cooldown_until = now() + _cfg_float("vote_cooldown", 60)
        _pass(city)
    elif now() >= _vote["until"]:
        _server.broadcast("Vote for %s failed -- %s" % (CITIES[_vote["city"]].upper(), _tally()), COLOR)
        _vote = None
        _cooldown_until = now() + _cfg_float("vote_cooldown", 60)


def _pass(city):
    name = CITIES[city].upper()
    _server.broadcast("Vote passed: switching to %s -- reconnecting everyone" % name, COLOR)
    _server.log.info("vote passed: city -> %d (%s)" % (city, name))
    _server.set_city(city)
    try:
        _server.save_config("city")
    except OSError as e:
        _server.log.warning("could not save the city: %s" % e)
    for p in _server.players():
        _server.kick(p, "server switched to %s - rejoin" % name, by="vote", announce=False,
                         rejoin=True)


def on_tick():
    _check()


def on_player_leave(player):
    # a leaver's vote no longer counts toward a majority of those still here
    if _vote is not None:
        _vote["yes"].discard(_key(player))
