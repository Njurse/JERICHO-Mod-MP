"""Every COPY source in the server images must exist in the build context.

Both Dockerfiles are built with the mp MOD DIRECTORY as the context (see
docker-compose.yml), and each one copies a hand-written list of files -- not a
whole tree. So adding a module the servers import, and forgetting a COPY line,
produces an image that builds clean and then dies at import time on the box,
which is the worst place to find out. Nothing else in the checks would notice:
the tests run against the checkout, where the file is simply there.

Run it from anywhere; it works out the paths from its own location.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))        # .../mp/tools/servers
CONTEXT = os.path.dirname(os.path.dirname(HERE))         # .../mp  (the build context)

IMAGES = ("masterserver/Dockerfile", "dediserver/Dockerfile")


def main():
    missing = 0

    for image in IMAGES:
        path = os.path.join(HERE, image)
        print("-- %s" % image)

        if not os.path.exists(path):
            print("   MISSING Dockerfile")
            missing += 1
            continue

        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                m = re.match(r"\s*COPY\s+(\S+)\s+(\S+)", line)

                if not m:
                    continue

                src = m.group(1)
                ok = os.path.exists(os.path.join(CONTEXT, src))
                print("   %-56s %s" % (src, "ok" if ok else "MISSING"))
                missing += 0 if ok else 1

        # ...and the modules the server imports have to be IN the image, which only
        # a COPY line can arrange. A shared module is the classic casualty.
        for name in ("serverkit.py",):
            hits = open(path, "r", encoding="utf-8").read()

            if ("tools/servers/" + name) not in hits:
                print("   NOT COPIED: tools/servers/%s" % name)
                missing += 1

    if missing:
        print("\ncheck_images: FAIL -- %d problem(s); the image would build and "
              "then fail at import" % missing)
        return 1

    print("\ncheck_images: OK -- every COPY source exists and the shared modules "
          "are in both images")
    return 0


if __name__ == "__main__":
    sys.exit(main())
