#!/usr/bin/env python3
"""Build Vegas MP 2 and MP 3 (the cut Ghost Town / North Las Vegas maps) from the
player's own Driver 2 MISSIONS.BLK.

Every multiplayer Take a Ride map is a mission whose header names a REGION of the
multiplayer level (MLEVELS/VEGAS.LEV) and a player start. Vegas ships two (MP 0 =
M62/M63, MP 1 = M502/M503). Regions 942 and 977 are multiplayer regions only the
2-player Checkpoint races reach (M244 / M245). This copies Take a Ride MP 0's
mission and points it at those regions, with the Checkpoint races' own starts:

    arena 2: M942.D2MS (day)  M943.D2MS (night)   region 942
    arena 3: M1382.D2MS (day) M1383.D2MS (night)  region 977

Arena n is mission 58 + city*2 + night + n*440 (glaunch.c), and the engine loads a
loose MISSIONS\\M<n>.D2MS before MISSIONS.BLK (mission.c LoadMission), so no engine
change and no redistributed game data.

    make_vegas_arenas.py --missions DRIVER2\\MISSIONS          build (idempotent)
    make_vegas_arenas.py --missions DRIVER2\\MISSIONS --check  exit 1 if not current

Exit codes: 0 ok (or already current), 1 --check found them missing or stale,
2 the BLK is missing or not the retail data this expects.
"""
import argparse
import os
import struct
import sys

MISSION_IDENT = (ord('D') << 24) | (ord('2') << 16) | (ord('M') << 8) | ord('S')
VEGAS = 2

# MS_MISSION byte offsets (dr2types.h _MISSION)
OFF_ID = 0
OFF_CITY = 8
OFF_START_X = 20
OFF_START_Z = 24
OFF_ROTATION = 28
OFF_REGION = 72

# (arena, region, start x, start z, rotation, source race) -- the starts are the
# 2-player Checkpoint races' own (M244 region 942, M245 region 977).
ARENAS = (
    (2, 942, -27510, 813508, 3070, 244),
    (3, 977, 176299, 870148, 3070, 245),
)
SOURCE_DAY, SOURCE_NIGHT = 62, 63	# Take a Ride, Vegas MP 0
SOURCE_REGION = 882


def mission_number(city, night, arena):
    return 58 + city * 2 + (1 if night else 0) + arena * 440


def read_blk_mission(blk, n):
    if len(blk) < (n + 1) * 4:
        raise ValueError("MISSIONS.BLK has no entry %d" % n)
    (header,) = struct.unpack_from("<I", blk, n * 4)
    if header == 0:
        raise ValueError("MISSIONS.BLK entry %d is empty" % n)
    offset = header & 0x7FFFF
    length = header >> 19
    data = bytearray(blk[offset:offset + length])
    if len(data) < OFF_REGION + 4 or struct.unpack_from("<i", data, OFF_ID)[0] != MISSION_IDENT:
        raise ValueError("MISSIONS.BLK mission %d is not a mission (wrong id)" % n)
    return data


def read_region(data):
    return struct.unpack_from("<i", data, OFF_REGION)[0]


def build(blk):
    """{filename: bytes} for the four arena missions, checked against the source."""
    out = {}
    for night, src in ((0, SOURCE_DAY), (1, SOURCE_NIGHT)):
        base = read_blk_mission(blk, src)
        city = struct.unpack_from("<i", base, OFF_CITY)[0]
        if city != VEGAS or read_region(base) != SOURCE_REGION:
            raise ValueError("mission %d is not Vegas MP 0 (city %d, region %d) -- "
                             "not the retail MISSIONS.BLK" % (src, city, read_region(base)))
        for arena, region, x, z, rot, race in ARENAS:
            # the race must still name the region, so a different BLK cannot be patched blind
            got = read_region(read_blk_mission(blk, race))
            if got != region:
                raise ValueError("mission %d names region %d, expected %d -- "
                                 "not the retail MISSIONS.BLK" % (race, got, region))
            m = bytearray(base)
            struct.pack_into("<i", m, OFF_REGION, region)
            struct.pack_into("<i", m, OFF_START_X, x)
            struct.pack_into("<i", m, OFF_START_Z, z)
            struct.pack_into("<i", m, OFF_ROTATION, rot)
            out["M%d.D2MS" % mission_number(VEGAS, night, arena)] = bytes(m)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--missions", required=True,
                    help="the game's DRIVER2\\MISSIONS folder (MISSIONS.BLK is read, the maps written there)")
    ap.add_argument("--check", action="store_true", help="only check; exit 1 if missing or stale")
    args = ap.parse_args(argv)

    blk_path = os.path.join(args.missions, "MISSIONS.BLK")
    try:
        with open(blk_path, "rb") as f:
            blk = f.read()
        files = build(blk)
    except (OSError, ValueError) as e:
        print("[vegas] cannot build Vegas MP 2/3: %s" % e)
        return 2

    stale = []
    for name, data in sorted(files.items()):
        path = os.path.join(args.missions, name)
        try:
            with open(path, "rb") as f:
                if f.read() == data:
                    continue
        except OSError:
            pass
        stale.append(name)
        if not args.check:
            tmp = path + ".tmp"
            with open(tmp, "wb") as f:
                f.write(data)
            os.replace(tmp, path)

    if args.check:
        if stale:
            print("[vegas] missing or stale: %s" % ", ".join(stale))
            return 1
        print("[vegas] Vegas MP 2/3 are current")
        return 0

    if stale:
        print("[vegas] wrote %s" % ", ".join(stale))
    else:
        print("[vegas] Vegas MP 2/3 already current")
    return 0


if __name__ == "__main__":
    sys.exit(main())
