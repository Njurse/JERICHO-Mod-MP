# dediserver — a headless dedicated server for the mp mod

A real multiplayer session with **no game window**. It accepts players, hands out
ids, relays car-state and chat, **broadcasts the roster** (so every client sees who
else is in the match, on the cadence a real host uses), answers `PING` with `PONG`,
and beacons on UDP so the in-game LAN browser lists it. Use it to leave a host up for
a while, or to give a client something to join on a machine that has no game build at
all — the server is pure Python and never loads the game.

    JERICHO\MODS\mp\tools\mp_dedi.bat                 # from a checkout, the quick way
    python JERICHO/MODS/mp/tools/servers/dediserver/dediserver.py

Then join it from anywhere:

    JERICHO_dev.exe -nointro -nofmv -join 127.0.0.1
    JERICHO_dev.exe -nointro -nofmv -join 127.0.0.1:1400

## Options

```
--config FILE        JSON config (default: dediserver.json beside this script)
--port N             TCP session port (default 1318)
--name "..."         name shown in the browser / registered with the master
--city N             GameLevel the clients should load (0..3)
--gamemode N
--max-players N      slots 1..N-1 are given to clients (0 belongs to the server)
--beacon / --no-beacon
--beacon-ms N        beacon cadence (default 1000)
--idle-drop S        seconds of real silence that drop a client (0 = never)
--log-level debug|info|warning|error
--log-file FILE      also append logs to FILE
--master URL         register with a master discovery service; http://host:8100, or a
                     bare host (the port defaults to the master's 8100)
--master-name "..."  name to register (default: --name)
--master-interval S  heartbeat cadence (default 20; keep well under the master's ttl)
--advertise-host H   the address PLAYERS reach us on, if that is not the address the
                     master sees. Behind NAT this is your public IP or hostname; the
                     listing is unusable without it
--advertise-port N   the public port players dial, when the router REMAPS ours
                     (0 = --port). Only needed when the forward is not 1:1
```

CLI flags override the config file. The config file is optional — with none, the
built-in defaults are used.

## `dediserver.json`

```jsonc
{
  "port": 1318,
  "name": "Dedicated",
  "city": 0,
  "gamemode": 0,
  "max_players": 8,
  "beacon": true,
  "beacon_ms": 1000,
  "idle_drop_s": 30.0,      // 0 = never drop a silent client
  "live": true,             // advertise as a live match (the level is always up)
  "log_level": "info",
  "log_file": "",
  "master": "http://jerichomp.animetiddi.es:8100",  // "" disables registration.
                            // A bare host works too ("jerichomp.animetiddi.es") and
                            // means the master's own port, 8100 -- not 80.
  "master_name": "",
  "advertise_host": "",
  "advertise_port": 0,
  "master_interval_s": 20.0,
  "master_timeout_s": 5.0
}
```

## Docker

On a server or VPS, [`../docker-compose.yml`](../docker-compose.yml) runs this and
the master together (`docker compose up -d --build`), with `--port`, `--name`,
`--master` and `--advertise-host` set from the environment. See
[`../README.md`](../README.md#running-it-on-a-server-docker).

## Ports

| Port | Protocol | Direction | Needed for |
| --- | --- | --- | --- |
| `port` (1318) | TCP | **inbound** | the session itself — every client connects here |
| `port` (1318) | UDP | inbound + outbound | the LAN discovery beacon, so the in-game browser lists the server |
| master port (8100) | TCP | **outbound** | only with `--master`: registering for the internet listing |

* **LAN play:** open nothing. The beacon and the session both work on the local
  network as-is.
* **Internet play (direct connect):** forward **TCP 1318** to the server. Add
  **UDP 1318** only if you also want LAN clients on the server's own network to
  find it in the browser; it is not needed for internet clients.
* **Internet listing:** with `--master`, the server makes an outbound TCP
  connection to the master, so no inbound port for that. Players reach it from
  the game's **Multiplayer -> Online** screen, which lists the master's servers
  once `master_url` is set in `mp.ini`.
  * **Behind NAT, set `--advertise-host`** (your public IP or hostname). The
    master records the address it sees the registration come FROM, which behind
    NAT is the server's private address -- the listing would name somewhere no
    player can reach. This does not affect LAN play: the beacon's address is
    taken from the packet, not from the listing.
  * Forward the **session port (TCP)** on the same address. `advertise_host` says
    WHERE to connect. The port in the listing is `--port`, unless the router hands
    out a different public port -- then `--advertise-port` says what players dial.

## Running it for a long time

* **It is self-contained.** No game, no assets, no display. Run it on any machine
  with Python 3.
* **Log to a file.** Set `log_file` (or `--log-file`) so a dropped client is
  still legible in the morning; the file is appended, never truncated.
* **`--idle-drop 0`** if you would rather never drop a quiet client than drop a
  client that is merely paused or loading. The default (30 s) matches the game's
  own idle timeout.
* **Restart on crash** with whatever your platform offers (a service unit, a
  `while true` loop, Task Scheduler). It is stateless: a restart is just a fresh
  session. If it registered with a master, it re-registers on start.
* **A clean stop unregisters.** `Ctrl-C`, `SIGTERM` or Windows `Ctrl-Break` closes
  the sockets and removes the server from the master's listing at once, instead of
  leaving it to lapse.

## Reading the log

Every line is timestamped and levelled. The ones that matter:

```
[INFO] listening on TCP/1318 (city 0, up to 8 players); Ctrl-C to stop
[INFO] player 1 'Bob' joined from 192.168.1.44:53120 (mods=2, build 0x74cc)
[INFO] player 1 'Bob' left (0/8)
[WARNING] player 1 'Bob' timed out (30s silent)
[WARNING] refused 192.168.1.44:53122: server is full (8/8)
[INFO] registered with master http://master.example.com:8100 as 203.0.113.5:1318
[WARNING] master ... unreachable (...); retrying in 4s
```

A client that **never** appears has a transport problem (firewall, wrong address).
A client that joins and then leaves on its own has a client-side problem. That
distinction is the whole point of logging joins and leaves.

## Liveness

The server answers a client's `PING` with a `PONG` echoing the tick, and pings on
its own timer so a quiet client is never given up just for being quiet. A client
that gets nothing after the handshake drops itself at its idle timeout; the server
does the same after `idle_drop_s`.

## Testing

`../rigs/test_e2e.py` starts this server and the master together on loopback and
checks registration, heartbeat, a client handshake, re-registration after a
master restart, and unregister-on-shutdown — no game, no assets:

```sh
python JERICHO/MODS/mp/tools/servers/rigs/test_e2e.py
```
