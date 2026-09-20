REDRIVER2 + JERICHO multiplayer -- LAN test package
===================================================

Both machines must run THIS build (the network protocol changed; an older
exe will not talk to it). Unzip it anywhere -- the game writes its log and
config next to the exe, so it does not need to be installed.

WHAT IS IN HERE
   REDRIVER2_dev.exe, the DLLs        the game
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
   writes REDRIVER2_mp_lan_<build>.7z in the project root. Copy that ONE file to
   the other machine and extract it over this folder -- nothing else is needed
   there (no Visual Studio, no git).

   <build> is the same string the game logs at startup
       [mp] multiplayer ready (... build be0d, mods 13bd)
   and prints on the pause-menu scoreboard, right under "-- PLAYERS --". If those
   two do not match, the two machines are not on the same build.

   This package ships JERICHO\CONFIG\mp.ini with `strict_version = 1`, so a
   mismatched build is REFUSED at join with a clear message, instead of the two
   players silently failing to see each other. To deliberately test mismatched
   builds, set it back to 0 in that file.

   The usual symptom of a stale exe on one side is a session that HALF works --
   the two players never see each other's car, or one HUD lists a peer that never
   moves. Check the two scoreboards (or the two startup lines) before anything
   else.


IF IT GOES WRONG
   JERICHO.log next to the exe is the game log; it reports the session detail
   (join, launch, resync). A `JERICHO.dmp` next to it is a crash dump.

NOTES
   * The test bot is OFF. Nothing drives your car unless you set MP_BOT.
   * Take a Ride with the two of you is the supported path for now. Car-to-car
     collisions are replicated -- each machine owns its own car's response, so a
     shove lands after one round trip -- but damage is not.
