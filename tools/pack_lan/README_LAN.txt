REDRIVER2 + JERICHO multiplayer -- LAN test package
===================================================

Both machines must run THIS build (the network protocol changed; an older
exe will not talk to it). Unzip it anywhere -- the game writes its log and
config next to the exe, so it does not need to be installed.

WHAT IS IN HERE
   JERICHO.exe, the DLLs        the game
   DRIVER2\                           the game data (the intro/mission FMV
                                      videos are left out; both launchers pass
                                      -nofmv, which the game supports)
   JERICHO\                           the mod loader, the multiplayer mod and
                                      its config (JERICHO\CONFIG\modlist.ini
                                      already has `mp = 1`)

HOW TO PLAY
   Host:    PLAY_HOST.bat            then in the menus choose Las Vegas and
                                     Take a Ride, and start
   Client:  PLAY_JOIN.bat <host-ip>  e.g. PLAY_JOIN.bat 192.168.1.42

   Both machines need the same city/arena; the host's choice is what everyone
   loads, so just start a normal Take a Ride on the host.

PLAYING AGAINST THE BOT (needs no second human)
   You can drive one seat yourself and let a BOT drive the other, on this one
   machine:

       play your host as usual, then run (start the host FIRST, then give it a
       few seconds to come up)

       mp_bot_client.bat                 the bot chases you
       mp_bot_client.bat "" pursuit      the two hunt EACH OTHER
       mp_bot_client.bat "" catmouse     the pair, driven by the pathfinder

   It starts a second copy of the game as a CLIENT with MP_BOT set, so the bot
   drives that car. The mode splits by ROLE -- in `chase` the host flees and the
   joiner chases -- so with the bot on the client it chases whoever is driving the
   host. That is the way to test a chase without a second player.

   Set the mode on the bot copy only. Putting MP_BOT on the host as well makes
   the host FLEE, which is right for a bot-vs-bot run and wrong when you are the
   one driving it.

   Both copies in one folder share JERICHO.log, so that file interleaves and the
   second copy truncates the first one's earlier lines. Read the timestamps, or
   set MP_EXEDIR to a second copy of the game folder to get a clean log from each.
   MP_BOT_DRAW=1 draws the bot's aim and pathfinding on its HUD.

FIREWALL
   On the HOST, inbound TCP + UDP on the session port (1400 here) must be
   allowed. Run FIREWALL_FIX.bat AS ADMINISTRATOR on the host -- it prints the
   current state and opens exactly those two ports.

   The CLIENT's firewall is almost never the problem: joining is an OUTBOUND
   connection, which Windows allows by default. If the join fails, look at the
   host first.

   Three things that look like "the firewall" but are not:
   * Windows keys its allow/block rules to the FULL PATH of the exe. If the
     "allow this app through the firewall" prompt was cancelled, Windows made a
     BLOCK rule for the old folder that still wins. FIREWALL_FIX.bat prints any
     such block rule; remove it if it names a stale path.
   * Windows 11 puts an unfamiliar network on the PUBLIC profile, which blocks
     inbound by default. FIREWALL_FIX.bat adds its rules for every profile, so
     this is covered -- but check the profile it prints.
   * Wi-Fi "AP/client isolation" (common on guest and ISP routers) stops two
     wireless machines from talking to each other at all, whatever the firewall
     says. Test with both machines on ethernet, or over a phone hotspot, to tell
     this apart from a firewall problem.


