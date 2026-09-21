#!/usr/bin/env python3
"""Drive a second PC through its mp agent, so a two-machine test is one command.

    python mp_remote.py status  --peer 192.168.50.244
    python mp_remote.py deploy  --peer 192.168.50.244 --seat host
    python mp_remote.py run     --peer 192.168.50.244 --seat host --seconds 90
    python mp_remote.py logs    --peer 192.168.50.244
    python mp_remote.py stop    --peer 192.168.50.244

The other PC runs `mp_agent.ps1` (START_AGENT.bat) once and is then hands-free:
`deploy`/`run` sends it only the FILES THAT CHANGED (the exe, JERICHO, VERSION.txt
-- not the 1.6 GB of game data), and the agent stops any running game, applies the
build and starts it again on its own.

This machine is driven directly (Popen, so a real PID we can kill), which is why
the local seat is reliable and the remote seat goes through the agent.
"""

import argparse
import hashlib
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
import zipfile
import io

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)                       # JERICHO/MODS/mp/tools
REPO = os.path.abspath(os.path.join(TOOLS, "..", "..", "..", ".."))
GAME_DIR = os.path.join(REPO, "src_rebuild", "bin", "Release_dev")
EXE_NAME = "REDRIVER2_dev.exe"
WORK = os.path.join(GAME_DIR, ".mp-remote")          # logs pulled from both seats
DEFAULT_PORT = 1401
DEFAULT_TOKEN = "jericho-mp"
# What a sync may replace. Deliberately NOT DRIVER2/ -- 1.6 GB that never changes
# between builds.
SYNC_SET = (EXE_NAME, "VERSION.txt", "JERICHO")

sys.path.insert(0, TOOLS)
try:
    import mp_localpair as lp        # reuse the localhost pair's verdict logic
except Exception:                    # pragma: no cover
    lp = None


class AgentError(RuntimeError):
    pass


class Agent:
    """A tiny client for mp_agent.ps1: one line in, one line out."""

    def __init__(self, host, port=DEFAULT_PORT, token=DEFAULT_TOKEN, timeout=30):
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

    def _command(self, cmd, rest="", keep_open=False):
        s = self._connect()
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

    def sync(self, blob, name):
        # No "ready" handshake: the agent reads <name>\n<length>\n<payload> as the
        # FIRST thing it does for a sync, so waiting for a reply line before sending
        # the payload deadlocks both ends. Send it straight away and read once.
        s = self._connect()
        s.sendall(f"sync {self.token}\n".encode("ascii"))
        s.sendall(f"{name}\n{len(blob)}\n".encode("ascii"))
        s.sendall(blob)
        reply = self._read_line(s)
        s.close()
        if reply.startswith("ERR "):
            raise AgentError(f"the agent refused the sync: {reply[4:]}")
        return reply

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


# ------------------------------------------------------------------ delta sync

def hash_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def local_files():
    """{relative path -> sha256} for everything a sync may replace."""
    out = {}
    for name in SYNC_SET:
        p = os.path.join(GAME_DIR, name)
        if os.path.isfile(p):
            out[name] = hash_file(p)
        elif os.path.isdir(p):
            for root, _dirs, files in os.walk(p):
                for fn in files:
                    full = os.path.join(root, fn)
                    rel = os.path.relpath(full, GAME_DIR).replace("\\", "/")
                    out[rel] = hash_file(full)
    return out


def make_delta(local, remote):
    """A zip of just the files whose hash differs, plus a manifest to verify."""
    changed = [rel for rel, h in sorted(local.items()) if remote.get(rel) != h]
    gone = [rel for rel in remote if rel not in local]
    if not changed and not gone:
        return None, [], []

    buf = io.BytesIO()
    manifest = []
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for rel in changed:
            z.write(os.path.join(GAME_DIR, rel), rel)
            manifest.append(f"{local[rel]}  {rel}")
        z.writestr("manifest.sha256", "\n".join(manifest) + "\n")
    return buf.getvalue(), changed, gone


