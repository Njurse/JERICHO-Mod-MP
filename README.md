# Multiplayer (mp)

LAN multiplayer for REDRIVER2, built as a JERICHO deep mod (compiled into the
game like `levelhacks`, so it can read/write game globals). Host on TCP/UDP
**1318** (configurable), join over LAN with UDP discovery, and play a shared
world. The first gamemode is **Take a Ride**: the host picks city, time of day
and weather, and players spawn into the same level.

## How it looks

The menus are **real frontend screens** (not an overlay): the main-menu
**Multiplayer** entry opens a native menu tree, rendered and navigated by the
engine through the JERICHO frontend-menu API (`jer_frontend.h`):

```
Multiplayer
  Host Game ......... gamemode -> city / time / weather / enforce-mods -> Start
  Join Game ......... LAN server list / manual IP
  Options ........... Change Name / Enforce Mods / Port
  Local Split-Screen  (returns to the stock 2-pad flow)
```

`Enforce Mods` is the host's lobby setting (Off / By ID / By ID+Version): the
join handshake then admits or refuses clients by their enabled-mod manifest.

## How it works

- **Transport** — non-blocking TCP session + UDP discovery (`mp_net.c`).
- **Handshake** — client sends `MP_HELLO` (identity + mod manifest); the host
  validates version and applies the mod-match policy, answering `MP_WELCOME`
  or `MP_REJECT`.
- **Synchronized start** — the host broadcasts `MP_START` with the session
  config; every machine runs `SetState(STATE_GAMESTART)` with the host's city,
  time and weather. The host adds one `PlayerStartInfo` slot per remote player
  (`JER_EVENT_NET_SPAWN`), so everyone has a car.
- **Deterministic input lockstep** — each machine contributes its pad per
  frame; the host gathers and broadcasts the full input set
  (`MP_INPUT`); every player car is driven from it (`JER_EVENT_NET_INPUT`).
- **State-resync fallback** — the host periodically broadcasts player-car
  transforms (`MP_CARSTATE`); a client whose car diverges beyond a threshold
  snaps it to the host's state.
- **The host owns the roster** — `MP_ROSTER` publishes who is in the match,
  ascending player id (host first), with each player's name, vehicle and ping.
  It goes out *before* a launch so every machine knows how many cars to spawn
  before its level loads, and is refreshed every couple of seconds. Car slots are
  handed out walking player ids, because that is the only ordering both machines
  agree on -- walking a per-machine registry let two sides put the same players in
  opposite slots, and a client that did not yet know about the host added no car
  at all and left a level AI car (a cop) in the remote player's slot.
- **Ping** — PONG echoes the tick from PING, so the host holds a round trip per
  peer and publishes it; that is the number in the pause menu list.
- **The frontend's idle demo is suppressed** while a session or lobby exists
  (`JER_EVENT_FRONTEND_IDLE`). A host waiting for players is idle by definition,
  and the demo's level load blocks the main thread, which used to drop everyone
  who was joining. `mp_host.bat` therefore sets `MP_AUTOSTART=host`: with the demo
  gone, nothing starts a match unless the host says so.
- **Addon network bridge** (`jer_net.h`) — any module can send named
  reliable / latest-wins channels over the session (`jer_net_send` /
  `JER_EVENT_NET_RECV`).

## Files

| File | Role |
|---|---|
| `mp.c` | the module's engine-facing entry point and hooks |
| `mp_net.c` | sockets: TCP session + UDP discovery, framing, per-peer ping |
| `mp_session.c` | handshake, start, input replication, resync, roster, dispatch |
| `mp_players.c` | the player registry and the car accessors built on it |
| `mp_config.c` | config, module identity, build/manifest hashes |
| `mp_map.c` | the multiplayer-map blips (`JER_EVENT_DRAW_MAP`) |
| `mp_bridge.c` | the `jer_net.h` addon bridge |
| `mp_ui.c` | the frontend menus (registered via `jer_frontend.h`) |
| `mp_proto.h` | the wire protocol (framed, little-endian) |
| `tools/` | launchers + harnesses, documented in `tools/README.md` |

## In game