STAYING ON THE SAME BUILD
   Build it from the project with ONE command:

       JERICHO\MODS\mp\tools\pack_lan\sync_lan.bat

   It regenerates the project files (so the build stamp is current), builds, and
   writes JERICHO_mp_lan_<build>.7z in the project root. Copy that ONE file to
   the other machine and extract it over this folder -- nothing else is needed
   there (no Visual Studio, no git).

   <build> is the same string the game logs at startup
       [mp] multiplayer ready (... build f381, mods 4806)
   and prints on the pause-menu scoreboard, right under "-- PLAYERS --".

   The BUILD stamp is the one that has to agree: it identifies the release series,
   and two machines built from the same series agree on it even when one is a dev
   build and the other is this package. The MODS stamp is a different thing -- it
   follows which modules are enabled -- so a build carrying extra modules shows a
   different one (this package shows `mods b7fe` where a dev build shows `mods
   4806`) and that is NOT a mismatch to fix.

   This package ships JERICHO\CONFIG\mp.ini with `strict_version = 1`, so a
   genuinely different BUILD is REFUSED at join with a clear message, instead of
   the two players silently failing to see each other. To deliberately test
   mismatched builds, set it back to 0 in that file. A different MOD SET is not
   refused: the joining player is told on screen that the host is on a different
   mod set, and the session carries on -- which is the honest answer, because the
   two machines really do not have the same cars and content.

   The usual symptom of a stale exe on one side is a session that HALF works --
   the two players never see each other's car, or one HUD lists a peer that never
   moves. Check the two scoreboards (or the two startup lines) before anything
   else.


WHAT TO EXPECT (the honest v1 limits)
   * The HOST IS THE SESSION. If the host quits, crashes or is disconnected, the
     match ends for everyone on it -- there is no host migration and no reconnect.
     To keep playing, host again. The players who were in it are told the match
     ended and are returned to the frontend.
   * EIGHT players at most, and in practice fewer: each level has five of its own
     cars, two spare slots and one special, so eight players get eight different
     cars and a ninth cannot be seated.
   * ONE gamemode: the stock Take a Ride free-roam. There is no race and no
     mission, and nothing here is a scoreboard -- the pause list is who is in.
   * ONE guest city at a time. You may pick a car from any city, but importing a
     SECOND city's car data can colour the wrong cars and can touch the scenery.
     Stay with one guest city per session.
   * Damage is not shared on a PLAYER car: if your car is badly bent on your
     screen it can look straight on the other player's, and vice versa. Damage to
     TRAFFIC cars IS shared, so a wreck you caused looks the same to both of you.
   * Car-to-car collisions are replicated, but each machine owns its own car's
     response, so a shove lands after one round trip -- you will feel your car
     move a moment after the bump.
   * It is tuned for a LAN. Over the internet at 100-200 ms the other car moves
     in visible steps (there is no prediction or smoothing yet).
   * Traffic and pedestrians are your own: each machine has its own and they will
     not match car for car. The mod replicates the traffic IT owns as state, so a
     car you hit is hit for the other player too -- but it need not be the same
     traffic car on both screens.

PLAYING OVER THE INTERNET, AND WHAT THAT EXPOSES
   The same package works across the open internet by DIRECT CONNECT. On the host,
   forward the session port on your router (TCP, and UDP as well if you want local
   machines to find the game in the browse list) -- 1400 in this package, set by
   `port = ` in JERICHO\CONFIG\mp.ini, which FIREWALL_FIX.bat also opens. Everyone
   else joins by typing the host's public IP or a domain name.

       PLAY_JOIN.bat play.example.net          (or -join <host>[:port])

   THERE IS NO AUTHENTICATION AND NO ENCRYPTION. Anyone who can reach that port
   can join, the game applies no password and no ban list, and everything that
   travels between the machines is plain text. A forwarded port is a public
   invitation: open it while you are playing, for people you trust, and close it
   afterwards. Nothing here leaves your network on a normal LAN.

   IPv4 only -- a name that resolves only to IPv6 will not connect.


IF IT GOES WRONG
   JERICHO.log next to the exe is the game log; it reports the session detail
   (join, launch, resync). A `JERICHO.dmp` next to it is a crash dump.

NOTES
   * The test bot is OFF. Nothing drives your car unless you set MP_BOT -- see
     PLAYING AGAINST THE BOT above for the one-command way to be chased.
   * Take a Ride with the two of you is the supported path for now. Read WHAT TO
     EXPECT above before you play: the notable ones are that the host leaving ends
     the match, that only one guest city is safe, and that player-car damage is not
     shared.
