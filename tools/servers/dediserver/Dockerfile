# A headless dedicated mp server in one container.
#
# Same three-file requirement as the master (dediserver.py imports proto_check and
# mp_test unguarded, so the wire-mirror guard runs at startup and the image must
# carry the files it reads). Build CONTEXT is the mod dir -- see the master's
# Dockerfile next door for the layout, and ../docker-compose.yml which sets it.
FROM python:3.12-alpine

WORKDIR /mp
COPY mp_proto.h                                   /mp/mp_proto.h
COPY tools/mp_test.py                             /mp/tools/mp_test.py
COPY tools/servers/proto_check.py                 /mp/tools/servers/proto_check.py
COPY tools/servers/dediserver/dediserver.py       /mp/tools/servers/dediserver/dediserver.py
COPY tools/servers/dediserver/dediserver.json     /mp/tools/servers/dediserver/dediserver.json

WORKDIR /mp/tools/servers/dediserver

# the session port (TCP, clients connect here) -- the compose decides the number
EXPOSE 1400/tcp
# the LAN discovery beacon (UDP broadcast); pointless on a VPS, useful on a LAN
EXPOSE 1318/udp

# exec form on purpose -- PID 1 must be the python process so `docker stop`'s
# SIGTERM reaches dediserver's handler, which UNREGISTERS from the master instead
# of leaving a stale listing behind until the TTL lapses. The compose adds the
# deployment flags (port, name, --master, --advertise-host) to this command.
CMD ["python", "-u", "dediserver.py"]
