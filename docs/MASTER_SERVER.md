# The master discovery server — the game side of internet play

The **server** half of internet discovery exists and is tested:
[`../tools/servers/masterserver/`](../tools/servers/masterserver/README.md) is a
running HTTP + JSON registry, and
[`../tools/servers/dediserver/`](../tools/servers/dediserver/README.md) can register
itself with it (`--master`). This document is the **game** half: what `mp_net.c` /
`mp_ui.c` / `mp_config.c` still need so that a host registers and a client lists
remote games from inside the game.

**None of this is implemented yet.** It is written down so the work has a shape and
so the wire it will talk to is already frozen ([the API contract](../tools/servers/masterserver/README.md)).

## What already works

* **Direct connect over the internet works today.** One authoritative host over TCP,
  the host binds `INADDR_ANY`, clients use an ephemeral outbound port — so a
  forwarded port is reachable and a NAT'd client is fine. The only thing missing is
  *finding* a game; joining one by address already works.
* **The manual address field already joins a public host.** `mp_ui.c:412`
  (`ActManualConnect`) parses `host[:port]` and calls `MpBeginJoinAsync`, which
  resolves DNS names too (`mp_net.c:796`). So an internet client is usable *now* if
  it knows the address.
* **The LAN browser renders any `(ip, port)` row.** `JoinOnEnter` (`mp_ui.c:658`)
  lists `MpDiscoveryGet(i)` and `ActJoinServer` (`mp_ui.c:330`) connects to
  `s->ip:s->port`. A remote-server row would plug in with **no UI change**.

## What the game still needs

### 1. Config (`mp_config.c`)

Two new keys, read the same way as the rest (`mp_config.c:40` onward):

| key | default | meaning |
| --- | --- | --- |
| `master_url` | `""` | master base URL, e.g. `http://master.example.com:8100`. Empty = internet listing off (the LAN behaviour is unchanged). |
| `master_interval_ms` | `20000` | heartbeat / list-refresh cadence. Keep well under the master's `ttl_s`. |

### 2. Host: register and heartbeat

* **Register** where a host starts advertising: `MpStartMatch` calls
  `MpDiscoveryStart(1)` at `mp_session.c:1489`. A `POST /v1/register` goes out on the
  same edge (internet registration is the WAN twin of that LAN advertise).
* **Heartbeat** on the beacon cadence, inside `MpDiscoveryPoll` (`mp_net.c:2429`),
  which `MpNetPoll` already calls every frame (`mp_net.c:1939`). Update the player
  count and `inProgress` so the listing stays true.
* **Unregister** wherever advertising stops: `MpDiscoveryStop` (`mp_net.c:2241`) and
  `MpSessionReset` (`mp_session.c:135`) — a clean leave should not wait for the TTL.

### 3. Client: fetch and list

`GET /v1/servers` on the `master_interval_ms` timer while the Join menu is open
(`mp_ui.c:224` begins browsing), and merge the rows into what the browser shows.

### 4. The two problems the C side has that the Python tools do not

* **The server list is wiped on every browse teardown.** `MpDiscoveryStop` does
  `memset(gServers, 0, ...)` (`mp_net.c:2251`), and the only insert path,
  `MpServerTouch` (`mp_net.c:2309`), is `static`, reachable only from beacon receive.
  Remote rows therefore need either their own storage (`gRemoteServers[]`, merged in
  `MpDiscoveryCount`/`MpDiscoveryGet`, `mp_net.c:2460`) or a re-fetch on every open.
  A dedicated array is the honest fix — the LAN rows are UDP-lived, the remote rows
  are HTTP-lived, and they have different timeouts.
* **An HTTP client in C, without stalling the frame.** The session layer already
  knows how to do non-blocking sockets (`MpSetNonBlocking`); the same discipline
  applies: a request/response state machine driven from `MpNetPoll`, never a blocking
  read on the main thread. JSON parsing can be a tiny fixed-subset parser — the
  master's reply shape is frozen and small (see the contract), so it does not need a
  general JSON library.

### 5. Treat the list as untrusted

The master is unauthenticated by design (anyone may register). A fetched row is
peer-supplied network data exactly like a beacon, so the existing hygiene rules
apply: bound every string read, validate `host`/`port` before it becomes a connect
target, and never let a length from the wire size a buffer. (The same class as the
existing rule that every peer-supplied wire string is terminated at receipt.)

## The minimal shippable slice

Because direct connect already works, internet play can ship in two steps:

1. **Listing off the game (available now):** operators run
   [`dediserver/`](../tools/servers/dediserver/README.md) with `--master`, and
   players join by address with the manual field. Nothing new in the game.
2. **Listing in the game (this document):** the config keys, the register/heartbeat
   edge, the remote-row array, and the non-blocking HTTP fetch above.

## Keeping the wire honest

The master speaks JSON and never touches the mp binary protocol, so `mp_proto.h`
and `mp_test.py` are unaffected by it. The one shared number is
`MP_PROTO_VERSION`: a host registers it, and the master only *logs* a mismatch
against the local mirror. `tools/servers/proto_check.py` is what keeps the Python
mirror and `mp_proto.h` from drifting, and it runs in CI.
