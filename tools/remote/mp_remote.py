#!/usr/bin/env python3
"""Drive a second PC through its mp agent, so a two-machine test is one command.

    python mp_remote.py status   --peer 192.168.50.244
    python mp_remote.py update   --peer 192.168.50.244 [--tag v0.9.1]
    python mp_remote.py deploy   --peer 192.168.50.244 --seat host
    python mp_remote.py run      --peer 192.168.50.244 --seat host --seconds 90
    python mp_remote.py logs     --peer 192.168.50.244
    python mp_remote.py rollback --peer 192.168.50.244
    python mp_remote.py stop     --peer 192.168.50.244

The other PC runs `mp_agent.ps1` (START_AGENT.bat -Bind <its LAN address>) once
and is then hands-free: `update`/`deploy`/`run` tell it which GitHub RELEASE to
install (the rolling 'alpha' pre-release unless --tag says otherwise), and the
agent downloads that release's Release_dev build (JERICHO_Release_dev_win64.zip;
the plain Release assets are refused) from GitHub itself, verifies it, stops any
running game, installs the build and starts it again on its own. Nothing is pushed from here:
the only thing an update carries is the release tag.

The agent's token is printed on that PC the first time it starts (and kept in its
mp_agent.config.json). Pass it with --token or the MP_AGENT_TOKEN environment
variable.

This machine is driven directly (Popen, so a real PID we can kill), which is why
the local seat is reliable and the remote seat goes through the agent.
"""

import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)                       # JERICHO/MODS/mp/tools
REPO = os.path.abspath(os.path.join(TOOLS, "..", "..", "..", ".."))
GAME_DIR = os.path.join(REPO, "src_rebuild", "bin", "Release_dev")
EXE_NAME = "JERICHO_dev.exe"
WORK = os.path.join(GAME_DIR, ".mp-remote")          # logs pulled from both seats
DEFAULT_PORT = 1401
DEFAULT_TAG = "alpha"               # the rolling pre-release build.yml refreshes from main
# An update downloads ~20 MB from GitHub and installs it before it answers.
UPDATE_TIMEOUT = 900

sys.path.insert(0, TOOLS)
try:
    import mp_localpair as lp        # reuse the localhost pair's verdict logic
except Exception:                    # pragma: no cover
    lp = None


class AgentError(RuntimeError):
    pass


class Agent:
    """A tiny client for mp_agent.ps1: one line in, one line out."""

    def __init__(self, host, port=DEFAULT_PORT, token=None, timeout=30):
        self.host, self.port, self.token = host, port, token
        self.timeout = timeout

    def _connect(self):
        try:
            s = socket.create_connection((self.host, self.port), timeout=10)
        except OSError as e:
            raise AgentError(
                f"cannot reach the agent at {self.host}:{self.port} ({e}).\n"
                f"  Is START_AGENT.bat still running on {self.host}? Is it allowed "
                f"through that machine's firewall (TCP {self.port})?") from e
        s.settimeout(self.timeout)
        return s

    @staticmethod
    def _read_line(s):
        buf = b""
        while not buf.endswith(b"\n"):
            ch = s.recv(1)
            if not ch:
                raise AgentError("the agent closed the connection")
            buf += ch
        return buf.decode("ascii", "replace").rstrip("\r\n")

    def _command(self, cmd, rest="", keep_open=False, timeout=None):
        s = self._connect()
        if timeout is not None:
            s.settimeout(timeout)
        line = f"{cmd} {self.token}" + (f" {rest}" if rest else "")
        s.sendall(line.encode("ascii") + b"\n")
        reply = self._read_line(s)
        if reply.startswith("ERR "):
            s.close()
            raise AgentError(f"the agent refused '{cmd}': {reply[4:]}")
        if not reply.startswith("OK"):
            s.close()
            raise AgentError(f"unexpected reply to '{cmd}': {reply!r}")
        if keep_open:
            return s, reply[3:]
        s.close()
        return reply[3:]

    def ping(self):
        return self._command("ping")

    def status(self):
        body = self._command("status")
        try:
            return json.loads(body)
        except json.JSONDecodeError as e:
            raise AgentError(f"could not read the agent's status: {body!r}") from e

    def stop(self):
        return self._command("stop")

    def start(self, args):
        return self._command("start", args)

    def update(self, tag):
        """Have the agent install release `tag` from GitHub. Only the TAG goes over
        the wire; the agent fetches and verifies the build itself."""
        return self._command("update", tag, timeout=UPDATE_TIMEOUT)

    def rollback(self):
        return self._command("rollback")

    def log(self):
        s, header = self._command("log", keep_open=True)
        want = int(header.split()[0])
        data = b""
        while len(data) < want:
            chunk = s.recv(min(65536, want - len(data)))
            if not chunk:
                break
            data += chunk
        s.close()
        return data, header

    def dump(self):
        """The peer's JERICHO.dmp, or (b"", reason) when there is none.

        The `log` command only MENTIONS a dump in its header ("+ JERICHO.dmp
        present"); it does not send it, so a crash on the other PC used to be
        unattributable without walking over to that machine. A dump is a pure
        file read -- nothing about the game -- so it is safe to pull at any time.

        Tolerant on purpose: an OLDER agent (one started before this command
        existed) answers "ERR unknown command 'dump'", and that must cost the
        caller its dump, not the whole log pull.
        """
        try:
            s, header = self._command("dump", keep_open=True)
        except RuntimeError as e:
            return b"", str(e)

        if header.startswith("ERR") or header.split()[0] == "ERR":
            s.close()
            return b"", header

        want = int(header.split()[0])
        data = b""
        while len(data) < want:
            chunk = s.recv(min(65536, want - len(data)))
            if not chunk:
                break
            data += chunk
        s.close()
        return data, header


