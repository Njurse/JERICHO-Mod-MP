# mp server-side tools

Two things live here, and they are **not** the same kind of thing:

| Folder | What it is | Who runs it | Part of the mp mod? |
| --- | --- | --- | --- |
| [`dediserver/`](dediserver/README.md) | a headless **dedicated game server** — a real mp session with no game window | a player who wants to leave a host up | **yes** — a dev/self-host tool for the mod |
| [`masterserver/`](masterserver/README.md) | the **master discovery service** — a registry that internet hosts register with and clients list from | whoever operates the public listing service | **no — a separate project** |

> **The master server is a separate project, not part of the mp mod download.**
> It is a network *service*, not game content. It lives under this folder only
> because that is where the mp tooling is and because a checkout of it here makes
> it immediately testable. If you came for the multiplayer mod, you do not want
> [`masterserver/`](masterserver/README.md) — you want the game plus
> [`dediserver/`](dediserver/README.md) (and, honestly, usually just the in-game
> Host button). See [`masterserver/README.md`](masterserver/README.md) for the
> "you probably don't need this" note and the plan to extract it into its own
> repository.

## Why these are safe by placement

Both live under `JERICHO/MODS/mp/tools/`, and **`tools/` is deliberately never
mirrored into a build or a package.** The premake post-build step mirrors each
mod folder into `bin/<cfg>/JERICHO/MODS/` (that is what a LAN package is made
from) but removes every `tools/` tree on the way in, because tools are source,
not game data (`src_rebuild/premake5.lua`, "The TOOLS are deliberately NOT
mirrored"). So nothing here can ride along in a mod download by accident — which
is exactly why the discovery server is placed here rather than in `JERICHO/`.

## What is where

| Path | What it is |
| --- | --- |
| [`run.sh`](run.sh) / [`run.bat`](run.bat) | the way in: `master`, `dedi`, `stack`, `checks`, `docker` |
| [`docker-compose.yml`](docker-compose.yml) | deploy the master and the dedi together on a test server / VPS |
| [`README.md`](README.md) | this file |
| [`TESTING.md`](TESTING.md) | the rig, what it has found, and the ladder from one PC to real players |
| `masterserver/` | the discovery service: `masterserver.py` + `.json` + `Dockerfile` + README + its own test |
| `dediserver/` | the dedicated server: `dediserver.py` + `.json` + `Dockerfile` + README |
| `rigs/` | the tests and rigs that need a working tree: `local_stack.py` (master + dedi + N real clients), `test_e2e.py` (the no-game end-to-end) |
| `proto_check.py` | the wire-mirror guard both servers call at startup |

**`proto_check.py` stays at this level on purpose.** It resolves `mp_proto.h` and
`mp_test.py` from its own location (`<mp>/mp_proto.h`, `<mp>/tools/mp_test.py`), and
each Dockerfile copies it to exactly that relative place. Move it and the guard
silently stops finding the wire it is meant to check.

## Running it

```sh
cd JERICHO/MODS/mp/tools/servers
./run.sh master                 # the discovery registry
./run.sh dedi --name "24/7 Take a Ride" --city 0 --rules 0
./run.sh stack --clients 2      # master + dedi + two real clients on this PC
./run.sh checks                 # what CI runs for this folder
./run.sh docker                 # or `docker compose up -d --build`
```

`run.bat` takes the same verbs on Windows. The single-purpose launchers one
directory up (`mp_dedi.bat`/`.sh`, `mp_pair.bat`/`.sh`, ...) still work and are
listed in [`../README.md`](../README.md).

## Running it on a server (Docker)

A test server or a VPS runs both services from here:

```bash
cd JERICHO/MODS/mp/tools/servers
docker compose up -d --build      # then: docker compose logs -f / ps / down
```

[`docker-compose.yml`](docker-compose.yml) builds the two images
([`masterserver/Dockerfile`](masterserver/Dockerfile),
[`dediserver/Dockerfile`](dediserver/Dockerfile)) and starts them with **host
networking** — the dedi's LAN beacon is a UDP broadcast that a bridge network
would keep to itself, and host networking is simply the tested configuration. So
the ports are the host's: **open the master's TCP port** (8100) and **the dedi's
session port** (its `port` in `dediserver.json`, 1318 as shipped), plus **UDP 1318**
only if you want LAN browser discovery.

**The dedicated server is configured by `dediserver/dediserver.json`** — your own,
untracked copy of `dediserver.example.json` (create it with `cp
dediserver/dediserver.example.json dediserver/dediserver.json` before the first
`docker compose up`, or docker mounts a directory in its place) — mounted
read-only into the container, and the only things the compose passes it are
`--config` and the container log path. Its name, city, rules, max players, session
port, `master` URL, advertise host/port, beacon and log level all come from that
file. The compose used to pass those itself with defaults of its own, and command
line beats the config — so editing the JSON did nothing and the server came up
with a name and a loopback master nobody had asked for. The master keeps
`MASTER_PORT` and `LOG_LEVEL` as environment overrides, since its port is also what
the healthcheck and every player's `master_url` point at.

## Both on one VPS

The intended deployment, and what `docker compose` gives you: one box runs the
**master** (the registry, HTTP/8100) and the **dedicated server** (the relay,
TCP/1318). They do different jobs and do not conflict — different ports, different
processes, and the master never sees game traffic.

```
git pull
cd JERICHO/MODS/mp/tools/servers
docker compose up -d --build          # or: ./run.sh master &  and  ./run.sh dedi &
```

Three things that decide whether the internet can actually use it:

1. **Open both ports, in BOTH firewalls.** TCP/8100 (the master) and the dedi's
   session port — `dediserver.json`'s `port` (`1318` as shipped; the compose no
   longer overrides it). `ufw allow …` is only the
   first: a cloud provider's firewall sits *in front of* ufw and drops silently.
   UDP/1318 is only for LAN discovery.
