# Throwaway local SFTP server for test_sftp_connection.cpp's live case (CSY-101).
# Password auth, files rooted under ROOT, logs one line per accepted TCP session
# (so "how many handshakes did that batch cost" is a grep).
#
#   python3 -m venv /tmp/sftpvenv && /tmp/sftpvenv/bin/pip install paramiko
#   mkdir -p /tmp/sftproot/content
#   /tmp/sftpvenv/bin/python -I scripts/sftp_test_server.py 2222 /tmp/sftproot tester pw123
#   HE_TEST_SFTP=127.0.0.1:2222:tester:pw123:/content he_tests --source-file='*test_sftp_connection.cpp'
#
# Restarting this process while a client holds an SftpConnection is the reconnect
# witness: the next operation must reconnect once and succeed.
#
# Trap: the channel returned by Transport.accept() must stay referenced. A
# collected channel closes, and libssh2_sftp_init then waits forever, because
# the SFTP client has no libssh2 timeout (CSY-102).
import os, socket, sys, threading
import paramiko
from paramiko import ServerInterface, SFTPServerInterface, SFTPServer, SFTPAttributes, SFTPHandle, SFTP_OK, AUTH_SUCCESSFUL, AUTH_FAILED, OPEN_SUCCEEDED

PORT, ROOT, USER, PASSWORD = int(sys.argv[1]), os.path.realpath(sys.argv[2]), sys.argv[3], sys.argv[4]
HOST_KEY = paramiko.RSAKey.generate(2048)
sessions = 0
lock = threading.Lock()


class Server(ServerInterface):
    def check_auth_password(self, username, password):
        return AUTH_SUCCESSFUL if (username, password) == (USER, PASSWORD) else AUTH_FAILED

    def get_allowed_auths(self, username):
        return "password"

    def check_channel_request(self, kind, chanid):
        return OPEN_SUCCEEDED


class Handle(SFTPHandle):
    def stat(self):
        try:
            return SFTPAttributes.from_stat(os.fstat(self.readfile.fileno()))
        except OSError as e:
            return SFTPServer.convert_errno(e.errno)


class Sftp(SFTPServerInterface):
    def _real(self, path):
        return os.path.join(ROOT, os.path.normpath("/" + path).lstrip("/"))

    def list_folder(self, path):
        p = self._real(path)
        try:
            out = []
            for name in os.listdir(p):
                a = SFTPAttributes.from_stat(os.stat(os.path.join(p, name)))
                a.filename = name
                out.append(a)
            return out
        except OSError as e:
            return SFTPServer.convert_errno(e.errno)

    def stat(self, path):
        try:
            return SFTPAttributes.from_stat(os.stat(self._real(path)))
        except OSError as e:
            return SFTPServer.convert_errno(e.errno)

    lstat = stat

    def open(self, path, flags, attr):
        p = self._real(path)
        try:
            fd = os.open(p, flags | getattr(os, "O_BINARY", 0), 0o644)
        except OSError as e:
            return SFTPServer.convert_errno(e.errno)
        if flags & os.O_WRONLY:
            mode = "ab" if flags & os.O_APPEND else "wb"
        elif flags & os.O_RDWR:
            mode = "a+b" if flags & os.O_APPEND else "r+b"
        else:
            mode = "rb"
        f = os.fdopen(fd, mode)
        h = Handle(flags)
        h.filename = p
        h.readfile = f
        h.writefile = f
        return h

    def mkdir(self, path, attr):
        try:
            os.mkdir(self._real(path))
        except OSError as e:
            return SFTPServer.convert_errno(e.errno)
        return SFTP_OK

    def remove(self, path):
        try:
            os.remove(self._real(path))
        except OSError as e:
            return SFTPServer.convert_errno(e.errno)
        return SFTP_OK


def serve(conn):
    global sessions
    with lock:
        sessions += 1
        print(f"session {sessions}", flush=True)
    t = paramiko.Transport(conn)
    t.add_server_key(HOST_KEY)
    t.set_subsystem_handler("sftp", SFTPServer, Sftp)
    t.start_server(server=Server())
    chan = t.accept(20)   # kept: a collected channel object closes the channel
    while t.is_active():
        t.join(0.5)


s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", PORT))
s.listen(16)
print("listening", flush=True)
while True:
    c, _ = s.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