# ------------------------------------------------------------------ release install

def describe_release(st):
    """One line for what the peer has installed, from its status reply."""
    rel = st.get("release") or {}
    if not rel:
        return "no release installed by the agent"
    return (f"{rel.get('tag')} ({rel.get('commit') or '?'}) sha256 "
            f"{str(rel.get('sha256', ''))[:12].lower()}  [{rel.get('verifiedBy')}]")


def do_update(agent, tag):
    reply = agent.update(tag)
    print(f"  {reply}")
    return agent.status()


# ------------------------------------------------------------------ seats

def local_start(args, bot=None, env_extra=None):
    exe = os.path.join(GAME_DIR, EXE_NAME)
    if not os.path.isfile(exe):
        raise SystemExit(f"no {EXE_NAME} in {GAME_DIR} -- build first (build_dev.bat)")
    argv = [exe] + (args.split() if args else [])
    env = dict(os.environ)
    env.setdefault("MP_DEBUG", "1")     # local runs keep the verbose log

    if bot:
        env["MP_BOT"] = bot           # the same lever the peer's seat gets, so both cars drive

    for k, v in (env_extra or {}).items():
        env[k] = v
    # Keep the output: a game that dies in a second says why on stdout, and
    # discarding it turns "it exited" into a mystery.
    os.makedirs(WORK, exist_ok=True)
    out = open(os.path.join(WORK, "local.out"), "wb")
    p = subprocess.Popen(argv, cwd=GAME_DIR, env=env, stdout=out, stderr=subprocess.STDOUT)
    return p


def seat_args(seat, host_ip, port, extra, bot=None, env_extra=None):
    if seat == "host":
        base = f"-nointro -nofmv -host {port}"
    else:
        base = f"-nointro -nofmv -join {host_ip}:{port}"

    # `+K=V` is ENVIRONMENT on the far end -- the agent parses leading +tokens as env for
    # the game (there is deliberately no way to push files to that machine), which is how
    # a remote seat gets MP_BOT and the MP_TEST_* levers. MP_BOT=chase is mp_localpair's
    # default and means the HOST FLEES while every joiner CHASES.
    pairs = dict(env_extra or {})

    if bot:
        pairs["MP_BOT"] = bot

    prefix = "".join(f"+{k}={v} " for k, v in pairs.items())

    return (prefix + base + " " + extra).strip()


def default_ip():
    """This machine's LAN address, so the peer knows where to join."""
    import socket as _s
    try:
        s = _s.socket(_s.AF_INET, _s.SOCK_DGRAM)
        s.connect(("192.168.50.1", 1))          # never actually sends
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"


# ------------------------------------------------------------------ commands

