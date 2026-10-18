#!/usr/bin/env python3
"""dediserver.py -- a headless dedicated server for the JERICHO mp mod.

It hosts a real session with no game window: it accepts players, hands out ids,
relays their car-state and chat, answers PING with PONG, and optionally beacons
on UDP so the in-game LAN browser lists it. A client has something to join and
the server can be left up while the client side restarts.

    python dediserver.py                       # TCP + UDP on 1318, beaconing
    python dediserver.py --config dediserver.json
    python dediserver.py --port 1400 --name "Bob's 24/7" --no-beacon
    python dediserver.py --master http://master.example.com:8100

Then point a client at it:

    JERICHO_dev.exe -nointro -nofmv -join 127.0.0.1
    JERICHO_dev.exe -nointro -nofmv -join 127.0.0.1:1400

The wire format lives in mp_test.py (imported so there is only one copy), and
servers/proto_check.py fails at startup if that mirror has drifted from
mp_proto.h. With --master the server registers with a master discovery service
(servers/masterserver/) and heartbeats, so internet clients can find it.
"""

# The server lives in the dedi/ package beside this file; this module stays the
# entry point (launch scripts, Dockerfile, packaging and tests all name it) and
# re-exports the package's public names so `import dediserver` keeps working.
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from dedi.core import *                       # noqa: E402,F401,F403
from dedi.core import _chat_chunks, _ink_bytes, main   # noqa: E402,F401


if __name__ == "__main__":
    sys.exit(main())
