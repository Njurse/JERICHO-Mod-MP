REMOTE TESTING AGENT
====================

Testing on two computers used to mean copying the package over by hand, starting
each side by hand, and copying a log back by hand. This makes the other PC a
fixture you set up once and then stop thinking about.

ONE-TIME SETUP ON THE OTHER PC
------------------------------
1. Put START_AGENT.bat, mp_agent.ps1 and this file in the game folder -- the one
   with REDRIVER2_dev.exe in it. (The LAN package already puts them there.)
2. Right-click START_AGENT.bat -> Run as administrator, ONCE, so it can be
   allowed through the firewall (TCP 1401). After the first time you can just
   double-click it.
3. Leave the window open. That is the whole setup. Ctrl+C or closing the window
   stops the agent.

You can also set a token so only you can drive it:
      START_AGENT.bat -Token something-private
and then pass --token something-private to mp_remote.py.

THEN, FROM THE MACHINE WITH THE REPO
------------------------------------
    python JERICHO/MODS/mp/tools/remote/mp_remote.py status --peer <other-pc-ip>
    python JERICHO/MODS/mp/tools/remote/mp_remote.py run    --peer <other-pc-ip> --seat host

`status`  says what build it is on, whether the game is up, and how many files
          differ.
`deploy`  sends only what CHANGED (the exe, JERICHO, VERSION.txt -- a few MB, not
          the 1.6 GB of game data), then starts both seats on the same build.
`run`     does the same, waits, pulls BOTH logs back into
          src_rebuild/bin/Release_dev/.mp-remote/ and prints a PASS/FAIL verdict.
`logs`    just pull both logs and say what the verdict is.
`stop`    close the game on both machines (PID-scoped: only the one we started).

WHAT "HANDS-FREE" MEANS
-----------------------
The agent stays resident and, when a sync arrives while a game is running, it
stops the game, verifies and applies the build, and starts the game AGAIN with the
same arguments it had. So you can leave that PC running a client, push a new build
from here, and watch the new build come up on its own.

It verifies every file's SHA256 against the manifest in the package before
replacing anything, and it refuses the whole update if a single file does not
match. Nothing outside the game folder is touched, and nothing is ever deleted.

IF IT WILL NOT CONNECT
----------------------
* "cannot reach the agent" -- the window on the other PC is closed, or the
  firewall is blocking TCP 1401. Run START_AGENT.bat as administrator once.
* "bad token" -- start the agent with the same -Token you pass to mp_remote.py.
* Nothing happens on `start` -- check the agent's own log, mp_agent.log, next to
  the game. It records every command it was given and what it did about it.
