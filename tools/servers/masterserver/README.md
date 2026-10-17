# masterserver — the mp master discovery service

> ## This is a SEPARATE PROJECT. You probably don't want it.
>
> This folder is a network **service**, not a game mod and not game content. It
> exists so that internet hosts can register and clients can list games without
> port-forwarding just to *find* one. If you downloaded the mp mod, you do not
> need this — you need the game, and at most [`../dediserver/`](../dediserver/README.md).
>
> It lives here only because this is where the mp tooling is and a checkout here
> makes it immediately testable. It is designed to be lifted into its own
> repository unchanged (see "Extracting this" at the end) — one file, the
> standard library, no game.

`masterserver.py` is a small HTTP + JSON registry:

* a **host** `POST`s `/v1/register` and then heartbeats; the master remembers the
  host's *public* address (the TCP peer IP, so no NAT configuration is needed — the
  same reason the game's direct connect works) with a time-to-live;
* a **client** `GET`s `/v1/servers` and gets the list of live entries;
* an entry that stops heartbeating is dropped after its TTL, so a crashed host
  disappears on its own.

It is deliberately **additive**: it does not touch the mp binary wire protocol
(`mp_proto.h`) at all. The game's LAN discovery keeps working exactly as before;
this is the internet listing on top. See
[`docs/MASTER_SERVER.md`](../../../docs/MASTER_SERVER.md) for the game-side work this
will eventually need.

## Run it

