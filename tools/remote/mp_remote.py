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
agent downloads it from GitHub itself, verifies it, stops any running game,
installs the build and starts it again on its own. Nothing is pushed from here:
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

def local_start(args):
    exe = os.path.join(GAME_DIR, EXE_NAME)
    if not os.path.isfile(exe):
        raise SystemExit(f"no {EXE_NAME} in {GAME_DIR} -- build first (build_dev.bat)")
    argv = [exe] + (args.split() if args else [])
    env = dict(os.environ)
    env.setdefault("MP_DEBUG", "1")     # local runs keep the verbose log
    # Keep the output: a game that dies in a second says why on stdout, and
    # discarding it turns "it exited" into a mystery.
    os.makedirs(WORK, exist_ok=True)
    out = open(os.path.join(WORK, "local.out"), "wb")
    p = subprocess.Popen(argv, cwd=GAME_DIR, env=env, stdout=out, stderr=subprocess.STDOUT)
    return p


def seat_args(seat, host_ip, port, extra):
    if seat == "host":
        base = f"-nointro -nofmv -host {port}"
    else:
        base = f"-nointro -nofmv -join {host_ip}:{port}"
    return (base + " " + extra).strip()


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
    remote_args = seat_args("client" if a.seat == "host" else "host", host_ip, a.port_game, a.extra)
    local_args = seat_args(a.seat, host_ip, a.port_game, a.extra)

    if a.seat == "host":
        # the host first: it must be listening before the client dials
        local_proc = local_start(local_args)
        print(f"  local  HOST  pid {local_proc.pid}: {local_args}")
        time.sleep(a.lead)
        print(f"  remote CLIENT: {agent.start(remote_args)}")
    else:
        print(f"  remote HOST : {agent.start(remote_args)}")
        time.sleep(a.lead)
        local_proc = local_start(local_args)
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


def pull_logs(a):
    agent = Agent(a.peer, a.port, a.token)
    os.makedirs(os.path.join(WORK, "local"), exist_ok=True)
    os.makedirs(os.path.join(WORK, "peer"), exist_ok=True)

    data, header = agent.log()
    with open(os.path.join(WORK, "peer", "JERICHO.log"), "wb") as f:
        f.write(data)
    print(f"  peer  log: {len(data)} bytes  ({header})")

    # The peer's DUMP has to be asked for; the log header only says it exists.
    # Pulled before the local one so a crash on the other PC is always in hand --
    # and so a later run starting on that PC cannot overwrite it first.
    if "JERICHO.dmp" in header:
        blob, dheader = agent.dump()

        if blob:
            with open(os.path.join(WORK, "peer", "JERICHO.dmp"), "wb") as f:
                f.write(blob)
            print(f"  peer  DUMP: {len(blob)} bytes ({dheader})")
        else:
            print(f"  peer  dump: asked for it, got nothing ({dheader})")

    src = os.path.join(GAME_DIR, "JERICHO.log")
    if os.path.isfile(src):
        shutil.copyfile(src, os.path.join(WORK, "local", "JERICHO.log"))
        print(f"  local log: {os.path.getsize(src)} bytes")
    else:
        print("  local log: none yet")

    # a crash is a different animal from a clean exit -- say which, always
    for name, d in (("peer", os.path.join(WORK, "peer")), ("local", os.path.join(WORK, "local"))):
        dmp = os.path.join(GAME_DIR, "JERICHO.dmp") if name == "local" else None
        if dmp and os.path.isfile(dmp):
            shutil.copyfile(dmp, os.path.join(d, "JERICHO.dmp"))
        if os.path.isfile(os.path.join(d, "JERICHO.dmp")):
            print(f"  {name}: HAS A CRASH DUMP (an access violation -- not an Alt+F4)")
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
            dirs = pull_logs(a)
            if lp is not None:
                print(f"  verdict: {lp.verdict(verdict_dirs(a, dirs))}")
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
                    help="extra game arguments for both seats")
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