2. **Do not point `master` at 127.0.0.1.** It is the obvious choice on one box, and
   it is wrong: the master records the address it saw you from — `127.0.0.1` — and
   lists *that* to every internet client, so the row is undialable everywhere but
   here. Use the public name (the shipped default is
   `http://jerichomp.animetiddi.es:8100`; set `master` in `dediserver.json`) or set
   `advertise_host`. The dedi warns in its log and on its
   console when it is listed under a loopback or private address, because such a
   listing looks perfectly healthy from this box.
3. **Check it from somewhere else.** The master probes each listing's session port,
   but when both run on one box that probe reaches the server over the host's own
   address — it cannot see an external firewall. From another machine:
   `python3 tools/mp_test.py client <host> --port <session port>`.

## Consoles

Both servers have an interactive command line on **stdin** — not a network admin
port: nothing listens, nothing authenticates, and the people who can use it already
have a shell on the machine. Start one in a terminal and `help` lists its commands.

With no terminal attached (a service, `docker compose up -d`, a captured run) the
console says so once and stays out of the way. `JER_CONSOLE=on` forces it on — that
is how a supervisor (or `test_console_stdin.py`) can pipe commands in:

```sh
printf 'status\nlist\nstop\n' | JER_CONSOLE=on ./run.sh dedi
```

| server | commands |
| --- | --- |
| master | `status`, `list`, `drop <token or name>`, `persist`, `stop` |
| dedicated | `status`, `list`, `say <text>`, `kick <id>`, `beacon [on or off]`, `master`, `stop` |

Both share `help`, and the implementations live in one place —
[`serverkit.py`](serverkit.py) — with the logging, config loading and signal
handling the two servers used to carry twice.

## Start here

* Running a 24/7 host → [`dediserver/README.md`](dediserver/README.md).
* Operating the public listing service → [`masterserver/README.md`](masterserver/README.md).
* **Putting both on a server** → [`docker-compose.yml`](docker-compose.yml).
* **Running and testing the whole stack on one PC** →
  [`TESTING.md`](TESTING.md) (the `local_stack.py` rig, and what it has found).
* The game-side work still needed for internet play (a listen host registering
  itself and being listed) → [`docs/MASTER_SERVER.md`](../../docs/MASTER_SERVER.md).
