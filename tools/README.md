# mp testing tools

Everything for driving a multiplayer session lives here — launchers and harnesses
together, so there is one place to look instead of a scatter of ad-hoc command
lines. The `.bat` files are thin wrappers; the real work is in the Python.

Path handling: the launchers find the game relative to themselves
(`tools/` → repo root → `src_rebuild/bin/Release_dev`), so they work from a
checkout anywhere. Set `MP_EXEDIR` to point them somewhere else.

## Which one do I want?

| I want to… | Use |
| --- | --- |
| test anything, on one PC | **`mp_pair.bat`** |
| host on this machine, by hand, two machines | `mp_host.bat` |
| join, by hand, two machines | `mp_join.bat` |
| drive the real game as a client with no second machine | `mp_mock_host.bat` |
| leave a session running with no game window | `mp_dedi.bat` |

## `mp_pair.bat` — two real instances, one PC

**Start here.** It runs a real host and a real client side by side and prints both
logs. Nearly every multiplayer bug so far showed up here, and the mock could never
have found them: it only ever puts *one* real engine in the room.

```
mp_pair.bat                       host + join, report, clean up
mp_pair.bat --keep                leave the run dirs so you can read the logs
mp_pair.bat --settle 30 --seconds 80
mp_pair.bat --clean               remove the run dirs again
mp_pair.bat --help                everything else
```

It builds two throwaway run directories beside the game from directory junctions,
so the big trees are shared and nothing is copied. **Separate working directories
are the whole trick** — they are what stop the two `JERICHO.log` files and the
two `mp.ini` files fighting each other, which is why two instances could not be
tested before this existed.

Both executables are launched **directly**, so the PIDs are real and the cleanup
stops exactly what it started. That is the one caveat on the other launchers:
they use `start`, which detaches, so **there is no PID to kill afterwards** — close
the game window instead of hunting for a process.

The host instance gets `MP_AUTOSTART=host` (it has to start the match itself now
that the attract demo is suppressed) and the client deliberately does not.

## `mp_host.bat` / `mp_join.bat` — two machines, by hand

```
mp_host.bat                 host on 1400
mp_host.bat 1500            ... on another port
mp_host.bat lobby           stay in the lobby instead of auto-starting
mp_host.bat dry 1500        print what would run, launch nothing

mp_join.bat 192.168.1.42    join a host by LAN address
mp_join.bat 192.168.1.42:1400
mp_join.bat                 host and client on this one machine
mp_join.bat dry 10.0.0.5    print what would run, launch nothing
```

Joining by address skips LAN discovery and needs only the host's session port
open, so it is the path to trust on a LAN with a firewall. `mp_join.bat` clears
`MP_AUTOSTART` — a client launches when the host's start arrives, and must never
be told to host as well — while `mp_host.bat` sets it, and leaves an
`MP_AUTOSTART` you set yourself alone.

## `mp_mock_host.bat` — a headless host

```
mp_mock_host.bat --start                   accept, then run a match
mp_mock_host.bat --start --peer-pad 0x40   and drive the "host" car
mp_mock_host.bat --reject mods             refuse the join, with a reason
mp_mock_host.bat --port 1400 --city 1 --start
```

Then point the game at it with `mp_join.bat 127.0.0.1:<port>`. It is one-sided by
nature — one real engine — so it can prove the wire format and the client's own
behaviour, never what two engines do to each other.

## `mp_dedi.bat` — a headless dedicated server

A real session with no game window, so a client has something to join and the
server can be left up while the client side restarts. It logs joins and leaves,
which is the fastest way to tell a client that never connected from one that
connected and was then dropped. Ctrl-C stops it.

## The Python underneath

| File | What it is |
| --- | --- |
| `mp_localpair.py` | the two-instance harness `mp_pair.bat` wraps; prints a PASS/FAIL verdict and reads `JERICHO.log` |
| `mp_test.py` | mock host / client / beacon, plus the protocol checks |
| `mp_dediserver.py` | the dedicated server `mp_dedi.bat` wraps |
| `check_debug_independence.py` | fails if any debug `getenv` guard wraps control flow or state (see the traps doc) |
| `_liveness_probe.py` | connects as a bare client and reports WELCOME/START/PONG/EOF -- the quick way to tell a server that answers from one that is silent |

**Liveness contract for any mock/dedi host.** A server here must answer a client's
`PING` with a `PONG` (echo the tick) and keep the connection up; a client that gets
nothing after the handshake drops itself at its idle timeout. The mock used to close
0.5 s after `WELCOME` and the dedi never replied to `PING`, so both looked like
instant drops. The wire format lives in `mp_test.py` -- when `mp_proto.h` changes,
update it there (e.g. `WELCOME` is `<12BIB`, 12xu8 + u32 seed + u8 hostCar; a
`<13BI` there crashes both simulated hosts).

`mp_localpair.py` takes `--no-debug` to run WITHOUT `MP_DEBUG=1` -- the packaged
launchers (PLAY_HOST/JOIN) never set it, so it is the only way to test what a player
actually runs (a bug that appears only without `MP_DEBUG` is invisible otherwise).

`mp_test.py` is also the reference for the wire format — it packs every message by
hand, so when a field changes there is exactly one other place to update.

## The test bot (`MP_BOT`, off by default)

Drives a real player's car so a pair can be exercised without two humans. It is a
testing component: nothing in the session/network path may depend on it, and it is
inert unless `MP_BOT` asks for it.

| `MP_BOT` | what it does |
| --- | --- |
| `random` (also `MP_TESTDRIVE=1`) | the canned manoeuvre: a pad that changes every 0.5–4 s |
| `chase` | the HOST flees, the joiner chases |
| `fight` | both charge each other |
| `pursuit` | MUTUAL chase: both cars hunt each other, so the pair reliably meets and collides |

`mp_localpair.py --bot <mode>` sets it on both instances. `pursuit` is the one to
reach for when you want the pair actually driving a distance around scenery (so the
network layer is exercised under real motion): the bots pick a clear heading with
the engine's own `CellEmpty` out of a fan of 15° steps covering a full ±180° (so a
heading AROUND a wall, up to a U-turn, is findable), looking both near (so they do
not nose into a wall) and far (so they do not commit to a gap that closes), hold a
chosen heading until it is blocked (otherwise the steering flaps), turn round when
the gap stops closing for 150 frames, and back out when they wedge. Their progress
and the resulting host<->client deviation are in the `[mp] bot:` and `[mp] sync:`
lines.

`--host-car default` (and `--client-car default`) pass NO `-mpcar` on that side, i.e.
exactly what a player who just presses Host/Join does: the ENGINE/level-facing
assignment decides the car (`config.car` stays -1). Use both together to reproduce
a whole no-`-mpcar` session -- the case where the two machines used to disagree
about who drives what (see trap 12 in `docs/ARCHITECTURE.md`).

## Reading a run

The logs are chatty at `MP_DEBUG=1`; `JPPN`, `JPPO`, `pose:`, `JPIN` and `JPCS`
flood them. Filter those out:

```sh
grep -a "\[mp\]\|\[error\]" JERICHO.log | grep -av "JPPN\|JPPO\|pose:\|JPIN\|JPCS"
```

Useful markers: `launching: city N mode M (1=TAKEADRIVE, 0=MISSION!)` — mode 0
means the mission ladder, i.e. the launch went wrong — `car: player N slot S`,
`added N remote player car(s)`, `map: drew N remote blip(s)`, `list:` (the pause
menu rows), and `peer dropped (<why>)`.
