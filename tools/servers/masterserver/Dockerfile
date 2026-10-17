# The master discovery service, in one container.
#
# It is a single stdlib-only Python file, BUT it refuses to start if the Python
# wire mirror has drifted from mp_proto.h (proto_check.assert_ok at startup), so
# the image carries the three files that check reads -- and nothing else.
#
# The build CONTEXT is the mod dir (JERICHO/MODS/mp), not this directory, so those
# files land where proto_check looks for them relative to itself:
#
#   /mp/mp_proto.h                    <- <mp>/mp_proto.h
#   /mp/tools/mp_test.py              <- <mp>/tools/mp_test.py
#   /mp/tools/servers/proto_check.py  <- ...
#
# See ../docker-compose.yml, which sets the context for you.
FROM python:3.12-alpine

WORKDIR /mp
COPY mp_proto.h                            /mp/mp_proto.h
COPY tools/mp_test.py                      /mp/tools/mp_test.py
COPY tools/servers/proto_check.py          /mp/tools/servers/proto_check.py
COPY tools/servers/masterserver/masterserver.py   /mp/tools/servers/masterserver/masterserver.py
COPY tools/servers/masterserver/masterserver.json /mp/tools/servers/masterserver/masterserver.json

WORKDIR /mp/tools/servers/masterserver

# documentation only: with host networking the port is the host's (see the compose)
EXPOSE 8100

# exec form on purpose -- the python process must be PID 1 so that `docker stop`'s
# SIGTERM reaches it. Anything in between (sh -c, a wrapper) would eat the signal
# and a stopped master would skip its shutdown path. -u so `docker compose logs`
# is live rather than buffered.
#
# --config is explicit because, unlike the dedi, this one does NOT look for its
# JSON beside itself: with no --config it runs on the built-in defaults.
CMD ["python", "-u", "masterserver.py", "--config", "masterserver.json"]
