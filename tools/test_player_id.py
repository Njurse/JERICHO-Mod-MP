"""player_id.py against the game's CONFIG/player.id format (mp_config.c).

C_VECTOR was written by mp_config.c's MpIdFileWrite itself (a harness around the
real code, nonce bytes 0b 30 55 7a, id a0..af), so this pins the Python decoder to
the C writer: if either side's cipher or checksum drifts, this fails.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import player_id as p   # noqa: E402

C_VECTOR = bytes.fromhex("4a524944010000000b30557ad84a6518adfb8e5412f77e5ac73df40540ab35ed")
ID = bytes(range(0xA0, 0xB0))


def main():
    fails = 0

    def check(name, ok):
        nonlocal fails
        print("[%s] %s" % ("PASS" if ok else "FAIL", name))
        fails += 0 if ok else 1

    check("decodes the file the C writer produced", p.decode(C_VECTOR) == ID)
    check("encode reproduces it byte for byte", p.encode(ID, 0x7A55300B) == C_VECTOR)
    check("dashed UUID text", p.uuid_text(ID) == "a0a1a2a3-a4a5-a6a7-a8a9-aaabacadaeaf")
    tampered = bytearray(C_VECTOR)
    tampered[15] ^= 1
    for name, blob in (("a flipped id bit", bytes(tampered)), ("a short file", C_VECTOR[:31]),
                       ("a wrong magic", b"XXXX" + C_VECTOR[4:])):
        try:
            p.decode(blob)
            check("rejects " + name, False)
        except ValueError:
            check("rejects " + name, True)
    check("the id bytes are not stored in the clear", ID not in C_VECTOR)
    print("%d failure(s)" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