While the pause menu is open, the players are listed down the left of the screen:
the host first and in cyan, then each player's name, index, the vehicle they are
in (`-1` = on foot) and their ping. The order is the roster's, so it reads the way
the match was built. `MP_PAUSE=1` holds the pause menu open for testing and
`MP_DEBUG` logs each row.

## Config (`JERICHO/CONFIG/mp.ini`)

| Key | Default | Meaning |
|---|---|---|
| `port` | `1318` | TCP session port. Discovery always uses a fixed UDP 1318 and advertises the session port in the beacon, so changing this does not hide your game |
| `player_name` | OS username | your name on the network |
| `host_name` | `<name>'s game` | the advertised server name |
| `beacon_ms` | `1000` | discovery beacon interval |
| `mod_check` | `0` | host lobby mod policy: 0 off, 1 by id, 2 by id+version |
| `strict_version` | `0` | host lobby setting: 1 also requires an identical build hash (off by default, because that hash tracks `git describe`) |

## Command line

`-help` (also `-h`, `--help`, `-?`) prints the full argument list **to the
terminal and the log**, then exits -- no window, no pop-up. It is handled before
any engine init, so it is instant.

Unknown arguments no longer dump the list or open a modal dialog: they raise a
toast in the frontend (`jer_error`, gentle red, left side, ~5 s) and the game
carries on.

    REDRIVER2_dev.exe -help
    REDRIVER2_dev.exe -host 1318
    REDRIVER2_dev.exe -join 192.168.1.20:1318

## If nobody can see your game

Two things have to be true, and the module only controls one of them.

1. **Windows Firewall.** Inbound **UDP 1318** (discovery) and **TCP 1318** (the
   session) must be allowed on the host. The module cannot create that rule,
   so if you never see another machine's game, check the firewall first.
2. **Discovery actually running.** If the discovery socket cannot be opened
   or its port is taken, the module now says so instead of failing quietly:

       [mp] discovery unavailable: the port is already taken by another program (UDP/1318)
       [error] LAN discovery is off: the port is already taken by another program (UDP/1318)

   A *second copy of this game* sharing the port is fine (both can still see
   each other); a different program holding it is not.

`-join <ip>[:port]` still works with discovery off, so a host can always be
reached by address.

## Testing

`tools/README.md` documents the whole launcher/harness set in `tools/` and says
which one to reach for; the short version is `tools/mp_pair.bat` (two real
instances on one PC), `mp_host.bat` / `mp_join.bat` (two machines), and
`mp_mock_host.bat` / `mp_dedi.bat` (no second engine).

`tools/mp_localpair.py` runs **two real instances on one PC** — one hosting, one
joining — and prints both sides' logs. Each instance gets its own working
directory (built from junctions, so nothing is copied) which is what keeps the
two `REDRIVER2.log` files and the two `mp.ini` files apart. It launches the
executables directly and kills exactly the PIDs it started.

    python tools/mp_localpair.py                # host + join, report, clean up
    python tools/mp_localpair.py --keep         # leave the run dirs to poke at
    python tools/mp_localpair.py --clean        # remove them again

Use it for anything that needs two real engines: a car that never appears, a mode
that launches wrong on one side, a connection that drops. `--clean` (and the
automatic cleanup) unlinks the junctions before deleting anything, so it can
never follow one into the real game tree.

`tools/mp_test.py` is a headless protocol harness (the game writes its log
relative to the CWD, so two live instances cannot share a folder). It drives
the real module against a scriptable peer:

```
# game as host (MP_AUTOSTART=host) <-> python mock client
python tools/mp_test.py client 127.0.0.1 --port 1318 --build 0x<buildhash> --channel demo
# python mock host <-> game as client (MP_AUTOSTART=join:127.0.0.1), start + lockstep + resync
python tools/mp_test.py host --port 1318 --city 1 --start --lockstep
# watch the host's discovery beacons
python tools/mp_test.py beacon --port 1318
```

`MP_AUTOSTART=host[:PORT] | join[:IP[:PORT]]` brings a session up at boot
without the frontend, and `MP_DEBUG=1` logs the bridge/session detail.

> Note: while testing, `JERICHO/CONFIG/modlist.ini` has every other module
> disabled so they cannot interfere; re-enable them afterwards.
