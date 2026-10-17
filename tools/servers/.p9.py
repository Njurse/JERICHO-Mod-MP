def patch(path, pairs):
    d = open(path, "rb").read().decode("utf-8").replace("\r\n", "\n")
    for old, new in pairs:
        n = d.count(old)
        assert n == 1, (path, n, old[:70])
        d = d.replace(old, new, 1)
    open(path, "wb").write(d.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8"))
    print("patched", path)


patch("dediserver/dediserver.py", [
    ('''    "master_interval_s": 20.0,''',
     '''    "mod_check": 0,            # 0 = admit anyone, 1 = ids must match, 2 = ids AND versions
    "mods": [],                # the ids a client must have enabled when mod_check is on
    "mod_versions": {},        # id -> version, consulted only by mod_check 2
    "master_interval_s": 20.0,'''),

    ("""def normalise_host(value):""",
     '''def parse_manifest(payload):
    """The MP_MOD_INFO rows that ride after an MP_HELLO, as [(id, version, enabled)].

    The manifest travels INSIDE the hello frame rather than as its own message, so a
    short payload is not an error -- it is a client with no mods to declare.
    """
    body = len(m.HELLO)
    rows = []

    for i in range(0, (len(payload) - body) // len(m.MODI)):
        chunk = payload[body + i * len(m.MODI):body + (i + 1) * len(m.MODI)]

        try:
            mod_id, version, enabled, _res = m.MODI.unpack(chunk)
        except Exception:
            break

        rows.append((mod_id.split(b"\0")[0].decode("latin1", "replace"),
                     version.split(b"\0")[0].decode("latin1", "replace"),
                     enabled))

    return rows


def mods_match(declared, required, versions, level):
    """The mod-check policy, exactly as the real host applies it.

    level 0 admits anyone. level 1 compares the SET OF ENABLED IDS (the host ignores
    versions there, because the same mod's version differs between a dev and a
    release build). level 2 compares ids AND versions. A server that names no
    required mods matches everyone, which is what makes leaving the check on
    harmless.
    """
    if int(level) <= 0 or not required:
        return True

    have = set(mod_id for mod_id, _ver, enabled in declared if enabled)

    if have != set(required):
        return False

    if int(level) >= 2:
        for mod_id, version, enabled in declared:
            if enabled and mod_id in versions and versions[mod_id] != version:
                return False

    return True


def normalise_host(value):'''),

    ("""    def _welcome(self, pid, running, seed=0):""",
     '''    def _reject(self, conn, reason, text):
        """Refuse, and let the client READ the reason.

        The old path sent the frame and closed the socket at once, so the peer often
        got 0 bytes back -- "a refused client is told nothing" (TESTING.md, F3). The
        real host half-closes, lets the peer drain, then closes; this does the same,
        and every refusal in this file goes through it.
        """
        try:
            m.send_frame(conn, m.TAG["reject"], m.REJECT.pack(reason, 0, 0, 0, text.encode()))
        except OSError:
            pass

        try:
            conn.shutdown(socket.SHUT_WR)     # half-close: our bytes still arrive
        except OSError:
            pass

        try:
            conn.settimeout(1.0)
            while conn.recv(4096):            # drain what the peer is still sending
                pass
        except OSError:
            pass

        try:
            conn.close()
        except OSError:
            pass

    def _welcome(self, pid, running, matched=1, seed=0):'''),

    ("""        return m.WELCOME.pack(pid, self.max_players, 0, 1, running, 0, 0, 0,
                              int(self.cfg["gamemode"]), self.city, 1, 0, seed, 0xFF, 0)""",
     '''        return m.WELCOME.pack(pid, self.max_players, int(self.cfg["mod_check"]),
                              1 if matched else 0, running, 0, 0, 0,
                              int(self.cfg["gamemode"]), self.city, 1, 0, seed, 0xFF, 0)'''),

    ("""        name = name32.split(b"\0")[0].decode("latin1", "replace") or "Player\"""",
     '''        name = name32.split(b"\0")[0].decode("latin1", "replace") or "Player"

        # The manifest rides after the hello header, modc rows of it.
        mods = parse_manifest(payload)

        # -- the wire has to agree before anything else means anything: a client on
        # a different protocol would misread every frame this server sends it, so it
        # is refused the way the real host refuses it, with the reason readable.
        if _proto != m.PROTO or _sdk != m.SDK:
            self.log.warning("%s: refused, version mismatch (proto %d/%d, sdk %d/%d)"
                             % (who, _proto, m.PROTO, _sdk, m.SDK))
            self._reject(conn, 2, "game/protocol version mismatch")
            return

        # -- and the mod list, if this server enforces one
        matched = mods_match(mods, self.cfg["mods"], self.cfg["mod_versions"],
                             self.cfg["mod_check"])

        if not matched:
            self.log.warning("%s: refused, mod list does not match (declared: %s; "
                             "required: %s)"
                             % (who, ", ".join(sorted(x[0] for x in mods if x[2])) or "none",
                                ", ".join(sorted(self.cfg["mods"]))))
            self._reject(conn, 3, "mod list does not match the server")
            return

        if mods:
            self.log.info("%s declares mods: %s"
                          % (who, ", ".join("%s %s" % (x[0], x[1]) for x in mods if x[2])))'''),

    ("""            if pid is None:
                try:
                    # The real host answers a full server with a REJECT alone --
                    # a WELCOME here would tell the client it got in.
                    m.send_frame(conn, m.TAG["reject"], m.REJECT.pack(1, 0, 0, 0, b"server is full"))
                except OSError:
                    pass
                conn.close()""",
     """            if pid is None:
                # The real host answers a full server with a REJECT alone -- a
                # WELCOME here would tell the client it got in. The half-close in
                # _reject is what lets the reason arrive at all.
                refused_full = True"""),

    ("""        self.log.info("player %d '%s' joined from %s (mods=%d, build %#06x)"
                      % (pid, name, who, modc, build))""",
     """            self._reject(conn, 1, "server is full")
            self.log.warning("refused %s: server is full (%d/%d)"
                             % (who, len(self.clients), self.max_players))
            return

        self.log.info("player %d '%s' joined from %s (mods=%d, build %#06x, "
                      "modsMatched=%d)"
                      % (pid, name, who, modc, build, matched))"""),

    ("""                         self._welcome(pid, running=1 if self.cfg["live"] else 0, seed=seed))""",
     """                         self._welcome(pid, running=1 if self.cfg["live"] else 0,
                                       matched=matched, seed=seed))"""),

    ('''                        "inProgress": 1 if self.cfg["live"] else 0,''',
     '''                        "inProgress": 1 if self.cfg["live"] else 0,
                        # whether this server checks a client's mod manifest, so a
                        # browser can warn before anyone tries to join
                        "modsEnforced": int(self.cfg["mod_check"]),'''),
])
