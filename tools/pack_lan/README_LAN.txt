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
   On the HOST, allow inbound TCP and UDP on the session port (1400 here) --
   Windows will usually ask the first time you run it. Joining by IP needs only
   the TCP port, which is why PLAY_JOIN.bat asks for the address.

IF IT GOES WRONG
   JERICHO.log next to the exe is the game log; it reports the session detail
   (join, launch, resync). A `JERICHO.dmp` next to it is a crash dump.

NOTES
   * The test bot is OFF. Nothing drives your car unless you set MP_BOT.
   * Take a Ride with the two of you is the supported path for now. Car-to-car
     collisions and damage are not replicated yet.