def cmd_status(a):
    agent = Agent(a.peer, a.port, a.token)
    st = agent.status()
    here = "unknown"
    vp = os.path.join(GAME_DIR, "VERSION.txt")
    if os.path.isfile(vp):
        here = open(vp).read().strip()
    print(f"peer {a.peer}:{a.port}")
    print(f"  build there : {st.get('build')}")
    print(f"  release     : {describe_release(st)}  (from {st.get('repo')})")
    print(f"  rollback    : {'available' if st.get('previous') else 'none'}")
    print(f"  build here  : {here}")
    print(f"  game running: {st.get('running')}" + (f"  args: {st.get('args')}" if st.get('running') else ""))
    print(f"  log         : {st.get('logBytes')} bytes    crash dump: {st.get('dump')}")


def cmd_update(a):
    agent = Agent(a.peer, a.port, a.token)
    print(f"asking {a.peer}:{a.port} to install release '{a.tag}'")
    st = do_update(agent, a.tag)
    print(f"  now on: {st.get('build')}")


def cmd_rollback(a):
    agent = Agent(a.peer, a.port, a.token)
    print(f"  {agent.rollback()}")


def cmd_deploy(a):
    agent = Agent(a.peer, a.port, a.token)
    host_ip = a.host_ip or default_ip()
    print(f"deploying to {a.peer}:{a.port}   (local seat = {a.seat}, host is {host_ip})")

    if a.no_update:
        print("1. update: skipped (--no-update)")
        st = agent.status()
    else:
        print(f"1. update (release '{a.tag}')")
        st = do_update(agent, a.tag)
    # The peer runs a RELEASE build and this seat runs the local one; with
    # strict_version on they must be the same build or the join is refused.
    print(f"  peer runs : {st.get('build')}")
    vp = os.path.join(GAME_DIR, "VERSION.txt")
    print(f"  local runs: {open(vp).read().strip() if os.path.isfile(vp) else 'unknown (no VERSION.txt)'}")

    print("2. start")
    bot = None if a.bot == "off" else a.bot
    env_extra = {}

    for pair in (a.env or []):
        if "=" not in pair:
            raise SystemExit(f"--env wants K=V, got {pair!r}")
        k, v = pair.split("=", 1)
        env_extra[k] = v

    remote_args = seat_args("client" if a.seat == "host" else "host", host_ip, a.port_game, a.extra, bot, env_extra)
    local_args = seat_args(a.seat, host_ip, a.port_game, a.extra, None)

    if bot:
        print(f"  bot: {bot} on both seats (the host flees, every joiner chases)")

    for k, v in env_extra.items():
        print(f"  env: {k}={v} on both seats")

    if a.seat == "host":
        # the host first: it must be listening before the client dials
        local_proc = local_start(local_args, bot, env_extra)
        print(f"  local  HOST  pid {local_proc.pid}: {local_args}")
        time.sleep(a.lead)
        print(f"  remote CLIENT: {agent.start(remote_args)}")
    else:
        print(f"  remote HOST : {agent.start(remote_args)}")
        time.sleep(a.lead)
        local_proc = local_start(local_args, bot, env_extra)
        print(f"  local  CLIENT pid {local_proc.pid}: {local_args}")

    os.makedirs(WORK, exist_ok=True)
    with open(os.path.join(WORK, "local.pid"), "w") as f:
        f.write(str(local_proc.pid))
    print(f"  (local pid recorded in {os.path.join(WORK, 'local.pid')})")
    return local_proc


