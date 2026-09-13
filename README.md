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
- **Addon network bridge** (`jer_net.h`) — any module can send named
  reliable / latest-wins channels over the session (`jer_net_send` /
  `JER_EVENT_NET_RECV`).

## Files

| File | Role |
|---|---|
| `mp.c` | module entry, config, player registry, hooks |
| `mp_net.c` | sockets: TCP session + UDP discovery |
| `mp_session.c` | handshake, start, lockstep, resync, dispatch |
| `mp_bridge.c` | the `jer_net.h` addon bridge |
| `mp_ui.c` | the frontend menus (registered via `jer_frontend.h`) |
| `mp_proto.h` | the wire protocol (framed, little-endian) |

## Config (`JERICHO/CONFIG/mp.ini`)

| Key | Default | Meaning |
|---|---|---|
| `port` | `1318` | TCP+UDP port |
| `player_name` | OS username | your name on the network |
| `host_name` | `<name>'s game` | the advertised server name |
| `beacon_ms` | `1000` | discovery beacon interval |
| `mod_check` | `0` | host lobby mod policy: 0 off, 1 by id, 2 by id+version |

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

## Testing

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