def do_sync(agent, quiet=False):
    st = agent.status()
    # Get-FileHash gives UPPERCASE hex and Python gives lowercase; without this the
    # comparison never matches and every sync resends the whole tree.
    remote = {k: v.lower() for k, v in st.get("files", {}).items()}
    local = local_files()
    blob, changed, gone = make_delta(local, remote)

    if blob is None:
        if not quiet:
            print(f"  peer already matches (build {st.get('build')}) -- nothing to send")
        return st, 0

    mb = len(blob) / 1048576
    print(f"  sending {len(changed)} changed file(s), {mb:.2f} MB "
          f"(peer was build {st.get('build')})")
    for rel in changed[:12]:
        print(f"    ~ {rel}")
    if len(changed) > 12:
        print(f"    ... and {len(changed) - 12} more")
    if gone:
        # We never delete on the peer: a leftover file is harmless, a wrong delete
        # is not. Say so rather than silently leaving it.
        print(f"  note: {len(gone)} file(s) exist there but not here -- left alone:")
        for rel in gone[:6]:
            print(f"    - {rel}")

    reply = agent.sync(blob, "REDRIVER2_mp_lan")
    print(f"  {reply}")
    return agent.status(), len(changed)


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
    local = local_files()
    remote = {k: v.lower() for k, v in st.get("files", {}).items()}
    changed = [r for r, h in local.items() if remote.get(r) != h]
    here = "unknown"
    vp = os.path.join(GAME_DIR, "VERSION.txt")
    if os.path.isfile(vp):
        here = open(vp).read().strip()
    print(f"peer {a.peer}:{a.port}")
    print(f"  build there : {st.get('build')}")
    print(f"  build here  : {here}")
    print(f"  game running: {st.get('running')}" + (f"  args: {st.get('args')}" if st.get('running') else ""))
    print(f"  log         : {st.get('logBytes')} bytes    crash dump: {st.get('dump')}")
    print(f"  would sync  : {len(changed)} file(s)")


def cmd_deploy(a):
    agent = Agent(a.peer, a.port, a.token)
    host_ip = a.host_ip or default_ip()
    print(f"deploying to {a.peer}:{a.port}   (local seat = {a.seat}, host is {host_ip})")

    print("1. sync")
    do_sync(agent)

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


def verdict_dirs(a, dirs):
    """mp_localpair.verdict() speaks {"a": host, "b": client}; we know which seat
    THIS machine took, so map onto that rather than passing our own key names
    through (which raised KeyError('a') the first time it was actually run)."""
    if a.seat == "host":
        return {"a": dirs["local"], "b": dirs["peer"]}
    return {"a": dirs["peer"], "b": dirs["local"]}


def pull_logs(a):
    agent = Agent(a.peer, a.port, a.token)
    os.makedirs(os.path.join(WORK, "local"), exist_ok=True)
    os.makedirs(os.path.join(WORK, "peer"), exist_ok=True)

    data, header = agent.log()
    with open(os.path.join(WORK, "peer", "JERICHO.log"), "wb") as f:
        f.write(data)
    print(f"  peer  log: {len(data)} bytes  ({header})")

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
        print(f"  verdict: {lp.verdict(verdict_dirs(a, dirs))}")
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
    ap.add_argument("action", choices=["status", "deploy", "run", "logs", "stop"])
    ap.add_argument("--peer", required=True, help="the other PC's IP")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help="the AGENT's port")
    ap.add_argument("--game-port", type=int, default=1400, dest="port_game",
                    help="the GAME's port")
    ap.add_argument("--token", default=DEFAULT_TOKEN)
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

    try:
        {"status": cmd_status, "deploy": cmd_deploy, "run": cmd_run,
         "logs": cmd_logs, "stop": cmd_stop}[a.action](a)
    except AgentError as e:
        print(f"\nERROR: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