def cmd_stop(a):
    agent = Agent(a.peer, a.port, a.token)
    print(f"remote: {agent.stop()}")
    pidfile = os.path.join(WORK, "local.pid")
    if os.path.isfile(pidfile):
        pid = int(open(pidfile).read().strip())
        # PID-scoped: only the process WE started, never "the newest game".
        try:
            subprocess.run(["taskkill", "/PID", str(pid), "/F"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            print(f"local: stopped pid {pid}")
        except Exception as e:
            print(f"local: could not stop pid {pid}: {e}")
    else:
        print("local: no recorded pid (start it with deploy/run so it is known)")


def verdict_args(a, dirs):
    """The arguments mp_localpair.verdict() now takes: the seat NAMES, and the logs
    keyed by them. Two seats here -- "a" is the host and "b" the joiner -- mapped
    onto whichever machine this is, rather than passing our own key names through
    (which raised KeyError('a') the first time it was actually run).

    (It takes the names because a local run can be 2..8 seats; a remote pair is
    always two, so the names are ours to choose.)"""
    names = ("a", "b")

    if a.seat == "host":
        return names, {"a": dirs["local"], "b": dirs["peer"]}

    return names, {"a": dirs["peer"], "b": dirs["local"]}


def pull_logs(a, since=None):
    agent = Agent(a.peer, a.port, a.token)

    def clear_stale_dump(seat):
        """A dump COPY left in the run dirs by an earlier pull poisons every later
        verdict: mp_localpair triages whatever dumps it finds there. So a dump judged
        not to be this run's has to be removed from them, not merely not-refreshed --
        keeping it out of the pull is only half the job."""
        p = os.path.join(WORK, seat, "JERICHO.dmp")
        if os.path.isfile(p):
            os.remove(p)
            print(f"  {seat}: removed a stale dump copy from the run dirs "
                  "(it predates this run, so the verdict must not see it)")
    os.makedirs(os.path.join(WORK, "local"), exist_ok=True)
    os.makedirs(os.path.join(WORK, "peer"), exist_ok=True)

    data, header = agent.log()
    with open(os.path.join(WORK, "peer", "JERICHO.log"), "wb") as f:
        f.write(data)
    print(f"  peer  log: {len(data)} bytes  ({header})")

    # A crash is a different animal from a clean exit, so say which. But a dump on disk
    # may be from an EARLIER session: both folders are reused, and the run dirs persist.
    # It is therefore judged against `since` (when this run started) and a stale one is
    # NOT put in the run dirs at all -- mp_localpair's verdict triages whatever dumps it
    # finds there, so a stale dump became "*** A CRASHED ***" for a run that never
    # crashed. An undateable dump (an agent too old to report t=) is kept.
    m = re.search(r"t=(\d+)", header)
    peer_dump_t = int(m.group(1)) if m else None
    peer_fresh = (since is None) or (peer_dump_t is None) or (peer_dump_t >= since)

    if "JERICHO.dmp" in header:
        if peer_fresh:
            blob, dheader = agent.dump()

            if blob:
                with open(os.path.join(WORK, "peer", "JERICHO.dmp"), "wb") as f:
                    f.write(blob)
                print(f"  peer: CRASH DUMP from this run -- {len(blob)} bytes (an access "
                      f"violation, not an Alt+F4)")
            else:
                print(f"  peer: a dump was announced but the agent sent nothing ({dheader})")
        else:
            age = (time.time() - peer_dump_t) / 60.0 if peer_dump_t else 0.0
            print(f"  peer: a crash dump is present but is {age:.1f} min old, so NOT from "
                  "this run - not pulled, and kept out of the verdict")
            clear_stale_dump("peer")
    else:
        clear_stale_dump("peer")

    src = os.path.join(GAME_DIR, "JERICHO.log")
    if os.path.isfile(src):
        shutil.copyfile(src, os.path.join(WORK, "local", "JERICHO.log"))
        print(f"  local log: {os.path.getsize(src)} bytes")
    else:
        print("  local log: none yet")

    local_dump = os.path.join(GAME_DIR, "JERICHO.dmp")

    if os.path.isfile(local_dump):
        mtime = os.path.getmtime(local_dump)

        if since is None:
            # the bare `logs` command has no run to compare against, so it must not
            # claim the dump is "from this run" -- say how old it is and let the
            # reader judge
            shutil.copyfile(local_dump, os.path.join(WORK, "local", "JERICHO.dmp"))
            print(f"  local: a crash dump is present, {(time.time() - mtime) / 60.0:.1f} min "
                  "old (no run to compare against -- `logs` was asked directly)")
        elif mtime >= since:
            shutil.copyfile(local_dump, os.path.join(WORK, "local", "JERICHO.dmp"))
            print("  local: CRASH DUMP from this run (an access violation, not an Alt+F4)")
        else:
            print(f"  local: a crash dump is present but is {(time.time() - mtime) / 60.0:.1f}"
                  " min old, so NOT from this run - kept out of the verdict")
            clear_stale_dump("local")
    else:
        clear_stale_dump("local")

    return {"local": os.path.join(WORK, "local"), "peer": os.path.join(WORK, "peer")}


def cmd_logs(a):
    print("pulling both logs")
    dirs = pull_logs(a)
    if lp is not None:
        names, vdirs = verdict_args(a, dirs)
        print(f"  verdict: {lp.verdict(names, vdirs)}")
    else:
        print("  (mp_localpair not importable -- logs are in .mp-remote/)")


def cmd_run(a):
    t0 = time.time()        # the cutoff a crash dump is judged against
    proc = cmd_deploy(a)
    print(f"3. running for {a.seconds}s")
    deadline = time.time() + a.seconds
    try:
        while time.time() < deadline:
            if proc.poll() is not None:
                print(f"  local game EXITED early (code {proc.returncode}) after "
                      f"{a.seconds - int(deadline - time.time())}s")
                break
            time.sleep(1)
    except KeyboardInterrupt:
        print("  interrupted")
    finally:
        print("4. pulling logs")
        try:
            dirs = pull_logs(a, since=t0)
            if lp is not None:
                # exactly what cmd_logs does: verdict_args() returns (names, logs) and
                # verdict() takes them as two arguments. This line used to name
                # verdict_dirs(), which does not exist, so `run` always died here and
                # never printed a verdict at all.
                names, vdirs = verdict_args(a, dirs)
                print(f"  verdict: {lp.verdict(names, vdirs)}")
            else:
                print("  (mp_localpair not importable -- logs are in .mp-remote/)")
        except AgentError as e:
            print(f"  could not pull the peer's log: {e}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["status", "update", "deploy", "run", "logs", "rollback", "stop"])
    ap.add_argument("--peer", required=True, help="the other PC's IP")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help="the AGENT's port")
    ap.add_argument("--game-port", type=int, default=1400, dest="port_game",
                    help="the GAME's port")
    ap.add_argument("--token", default=os.environ.get("MP_AGENT_TOKEN"),
                    help="the agent's token (printed on that PC at first start; "
                         "default: $MP_AGENT_TOKEN)")
    ap.add_argument("--tag", default=DEFAULT_TAG,
                    help="the GitHub release the peer installs (default: alpha)")
    ap.add_argument("--no-update", action="store_true",
                    help="deploy/run: start the peer on whatever build it already has")
    ap.add_argument("--seat", choices=["host", "client"], default="host",
                    help="what THIS machine should be")
    ap.add_argument("--host-ip", default=None,
                    help="the host's address for the joining seat (default: this machine)")
    ap.add_argument("--extra", default="-mp 1 -level rio",
                    help="extra game arguments for both seats. Default is the rig's arena; "
                         "for a Take a Ride pair pass \"--extra '-level vegas'\" (the "
                         "gamemode defaults to takeadrive when -mp is not given)")
    ap.add_argument("--bot", default="chase", choices=["off", "chase", "pursuit", "random"],
                    help="drive BOTH player cars with the mp test bot. 'chase' (the "
                         "default, same as mp_localpair) makes the host FLEE and every "
                         "joiner CHASE, so an unattended pair moves; 'pursuit' has both "
                         "hunt; 'off' leaves the cars to whoever is at the keyboard")
    ap.add_argument("--env", action="append", metavar="K=V",
                    help="set an environment variable on BOTH seats (repeatable). This is "
                         "how the MP_TEST_* levers reach a seat, e.g. "
                         "--env 'MP_TEST_PAUSECAR=25,3,1;35,1,2' to make both players "
                         "cycle cars across cities mid-match")
    ap.add_argument("--lead", type=int, default=4,
                    help="seconds between starting the host and the client")
    ap.add_argument("--seconds", type=int, default=90, help="how long `run` waits")
    a = ap.parse_args()
    if not a.token:
        ap.error("no agent token: pass --token or set MP_AGENT_TOKEN (START_AGENT.bat "
                 "prints it on the other PC the first time it runs, and keeps it in "
                 "mp_agent.config.json there)")

    try:
        {"status": cmd_status, "update": cmd_update, "deploy": cmd_deploy,
         "run": cmd_run, "logs": cmd_logs, "rollback": cmd_rollback,
         "stop": cmd_stop}[a.action](a)
    except AgentError as e:
        print(f"\nERROR: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
