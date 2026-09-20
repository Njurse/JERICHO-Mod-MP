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


IF IT GOES WRONG
   JERICHO.log next to the exe is the game log; it reports the session detail
   (join, launch, resync). A `JERICHO.dmp` next to it is a crash dump.

NOTES
   * The test bot is OFF. Nothing drives your car unless you set MP_BOT.
   * Take a Ride with the two of you is the supported path for now. Car-to-car
     collisions and damage are not replicated yet.
