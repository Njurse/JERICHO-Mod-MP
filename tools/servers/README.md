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
the ports are the host's: **open TCP 8100** (the master), **TCP 1400** (the
session) and **UDP 1318** only if you want LAN browser discovery. `MASTER_PORT`,
`DEDI_PORT`, `DEDI_NAME`, `DEDI_CITY`, `DEDI_RULES`, `DEDI_MAX_PLAYERS`,
`ADVERTISE_HOST` and `LOG_LEVEL` are read from the environment (inline or a
`.env`), and the two JSON configs stay on the host, mounted read-only.

## Start here

* Running a 24/7 host → [`dediserver/README.md`](dediserver/README.md).
* Operating the public listing service → [`masterserver/README.md`](masterserver/README.md).
* **Putting both on a server** → [`docker-compose.yml`](docker-compose.yml).
* **Running and testing the whole stack on one PC** →
  [`TESTING.md`](TESTING.md) (the `local_stack.py` rig, and what it has found).
* The game-side work still needed for internet play (a listen host registering
  itself and being listed) → [`docs/MASTER_SERVER.md`](../../docs/MASTER_SERVER.md).
