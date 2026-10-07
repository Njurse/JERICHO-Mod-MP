REMOTE TESTING AGENT
====================

Testing on two computers used to mean copying the package over by hand, starting
each side by hand, and copying a log back by hand. This makes the other PC a
fixture you set up once and then stop thinking about.

The agent never accepts a build from the network. When you ask it to update, it
downloads a GitHub RELEASE of the repo itself (the rolling 'alpha' pre-release
that CI refreshes from main, or a tag such as v0.9.1), checks the download's
SHA256 against the digest GitHub publishes for that asset, and only then installs
it. The only thing an update command carries is the release tag.

ONE-TIME SETUP ON THE OTHER PC
------------------------------
1. Put START_AGENT.bat, mp_agent.ps1 and this file in the game folder -- the one
   with JERICHO_dev.exe in it. (The LAN package already puts them there.)
2. Right-click START_AGENT.bat -> Run as administrator, ONCE, with that PC's LAN
   address, so it can be allowed through the firewall:
         START_AGENT.bat -Bind 192.168.1.20
   The firewall rules (TCP 1401 for the agent, TCP 1400 for the game) only apply
   to the PRIVATE network profile and to the local subnet, so set that network to
   Private in Windows. After the first time you can just run it normally.
3. The first start generates a random token, prints it, and keeps it in
   mp_agent.config.json next to the game. Note it down: mp_remote.py needs it.
   (It is never written to mp_agent.log. Delete the config file to get a new one.)
4. Leave the window open. That is the whole setup. Ctrl+C or closing the window
   stops the agent.

Without -Bind the agent listens on 127.0.0.1 only, which nothing else can reach.
It refuses to listen on every interface at once (0.0.0.0) -- name the address.
It also refuses the old published default token 'jericho-mp'; if you want your own
token instead of the generated one, pass -Token <16+ characters>.

Releases come from Njurse/JERICHO unless you say otherwise:
      START_AGENT.bat -Bind 192.168.1.20 -Repo someone/JERICHO
(or put "repo": "someone/JERICHO" in mp_agent.config.json).

THEN, FROM THE MACHINE WITH THE REPO
------------------------------------
    set MP_AGENT_TOKEN=<the token the other PC printed>
    python JERICHO/MODS/mp/tools/remote/mp_remote.py status --peer <other-pc-ip>
    python JERICHO/MODS/mp/tools/remote/mp_remote.py run    --peer <other-pc-ip> --seat host

`status`   says what build it is on (and which release, verified how), whether a
           rollback point exists, and whether the game is up.
`update`   has it install a release: 'alpha' by default, or --tag v0.9.1.
`deploy`   updates it (skip with --no-update), then starts both seats.
`run`      does the same, waits, pulls BOTH logs back into
           src_rebuild/bin/Release_dev/.mp-remote/ and prints a PASS/FAIL verdict.
`logs`     just pull both logs and say what the verdict is.
`rollback` put back the build the last update replaced.
`stop`     close the game on both machines (PID-scoped: only the one we started).

Note: the other PC runs a published release, while this machine runs whatever you
built locally. If they are not the same commit, a match with strict_version will
tell you so -- push to main (or tag) and let CI publish first.

WHAT AN UPDATE DOES
-------------------
1. Asks the GitHub API (HTTPS) for the release and finds the Release
   Windows zip, JERICHO_Release_win64.zip (the one with JERICHO.exe).
   That is the ONLY build it installs: every other asset is refused -- by name,
   and again if an archive does not carry JERICHO.exe.
2. Gets the expected SHA256 from GitHub's asset digest -- or, if a release has
   none, from a SHA256SUMS file published beside the zips. Never from
   anything inside the zip. With neither, it refuses to install.
3. Downloads into _mp_staging, compares the hash, and unpacks there. Any
   mismatch, or a zip entry that would land outside the staging folder, and the
   whole update is refused with the live build untouched.
4. Stops the game if it is running, swaps the new files in (JERICHO_dev.exe and
   its .pdb/.map, SDL2.dll, OpenAL32.dll, the JERICHO folder, VERSION.txt), and
   moves what they replaced to _mp_previous. Your CONFIG files are kept, and the
   game data (DRIVER2) and config.ini are never touched.
5. Starts the game AGAIN with the same arguments it had, so you can leave that PC
   running a client and watch the new build come up on its own.

`rollback` swaps _mp_previous back in (running it again swaps forward again).

You can also install a release on that PC by hand, without the listener:
      powershell -NoProfile -ExecutionPolicy Bypass -File mp_agent.ps1 -InstallRelease -Tag alpha
      powershell -NoProfile -ExecutionPolicy Bypass -File mp_agent.ps1 -Rollback

IF IT WILL NOT CONNECT
----------------------
* "cannot reach the agent" -- the window on the other PC is closed, it was
  started without -Bind <its LAN address>, its network is not set to Private, or
  the firewall is blocking TCP 1401. Run START_AGENT.bat as administrator once.
* "bad token" -- pass the token from that PC's mp_agent.config.json.
* "sync ... was removed" -- an old mp_remote.py; pull the repo.
* An update fails -- the reason is in the reply and in mp_agent.log. GitHub
  allows 60 API requests an hour per address without a login, which a few
  updates never get near.
* Nothing happens on `start` -- check the agent's own log, mp_agent.log, next to
  the game. It records every command it was given (minus the token) and what it
  did about it.
* `start` fails with "Cannot validate argument on parameter 'ArgumentList'. The
  argument is null or empty" -- that is an mp_agent.ps1 from before 2026-10-04.
  It handed Start-Process an empty argument list to mean "no arguments", and
  Windows PowerShell 5.1 rejects an empty collection outright, so a start with no
  arguments could never launch the game (nor could a restart-after-update of a
  game that had been started that way). Fixed in this copy: the parameter is left
  out entirely when there is nothing to pass.
