#!/usr/bin/env python3
"""player_id.py -- print this install's multiplayer player id (for a server's admins list).

The game keeps the id in JERICHO/CONFIG/player.id (written at first launch; see
mp_config.c for the format). This reads it and prints the dashed UUID:

    python player_id.py                      # finds ../../../CONFIG/player.id
    python player_id.py path/to/player.id

Paste the printed id into dediserver.json:  "admins": ["<id>"]

The file is obfuscated and checksummed with a key that ships in the game, so it
resists casual editing; it is NOT secret from anyone who has the file.
"""
import os
import struct
import sys

MAGIC = b"JRID"
VERSION = 1
SIZE = 32
KEY = 0x4A45524943484F31          # "JERICHO1" (mp_config.c MP_ID_KEY)
M64 = (1 << 64) - 1


def _splitmix(state):
    state = (state + 0x9E3779B97F4A7C15) & M64
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
    return state, z ^ (z >> 31)


def _crypt(data, nonce):
    st, out, r = KEY ^ nonce, bytearray(data), 0
    for i in range(len(out)):
        if i % 8 == 0:
            st, r = _splitmix(st)
        out[i] ^= (r >> ((i % 8) * 8)) & 0xFF
    return bytes(out)


def _check(head12, plain):
    h = 2166136261
    for b in KEY.to_bytes(8, "little") + head12 + plain:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def decode(blob):
    """The 16 id bytes from a player.id file's contents; ValueError if invalid."""
    if len(blob) != SIZE or blob[:4] != MAGIC or blob[4] != VERSION:
        raise ValueError("not a JERICHO player.id file")
    nonce = struct.unpack_from("<I", blob, 8)[0]
    plain = _crypt(blob[12:28], nonce)
    if _check(blob[:12], plain) != struct.unpack_from("<I", blob, 28)[0]:
        raise ValueError("player.id is damaged (checksum mismatch)")
    return plain


def encode(raw, nonce):
    """The file for 16 id bytes (tests, and a server tool that wants to mint one)."""
    head = MAGIC + bytes([VERSION, 0, 0, 0]) + struct.pack("<I", nonce)
    return head + _crypt(bytes(raw), nonce) + struct.pack("<I", _check(head, bytes(raw)))


def uuid_text(raw):
    h = bytes(raw).hex()
    return "%s-%s-%s-%s-%s" % (h[:8], h[8:12], h[12:16], h[16:20], h[20:])


def main(argv):
    here = os.path.dirname(os.path.abspath(__file__))
    path = argv[1] if len(argv) > 1 else os.path.normpath(
        os.path.join(here, "..", "..", "..", "CONFIG", "player.id"))
    try:
        with open(path, "rb") as f:
            print(uuid_text(decode(f.read())))
    except FileNotFoundError:
        print("no %s -- start the game once (with the mp module enabled) to create it" % path,
              file=sys.stderr)
        return 1
    except ValueError as e:
        print("%s: %s" % (path, e), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
