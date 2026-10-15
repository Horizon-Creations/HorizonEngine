#!/usr/bin/env python3
"""he_team_sync_backup — the GitHub backup of a team the acceptance run made (topic 199).

scripts/he_team_sync_acceptance.py leaves its team (he-e2e-199-NNNNNN, members Anna
and Bob, project "game") on the server. This drives that project's backup as Anna,
without editors, on the protocol alone:

  he_team_sync_backup.py set    TEAM URL   BACKUP_SET, prints the deploy key to register
  he_team_sync_backup.py big    TEAM       commit a 60 MiB .png (goes to GitHub as LFS)
  he_team_sync_backup.py huge   TEAM       try a 120 MiB .bin (must be refused, names it)
  he_team_sync_backup.py status TEAM       head and BACKUP_STATUS

The server is the rented host unless HE_TEAM_SERVER=HOST:PORT says otherwise.
"""
import hashlib
import os
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import he_team_sync_acceptance as acc  # noqa: E402
from he_team_sync_two_editors import frame, recv_frame  # noqa: E402

HOST, PORT = os.environ.get("HE_TEAM_SERVER", "212.227.140.164:7777").rsplit(":", 1)
PORT = int(PORT)
PUT, COMMIT = 5, 6
CHUNK = 1024 * 1024


def stream(rid, data):
    out, off = [], 0
    while True:
        part = data[off:off + CHUNK]
        last = off + len(part) >= len(data)
        out.append(frame(13, struct.pack(">IQB", rid, off, 1 if last else 0) + part))
        off += len(part)
        if last:
            return b"".join(out)


def call(c, rid, op, fields, data):
    c.sendall(frame(11, struct.pack(">I", rid) + bytes([op]) + fields) + stream(rid, data))
    got = b""
    while True:
        ftype, body = recv_frame(c)
        if ftype == 13:
            got += body[13:]
        elif ftype == 12:
            _, result, n = struct.unpack(">IBH", body[:7])
            return result, body[7:7 + n].decode(errors="replace"), body[7 + n:], got


def upload(obs, path, content):
    digest = hashlib.sha256(content).digest()
    head = obs.head()
    c = obs._open()
    c.settimeout(600)   # a socket timeout bounds the whole sendall: 120 MiB over a home uplink
    try:
        r = call(c, 1, PUT, digest + struct.pack(">Q", len(content)), content)
        print("PUT", r[0], r[1])
        msg = b"Topic 199 step 5: backup check"
        lines = ("A\t%s\t-\t%s\n" % (path, digest.hex())).encode()
        r = call(c, 2, COMMIT, struct.pack(">QH", head, len(msg)) + msg, lines)
        print("COMMIT", r[0], r[1], r[3].decode(errors="replace").strip())
    finally:
        c.close()


def main():
    cmd, team = sys.argv[1], sys.argv[2]
    obs = acc.Observer(HOST, PORT, team, "game", "Anna", "anna-password")
    if cmd == "set":
        print(obs.backup_set(sys.argv[3]))
    elif cmd == "big":
        upload(obs, "Content/E2E_Big.png", b"\x89PNG\r\n\x1a\n" + os.urandom(60 * 1024 * 1024))
    elif cmd == "huge":
        upload(obs, "Content/E2E_Huge.bin", os.urandom(120 * 1024 * 1024))
    print("head", obs.head(), obs.backup_status())


if __name__ == "__main__":
    main()
