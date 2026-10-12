#!/usr/bin/env python3
"""A throwaway, read-only SFTP server with password login, for proving the
editor's SFTP self-update path against a REAL server without needing sshd, root
or a hosting account.

    pip install paramiko
    HE_SELFUPDATE_PASSWORD=... scripts/selfupdate_test_sftp_server.py --root DIR

serves DIR as the account's root ("/"), listens on 127.0.0.1 (--port 0 picks a
free one), and prints a single line

    READY port=<n>

on stdout once it accepts connections, so a caller can wait for it. Everything
else goes to stderr. It runs until killed.

Deliberately a stand-in for the update server's *account*, not for a hosting
product: one user, one password (read from the environment, never from argv,
where `ps` would show it), no writes of any kind — an upload, a rename, a mkdir
or a delete is refused, which is exactly how the editor's account should be set
up on the real server. The host key is generated fresh on every start.

Used by the self-update live test (HE_SELFUPDATE_LIVE_TEST=1 he_tests
-tc="selfupdate: live*") and by the CI matrix that runs it on every platform.
"""
import argparse
import os
import socket
import sys
import threading

try:
    import paramiko
    from paramiko import (
        SFTP_OK,
        SFTP_PERMISSION_DENIED,
        SFTP_NO_SUCH_FILE,
        SFTPAttributes,
        SFTPHandle,
        SFTPServer,
        SFTPServerInterface,
        ServerInterface,
    )
except ImportError:
    sys.exit("selfupdate_test_sftp_server.py needs paramiko: pip install paramiko")


def log(message):
    print(message, file=sys.stderr, flush=True)


class Account(ServerInterface):
    def __init__(self, user, password):
        self._user = user
        self._password = password

    def get_allowed_auths(self, username):
        return "password"

    def check_auth_password(self, username, password):
        if username == self._user and password == self._password:
            return paramiko.AUTH_SUCCESSFUL
        log("login refused for user %r" % username)
        return paramiko.AUTH_FAILED

    def check_channel_request(self, kind, chanid):
        if kind == "session":
            return paramiko.OPEN_SUCCEEDED
        return paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED


ROOT = None  # set in main(); the directory served as "/"


def real_path(remote):
    """Maps an SFTP path onto ROOT, or None when it would leave it."""
    remote = remote.replace("\\", "/")
    parts = []
    for part in remote.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            if not parts:
                return None
            parts.pop()
            continue
        parts.append(part)
    candidate = os.path.realpath(os.path.join(ROOT, *parts))
    if candidate != ROOT and not candidate.startswith(ROOT + os.sep):
        return None
    return candidate


class ReadOnlySftp(SFTPServerInterface):
    def list_folder(self, path):
        target = real_path(path)
        if target is None or not os.path.isdir(target):
            return SFTP_NO_SUCH_FILE
        out = []
        for name in sorted(os.listdir(target)):
            attrs = SFTPAttributes.from_stat(os.stat(os.path.join(target, name)), name)
            out.append(attrs)
        return out

    def stat(self, path):
        target = real_path(path)
        if target is None or not os.path.exists(target):
            return SFTP_NO_SUCH_FILE
        return SFTPAttributes.from_stat(os.stat(target))

    lstat = stat

    def open(self, path, flags, attr):
        writing = flags & (os.O_WRONLY | os.O_RDWR | os.O_APPEND | os.O_CREAT | os.O_TRUNC)
        if writing:
            log("refused a write to %s" % path)
            return SFTP_PERMISSION_DENIED
        target = real_path(path)
        if target is None or not os.path.isfile(target):
            return SFTP_NO_SUCH_FILE
        handle = SFTPHandle(flags)
        handle.filename = target
        handle.readfile = open(target, "rb")
        log("serving %s" % path)
        return handle

    def remove(self, path):
        return SFTP_PERMISSION_DENIED

    def rename(self, oldpath, newpath):
        return SFTP_PERMISSION_DENIED

    def mkdir(self, path, attr):
        return SFTP_PERMISSION_DENIED

    def rmdir(self, path):
        return SFTP_PERMISSION_DENIED

    def chattr(self, path, attr):
        return SFTP_PERMISSION_DENIED

    def canonicalize(self, path):
        # The client asks for "." once; answer with the account's root.
        target = real_path(path)
        if target is None:
            return "/"
        rel = os.path.relpath(target, ROOT)
        return "/" if rel == "." else "/" + rel.replace(os.sep, "/")


def serve_connection(client, host_key, user, password):
    transport = paramiko.Transport(client)
    try:
        transport.add_server_key(host_key)
        transport.set_subsystem_handler("sftp", SFTPServer, ReadOnlySftp)
        transport.start_server(server=Account(user, password))
        while transport.is_active():
            transport.join(1.0)
    except Exception as exc:  # a client hanging up mid-handshake is routine
        log("connection ended: %s" % exc)
    finally:
        transport.close()


def main():
    global ROOT
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", required=True, help="directory served as the account's root")
    ap.add_argument("--user", default="editor-ro")
    ap.add_argument("--port", type=int, default=0, help="0 = pick a free port")
    ap.add_argument("--password-env", default="HE_SELFUPDATE_PASSWORD",
                    help="name of the environment variable holding the password")
    args = ap.parse_args()

    password = os.environ.get(args.password_env, "")
    if not password:
        sys.exit("set %s to the password the test account should accept" % args.password_env)
    if not os.path.isdir(args.root):
        sys.exit("--root is not a directory: %s" % args.root)
    ROOT = os.path.realpath(args.root)

    host_key = paramiko.RSAKey.generate(2048)

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", args.port))
    listener.listen(16)
    print("READY port=%d" % listener.getsockname()[1], flush=True)

    while True:
        client, _addr = listener.accept()
        threading.Thread(target=serve_connection, args=(client, host_key, args.user, password),
                         daemon=True).start()


if __name__ == "__main__":
    main()
