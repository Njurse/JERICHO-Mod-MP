def patch(path, pairs):
    d = open(path, "rb").read().decode("utf-8").replace("\r\n", "\n")
    for old, new in pairs:
        n = d.count(old)
        assert n == 1, (path, n, old[:70])
        d = d.replace(old, new, 1)
    open(path, "wb").write(d.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8"))
    print("patched", path)


# ------------------------------------------------------------ the master's README
patch("masterserver/README.md", [
    ('''      "gameBuild": 0,
      "ageSeconds": 12.5
    }''',
     '''      "gameBuild": 0,
      "ageSeconds": 12.5,
      "reachable": true,
      "probeMs": 31
    }'''),

    ('''`ageSeconds` is how long ago the master last heard from that host (from the
master's clock), so a client can render "12s ago".''',
     '''`ageSeconds` is how long ago the master last heard from that host (from the
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
> is from another machine: `python3 tools/mp_test.py client <host> --port <port>`.'''),

    ('''| `sweep_s` | `5` | how often expired entries are swept |''',
     '''| `sweep_s` | `5` | how often expired entries are swept |
| `probe_s` | `5` | how often each listing's session port is checked (0 disables) |'''),

    ("| `public_url` | `\"\"` | shown on the `GET /` page, e.g. the DNS name clients use |",
     "| `public_url` | `\"\"` | shown on the `GET /` page, e.g. the DNS name clients use |\n"
     "\n"
     "`--probe SECONDS` overrides `probe_s` from the command line."),
])

# ------------------------------------------------------------ the dedi's README
patch("dediserver/README.md", [
    ("## Console",
     "## Console") if False else
    ("""## Configuration""",
     '''## The console

Run it in a shell and it has a command line; `help` lists what it takes. When stdin
is not a terminal (a service, a container without `-it`, a captured run) it says so
once and stays out of the way — `JER_CONSOLE=on` forces it on, which is how you can
pipe commands into a server under a supervisor.

| command | what it does |
| --- | --- |
| `status` | name, session port, city, gamemode/rules, players, uptime, beacon, master |
| `list` | connected players: id, name, address, how long connected |
| `say <text>` | one line into every player's console, prefixed `SERVER:` |
| `kick <id>` | drop a player — the protocol's own kick message, so they are told why |
| `beacon [on\\|off]` | the LAN beacon's state, or turn it off (a LAN you do not want to appear on) |
| `master` | discovery registration: the URL, whether registered, what last happened |
| `stop` | close the server (unregisters from the master, closes players) |

## Configuration'''),
])

# ------------------------------------------------------------ the servers' README
patch("README.md", [
    ("## Start here",
     '''## Consoles

Both servers have an interactive command line on **stdin** — not a network admin
port: nothing listens, nothing authenticates, and the people who can use it already
have a shell on the machine. Start one in a terminal and `help` lists its commands.

With no terminal attached (a service, `docker compose up -d`, a captured run) the
console says so once and stays out of the way. `JER_CONSOLE=on` forces it on — that
is how a supervisor (or `test_console_stdin.py`) can pipe commands in:

```sh
printf 'status\\nlist\\nstop\\n' | JER_CONSOLE=on ./run.sh dedi
```

| server | commands |
| --- | --- |
| master | `status`, `list`, `drop <token\\|name>`, `persist`, `stop` |
| dedicated | `status`, `list`, `say <text>`, `kick <id>`, `beacon [on\\|off]`, `master`, `stop` |

Both share `help`, and the implementations live in one place —
[`serverkit.py`](serverkit.py) — together with the logging, config loading and
signal handling the two servers used to carry twice.

## Start here'''),
])
