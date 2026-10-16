# The master discovery server — the game side of internet play

The **server** half of internet discovery exists and is tested:
[`../tools/servers/masterserver/`](../tools/servers/masterserver/README.md) is a
running HTTP + JSON registry, and
[`../tools/servers/dediserver/`](../tools/servers/dediserver/README.md) can register
itself with it (`--master`). This document is the **game** half: what `mp_net.c` /
`mp_ui.c` / `mp_config.c` still need so that a host registers and a client lists
remote games from inside the game.

**The client half is implemented; the host half is not.** A player can open
`Multiplayer -> Online`, see the master's list (five columns, and the servers it
cannot reach are just rows), and join one. What is still missing is the other
direction: an in-game host does not register itself, so today a game reaches the
master only through [`dediserver/`](../tools/servers/dediserver/README.md) (or a
`--master` registration the operator runs). Section 3 below is what changed for
the client; sections 1 (host key), 2 and the slice are the remaining work.

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

`master_url` is read the same way as the rest (`mp_config.c:40` onward) and is
**done** — the client uses it. What the HOST side still needs:

| key | default | meaning |
| --- | --- | --- |
| `master_url` | `""` | the master to list from AND (once the host half lands) to register with, e.g. `http://master.example.com:8100`. Empty = internet listing off, and the LAN behaviour is unchanged. **Done, client side.** |
| `master_interval_ms` | `20000` | heartbeat / list-refresh cadence for the HOST's registration. Keep well under the master's `ttl_s`. Not added yet: the client fetches on screen-open instead, and a cadence nothing uses would only be a number to keep true. |

### 2. Host: register and heartbeat

* **Register** where a host starts advertising: `MpStartMatch` calls
  `MpDiscoveryStart(1)` at `mp_session.c:1489`. A `POST /v1/register` goes out on the
  same edge (internet registration is the WAN twin of that LAN advertise).
* **Heartbeat** on the beacon cadence, inside `MpDiscoveryPoll` (`mp_net.c:2429`),
  which `MpNetPoll` already calls every frame (`mp_net.c:1939`). Update the player
  count and `inProgress` so the listing stays true.
* **Unregister** wherever advertising stops: `MpDiscoveryStop` (`mp_net.c:2241`) and
  `MpSessionReset` (`mp_session.c:135`) — a clean leave should not wait for the TTL.

### 3. Client: fetch and list — DONE

The Online screen (`mp.online`) lists the master's servers and joins the selected
one through the same `MpBeginJoinAsync` the LAN rows use. What was built:

* `mp_config.c` reads `master_url` (accepts `host`, `host:port` and
  `http://host:port`; empty = the screen says so). No `master_interval_ms`: the
  fetch runs when the screen OPENS, which is when a player wants fresh data, and
  again only on an explicit refresh. A timer nobody asked for would be a second
  cadence to keep honest for no gain.
* `mp_net.c` holds the whole client (`MpOnlineSetMaster` / `MpOnlineRefresh` /
  `MpOnlinePoll` / `MpOnlineCount` / `MpOnlineGet`). `mp_online` is NOT a separate
  file on purpose: it needs mp_net.c's static socket helpers, and exposing
  `SOCKET` through `mp.h` to split it out would be worse than the size.
* `mp_ui.c` builds the rows (`OnlineOnEnter`) and draws the table
  (`MpUiDrawOnline`), from `JER_EVENT_FRONTEND_DRAW` — see below.

### 4. The two problems the C side has that the Python tools do not — SOLVED

* **The server list is wiped on every browse teardown.** Confirmed: `MpDiscoveryStop`
  `memset`s `gServers` (`mp_net.c:2251`). So the online list has its OWN storage
  (`gOnline[]` in `mp_net.c`), exactly as this document argued for: the LAN rows are
  UDP-lived and the remote rows are HTTP-lived, and merging them would have made one
  timeout table answer two questions.
* **An HTTP client in C, without stalling the frame.** Built as predicted: a
  connect/send/recv state machine driven from the frame poll, `MpSetNonBlocking` +
  `select` with a zero timeout, a bounded 8 KiB reply buffer and a 5 s deadline. The
  JSON is read by a fixed-subset parser (`MpJsonValue` / `MpJsonInt` / `MpJsonStr`).
  Every failure leaves the PREVIOUS list alone: a reply it cannot follow is a
  refusal, not an empty list.
* **The one thing this document did not foresee:** a module cannot draw a menu row
  from `JER_EVENT_FRAME`. That event fires BEFORE the engine draws the screen, so the
  menu box covers anything drawn there. The frontend therefore gained
  `JER_EVENT_FRONTEND_DRAW`, fired right after `DrawScreen`; the table uses it with
  `jer_text.h` (measure + draw at an x) and `jer_frontend_row_rect()`.

### 5. Treat the list as untrusted

The master is unauthenticated by design (anyone may register). A fetched row is
peer-supplied network data exactly like a beacon, so the existing hygiene rules
apply: bound every string read, validate `host`/`port` before it becomes a connect
target, and never let a length from the wire size a buffer. (The same class as the
existing rule that every peer-supplied wire string is terminated at receipt.)

## The minimal shippable slice

Because direct connect already works, internet play can ship in two steps:

1. **Listing off the game:** operators run
   [`dediserver/`](../tools/servers/dediserver/README.md) with `--master`, and
   players join by address with the manual field.
2. **Listing in the game (DONE):** the `master_url` key and the non-blocking fetch,
   so a player picks a server from `Multiplayer -> Online` instead of typing one.
3. **Registering from the game (TO DO):** sections 1 and 2 above, so hosting needs
   no external process at all.

## Keeping the wire honest

The master speaks JSON and never touches the mp binary protocol, so `mp_proto.h`
and `mp_test.py` are unaffected by it. The one shared number is
`MP_PROTO_VERSION`: a host registers it, and the master only *logs* a mismatch
against the local mirror. `tools/servers/proto_check.py` is what keeps the Python
mirror and `mp_proto.h` from drifting, and it runs in CI.
