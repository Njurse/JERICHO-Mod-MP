REDRIVER2 + JERICHO -- LAN TEST, 2026-10-05 (package 8938e623)
=============================================================

  git commit : 8938e623   (for provenance; the project's main branch)
  build stamp: build f381, mods 0cca
               -- what the startup line and the pause-menu scoreboard print, and
                  what the two machines compare. It is a digest of the RELEASE
                  SERIES, not of the code, so it does NOT change per commit: two
                  machines on the same release are admitted whatever their build
                  provenance, which is deliberate.

Read README_LAN.txt for hosting/joining/firewall. THIS file says what changed
since the last package and what is worth trying.

Both machines must run THIS unpacked folder (mp.ini ships strict_version = 1, so
a mismatched release is refused at join rather than half-working).

NEW IN THIS PACKAGE
-------------------
* The session port is written down at last. mp.ini now says port = 1400, which is
  the number PLAY_HOST/PLAY_JOIN and the firewall note always claimed. Before it
  did not, and a config port WINS over a -host/-join port, so the host actually
  listened on 1318 while telling the world 1400 -- a join by address could never
  connect, and only LAN discovery found the host. If a join fails now, that is a
  firewall or discovery problem, not this.

* mp_bot_client.bat -- a BOT you can be chased by, on one machine. Start your host,
  run mp_bot_client.bat in this same folder, and the second copy drives itself and
  comes after you (mp_bot_client.bat "" pursuit makes the two hunt each other).
  This is the fast way to exercise car-to-car collisions without a second player.
  You can also use it ALONGSIDE the other machine, if a third seat would help.


WHAT TO EXERCISE
----------------
1. CAR CYCLING (the point of this build). In a session, use the pause menu's
   Change car and cycle through cars from EVERY city this build offers -- the four
   Driver 2 cities (CHICAGO, HAVANA, VEGAS, RIO). Then do it again, and again: the
   bug this fixes only showed after several changes.

   (The five Driver 1 cities -- MIAMI, FRISCO, LA, NEWYORK, NEWCASTLE -- are NOT in
   this package. They ship as a separate Driver 1 content release; see "WHAT IS IN
   THE PACKAGE" below. Do not expect them in the Change car list here.)

   What was wrong before:
     - panels on cars from every city wore a stranger's colours (the engine
       fabricated a missing palette row by copying a NEIGHBOURING row, slot 0
       included);
     - after a few changes the cars went INVISIBLE while their textures still
       uploaded (a mid-session car build quietly exhausted the level's polygon
       arena and every later model built 0 polygons).

   Now: every car should render complete, with its own colours, for as many
   changes as you like -- no fading out, no invisible cars.

2. DYING / FALLING OFF THE MAP. Drive off the world (or drown). Before this
   build it locked into an endless black game-over screen. Now the engine is
   asked "may I end the game over?", the session says no, and you are put back
   on the map with the car repaired, no wanted level and your controls back.

   PLEASE REPORT ANY OF THESE, they are the things most likely to still be off:
     - a respawn that leaves you upside down or under the ground;
     - the camera rolling or flipping after the respawn;
     - the game still showing "your vehicle's wrecked" after you are put back;
     - the controls still locked after the respawn;
     - a wanted level (felony) that survives the respawn.

3. A CHASE against the bot (see NEW IN THIS PACKAGE): can it keep up, does it
   collide cleanly, does anything on either car corrupt after a car change made
   while it is chasing you.

4. THE SINGLE PLAYER / MULTIPLAYER PROMPT (levelhacks). Start a normal
   single-player Take a Ride: after you pick a city and BEFORE the time-of-day
   screen, you should get a Singleplayer / Multiplayer choice. Multiplayer opens
   the four MP-city maps (MP Chicago / MP Havana / MP Las Vegas / MP Rio) and
   starts the chosen one in single-player take-a-ride, so the small multiplayer
   levels can be looked at without a second player.

   In a LAN session the mp mod owns this question instead (its own Single Player /
   Multiplayer menu, leading into the host flow), so while hosting you should see
   that one and NOT this one -- never both on the same city confirm.

5. WHAT IS KNOWN-IMPERFECT (do not chase these, they are the next unit):
   vehicles with EXTRA PANELS -- the Vegas ambulance, large SUVs, long cars --
   can still show minor colour corruption on their extra panels. The engine gives
   a car set eight palette rows, so a set with more pages borrows the first page's
   palette for the extra ones. Cosmetic, known, documented. Likewise CHICAGO is
   where scenery leaks most, and RIO is where vehicle textures are most likely to
   look wrong.


WHAT IS IN THE PACKAGE
----------------------
  JERICHO.exe + DLLs   the game (this build)
  DRIVER2\                 the Driver 2 data, minus the 1.4 GB of FMV
                           (both launchers pass -nofmv)
  (no DRIVER\D1CARS\)      The Driver 1 car content is NOT in this package, so the
                           five Driver 1 cities cannot be selected here. It ships
                           SEPARATELY, as its own release -- never inside this one.
  JERICHO\                 the loader, the four pre-included mods and their
                           config (modlist.ini is carhacks = 1, crumple = 1,
                           levelhacks = 1, mp = 1 -- the only mods a release ships)
  PLAY_HOST.bat / PLAY_JOIN.bat / mp_bot_client.bat / FIREWALL_FIX.bat
  mp_agent.ps1 + START_AGENT.bat   hands-free remote test agent (optional)


SPLITTING A RUN
---------------
You do not both need to be at the keyboard at the same time. One seat can drive
and the other can watch: the log next to the exe, or mp_agent.ps1 on the other PC.


IF IT GOES WRONG
----------------
JERICHO.log next to the exe is the game log. The lines worth grepping after a
car change or a death:

  cross-city: slot N geometry from <CITY> model M      the car you just changed to
  [mp] car status: driving model M (resident N, source <CITY>); mesh loaded
                                                       "MISSING" here = invisible car
  [mp] death in a session: respawned at X,Z ...        the death refusal
  JERICHO: game over REFUSED by a module ...           the engine released the lock
  JERICHO: car model build for slot N produced 0 polys ...  should NEVER appear
  [mp] chase: this seat is the JOINER, so it CHASES     the bot took its seat

JERICHO.dmp next to it is a crash dump; send it with the log.
