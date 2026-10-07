# pack_lan — making a build you can hand to someone else

Everything needed to turn a built tree into a folder (or a zip) that another
person can unpack and play. There are **two** packagers here, and which one you
want depends on whether Driver 1 content is going with it.

| You want… | Use | Ships |
| --- | --- | --- |
| the public LAN package | `make_lan_package.bat` | the **Release** exe, four mods (carhacks, crumple, levelhacks, mp), the game data minus its movies |
| a private hand-off, with Driver 1 | `make_lan_package_micheal.sh` | the **dev** exe, **six** mods (those four plus d1cars and sandbox), no game data at all |

## Why there are two, and why they cannot be one

`make_lan_package.bat` packages `bin/Release`, and the Release exe **cannot carry
d1cars** — it is built against `JERICHO_RELEASE_MODS` in `premake5.lua`, which
pre-includes four modules and leaves d1cars out so the Driver 1 content can be
revealed on its own. The Driver 1 module and its baked data are therefore only
reachable from a **dev** build, which is what the second packager uses.

The dev build also mirrors every module into `bin/Release_dev/JERICHO/MODS`,
*except* d1cars and gaildrv2 (`JER_MIRROR_EXCLUDE`). That exclusion matters more
than it looks: the loader reads `MODS/<id>/mod.toml` **even for a module compiled
into the exe** (`jer_loader.c`), so a package with d1cars enabled but no d1cars
folder has a module with no metadata to load with. The Micheal packager copies
that one folder out of the repo for exactly this reason.

## The public package

    JERICHO\MODS\mp\tools\pack_lan\make_lan_package.bat [output.7z]

Ships `JERICHO.exe`, the DLLs, `config.ini`, `VERSION.txt`, `DRIVER2` and
`JERICHO` — minus `DRIVER2\FMV`, which is 1.4 GB of movies the launchers skip
with `-nofmv`. It stages an `mp.ini` that pins the session port and refuses a
build mismatch, and it relies on the Release build having written its own
four-module `modlist.ini`.

## The private hand-off (Driver 1)

    bash JERICHO/MODS/mp/tools/pack_lan/make_lan_package_micheal.sh

A different shape on purpose, matching the earlier hand-offs:

* **No `DRIVER2\`** — the recipient already has the game data, so the zip is
  ~11 MB instead of ~1 GB. It must be unpacked **beside** their existing
  `DRIVER2\`, and `PACKAGE_NOTES.txt` inside says so in the first paragraph.
* the **dev** exe, staged as `JERICHO.exe` so the launchers and readmes are the
  same files the public package ships;
* `DRIVER\D1CARS\`, the Driver 1 car content, plus `JERICHO\MODS\d1cars`;
* `JERICHO\CONFIG\modlist.ini` with six modules on (see
  `micheal_package/modlist.ini`) and `JERICHO\CONFIG\mp.ini` carrying the
  recipient's player name (see `micheal_package/mp.ini`);
* and it strips what must never ship: the d1cars submodule's `.git`, and this
  machine's own `build.log`, `*.old` and stale `modlist.ini.bak-*`. The script
  fails the build if any of them reach the archive.

Both packagers stage into a temp folder and add the launchers, the readmes and
the remote-testing agent by name, so the two packages differ only in the ways
listed above.

## What is in this folder besides the packagers

| File | What it is |
| --- | --- |
| `PLAY_HOST.bat` / `PLAY_JOIN.bat` | the launchers the recipients run; `PLAY_HOST.bat [port]`, `PLAY_JOIN.bat <host-ip>` |
| `mp_bot_client.bat` | a second, self-driving copy of the game to be chased by |
| `FIREWALL_FIX.bat` | adds the inbound firewall rule for the session port |
| `README_LAN.txt` | the one-page "how two people play" |
| `README_LAN_TEST.txt` | the checklist of what to exercise and what "working" looks like |
| `PACKAGE_NOTES_micheal.txt` | notes from an **earlier** hand-off (no Driver 1 in it) |
| `PACKAGE_NOTES_micheal_d1cars.txt` | notes for the Driver 1 hand-off; shipped as `PACKAGE_NOTES.txt` |
| `sync_lan.bat` | rebuilds the Release locally the way CI does (the two-pass `exports.def` dance) |
| `micheal_package/` | the tracked inputs the hand-off stages: `mp.ini` and `modlist.ini` |
