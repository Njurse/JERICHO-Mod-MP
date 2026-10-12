# pack_lan — making a build you can hand to someone else

Everything needed to turn a built tree into a folder (or a zip) that another
person can unpack and play. There are **two** packagers here, and which one you
want depends on whether Driver 1 content is going with it.

| You want… | Use | Ships |
| --- | --- | --- |
| the public LAN package | `make_lan_package.bat` | the **Release** exe, four mods (carhacks, crumple, d1cars, mp), the game data minus its movies, plus `BAKE_D1_CARS.bat` / `BAKE_D1_CARS.sh` and the d1cars bake tools |
| a private hand-off, with Driver 1 | `make_lan_package_micheal.sh` | the **dev** exe, **six** mods (those four plus levelhacks and sandbox), no game data at all |

## Why there are two, and why they cannot be one

`make_lan_package.bat` packages `bin/Release`, and that exe carries the four
pre-included modules (`JERICHO_RELEASE_MODS` in `premake5.lua`): carhacks, crumple,
d1cars and mp. **The d1cars module ships; the Driver 1 car DATA does not.** It is
baked out of Driver 1's copyrighted files, so the package carries the way to make it
instead -- the bake tools plus `BAKE_D1_CARS.bat` / `BAKE_D1_CARS.sh` at the package
root, which read the player's own Driver 1 data from `DRIVER\` and write
`DRIVER\D1CARS\`.

That is why the packager copies `JERICHO\MODS\d1cars\tools` out of the REPO: the dev
mirror excludes d1cars (premake `JER_MIRROR_EXCLUDE`), so `bin\Release` never has it
and the package would otherwise ship with no way to bake. The bake script comes in two
flavours, `.bat` for Windows and `.sh` for Linux/macOS, so whichever machine you are on
one of them runs. The CI release archives are staged the same way (see `docs/CI.md`).

The rest of `DRIVER\` is out either way -- it is the Driver 1 game's own levels and
586 MB of movies, which the car cities do not read.

carhacks only offers a D1 city whose data is on disk (`chkCityAssetsPresent`, which
tests `DRIVER\D1CARS\<CITY>.LEV`), so before a player bakes, the five cities are simply
not offered -- the feature degrades to absent rather than to broken. A package with no
`DRIVER\D1CARS\` and no tools would look the same as one where the player has not baked
yet, which is why the packager warns rather than fails when the tools are missing.

The private packager exists for the things a public release does not carry: the
`levelhacks` and `sandbox` modules, and a dev exe. It also stages `DRIVER\D1CARS\`
itself, which the public one deliberately does not (below).

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
| `BAKE_D1_CARS.bat` / `.sh` | the Driver 1 bake, run once by a player who owns Driver 1; identical steps, one per platform |
| `update_jericho.bat` / `.sh` | updates an install from the latest **release** (never the `alpha` pre-release) after checking it against `SHA256SUMS`; keeps `CONFIG/`, `DRIVER/` and any mod you added. `update_jericho.bat dev` / `./update_jericho.sh dev` takes the release's `Release_dev` build instead |
| `mp_bot_client.bat` | a second, self-driving copy of the game to be chased by |
| `FIREWALL_FIX.bat` | adds the inbound firewall rule for the session port |
| `README_LAN.txt` | the one-page "how two people play" |
| `README_LAN_TEST.txt` | the checklist of what to exercise and what "working" looks like |
| `PACKAGE_NOTES_micheal.txt` | notes from an **earlier** hand-off (no Driver 1 in it) |
| `PACKAGE_NOTES_micheal_d1cars.txt` | notes for the Driver 1 hand-off; shipped as `PACKAGE_NOTES.txt` |
| `sync_lan.bat` | rebuilds the Release locally the way CI does (the two-pass `exports.def` dance) |
| `micheal_package/` | the tracked inputs the hand-off stages: `mp.ini` and `modlist.ini` |