On a server or VPS, the compose file next door runs this and the dedi together
with host networking: see [`../docker-compose.yml`](../docker-compose.yml) and
[`../README.md`](../README.md#running-it-on-a-server-docker). Otherise:

```sh
python masterserver.py                          # bind 0.0.0.0:8100
python masterserver.py --port 8080 --ttl 90
python masterserver.py --config masterserver.json
```

Then, from anywhere:

```sh
curl http://127.0.0.1:8100/v1/health
curl http://127.0.0.1:8100/v1/servers
```

Open `http://127.0.0.1:8100/` in a browser for a plain HTML list.

## The API contract (v1)

Everything below is the contract the server and its tests are written against.
Bodies are JSON (UTF-8) with `Content-Type: application/json`. Every response
carries `"version": 1` (the master API version). Errors are
`{"error": "<code>", "message": "<human>"}` with a 4xx/5xx status.

The `host` the master reports is **always the public address the entry was
recorded from** — the TCP peer IP, or the host's own `advertiseHost` if it sent
one. The master never stores or returns a host's private/LAN address.

### `POST /v1/register`

A host announces itself. Re-registering the same public `(host, port)` replaces
the previous entry and issues a fresh token.

```jsonc
{
  "name": "Bob's Game",     // 1..31 chars; the name shown in a browser
  "port": 1318,             // the host's TCP session port (the port clients connect to)
  "city": 1,                // 0..3, the GameLevel clients should load
  "gamemode": 0,
  "rules": 1,               // MP_RULES_PACK'd session rules (mp_proto.h). The
                            // SAME packed byte the LAN beacon carries, so a
                            // browser names the mode from either. Optional, 0
                            // default; 1 = COPS N ROBBERS, 0 = TAKE A RIDE
  "players": 0,             // current players
  "maxPlayers": 8,
  "inProgress": 0,          // 1 once the host's level has launched
  "modsEnforced": 0,        // MP_MODCHECK_*
  "modHash": 0,             // short digest of the host's manifest
  "protoVersion": 11,       // mp_proto.h MP_PROTO_VERSION
  "sdkVersion": 2,
  "gameBuild": 0,           // JERICHO build hash, optional
  "advertiseHost": "game.example.com",  // OPTIONAL override of the public address
  "advertisePort": 30000                // OPTIONAL override of the port players dial,
                                        // for a router that REMAPS it (0/absent = `port`)
}
```

→ `201 Created`

```json
{"ok": true, "token": "9f2c...", "host": "203.0.113.5", "port": 1318, "ttl": 60, "version": 1}
```

`token` is the handle for heartbeat/unregister; keep it. `host` is what the
master recorded, so the host can see what it is advertising.

### `POST /v1/heartbeat`

Refresh the TTL (and optionally the live counts). Only the fields you send are
updated.

```json
{"token": "9f2c...", "players": 3, "inProgress": 1}
```

→ `200 OK` `{"ok": true, "ttl": 60, "expiresIn": 57, "version": 1}`

→ `404` `{"error": "unknown_token", ...}` if the token was never issued or has
already expired — the host must `/v1/register` again (this is how a restarted
master tells a host to re-register).

Heartbeat at about `ttl / 3` (default 20 s) so one lost heartbeat does not drop a
live host.

### `POST /v1/unregister`

Leave immediately on a clean shutdown. Idempotent (an unknown token is `ok`).

```json
{"token": "9f2c..."}
```

→ `200 OK` `{"ok": true}`

### `GET /v1/servers`

The listing. **Everything is returned by default** — a live match must not be
hidden from a client that did not ask for anything special. Optional query
flags: `limit=N`, and the opt-in *exclusions* `exclude_full=1` /
`exclude_inprogress=1` (a client may always filter itself).

→ `200 OK`

```json
{
  "version": 1,
  "count": 1,
  "generatedAt": 1710000000.5,
  "servers": [
    {
      "name": "Bob's Game",
      "host": "203.0.113.5",
      "port": 1318,
      "players": 3,
      "maxPlayers": 8,
      "city": 1,
      "gamemode": 0,
      "rules": 1,
      "inProgress": 1,
      "modsEnforced": 0,
      "modHash": 0,
      "protoVersion": 11,
      "sdkVersion": 2,
      "gameBuild": 0,
      "ageSeconds": 12.5,
      "reachable": true,
      "probeMs": 31
    }
  ]
}
```

`ageSeconds` is how long ago the master last heard from that host (from the
master's clock), so a client can render "12s ago".

`reachable` is whether anything **answers on the session port**, which is a
different question from `ageSeconds`: an entry can heartbeat happily while nothing
listens on its port (a firewall, a container whose port is not published, a server
that moved). `null` until the first probe, `false` when it stopped answering —
and the master logs `UNREACHABLE: '<name>' at host:port` **once** when that
changes, not once per sweep. `probeMs` is the connect time, at least 1.

> **The one case it cannot see.** If the dedicated server runs on the SAME host as
> the master, the probe connects over the host's own public address — which works
> locally even when a firewall blocks it from the internet. `reachable: true` then
> means "answers from here", not "answers from anywhere". The check that settles it
> is from another machine: `python3 tools/mp_test.py client <host> --port <port>`.

### `GET /v1/health`

```json
{"ok": true, "version": 1, "uptimeSeconds": 12.3, "servers": 1}
```

### `GET /`

A minimal HTML page listing the servers, so a human can just open the master in
a browser. Not an API — it links to `/v1/servers`.

## Semantics worth stating

* **TTL, not polling.** An entry lives for `ttl_s` seconds after the last
  register/heartbeat and is then dropped. Default `ttl_s` = 60.
* **The public address is derived, not trusted.** `host` comes from the socket
  peer address. `advertiseHost` is an explicit override for a host behind a
  reverse proxy or with a known public DNS name; the master does not guess it.
* **The port is the one players can dial.** `advertisePort`, when sent, becomes
  the entry's port — the listing names it, and it is what identifies the entry.
  A host whose router maps public `30000` to local `1318` registers
  `port: 1318, advertisePort: 30000`, so the list shows `host:30000`. Without it
  the master assumes the forward is 1:1, which is the usual case for a game rule.
* **In memory.** v1 keeps the registry in memory. An optional `persist_path`
  writes it to JSON on change and reloads on start, so a master restart does not
  blank the list; it is off by default.
* **Open and unauthenticated.** Any host may register and the list is public.
  That is intentional for a first version and matches how the game's direct
  connect already works (a forwarded port is reachable by anyone). Do not treat
  this as abuse-resistant: there is no auth, no rate limit beyond a cap on
  entries and body size, and no reputation.

## Config

`masterserver.json` (all keys optional; CLI flags override):

| key | default | meaning |
| --- | --- | --- |
| `bind` | `0.0.0.0` | interface to listen on |
| `port` | `8100` | HTTP port |
| `ttl_s` | `60` | entry lifetime without a heartbeat |
| `sweep_s` | `5` | how often expired entries are swept |
| `probe_s` | `5` | how often each listing's session port is checked (0 disables) |
| `max_entries` | `512` | cap on registered hosts |
| `max_body_bytes` | `4096` | request body cap (larger → 413) |
| `persist_path` | `""` | JSON file to persist the registry ("" = off) |
| `log_level` | `info` | `debug` / `info` / `warning` / `error` |
| `log_file` | `""` | append logs here as well as stdout ("" = stdout only) |
| `public_url` | `""` | shown on the `GET /` page, e.g. the DNS name clients use |

`--probe SECONDS` overrides `probe_s` from the command line.

## Testing

`test_masterserver.py` is self-contained (loopback only, no game, no assets):

```sh
python test_masterserver.py
```

It registers a host, lists it, heartbeats it past a short TTL boundary, checks it
expires, and checks unregister removes it. The CI "Checks" job runs it.

## Extracting this

Nothing here imports the game or any other tool. To lift it into its own
repository: copy `masterserver.py`, `masterserver.json`, `test_masterserver.py`
and this README; the only in-repo dependency is `../proto_check.py`, which is
used solely to validate the `protoVersion` a host registers against the mod's
mirror — drop that import (or copy the two constants it needs) and it is
standalone.
