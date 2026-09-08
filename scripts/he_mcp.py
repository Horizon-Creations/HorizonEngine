#!/usr/bin/env python3
"""he_mcp — the stdio MCP server that fronts the editor's Remote Control bridge.

The editor listens on a framed TCP socket and speaks raw JSON-RPC 2.0
(`src/HE_Editor/McpBridge.cpp`). MCP clients speak newline-delimited JSON-RPC on
stdin/stdout of a process they start themselves. A GUI application cannot be
that process — it is started by a human and outlives any single client — so this
script is the piece in between: the client starts it, it connects to whatever
editor is currently listening, and it hands messages across in both directions.

It defines NO tools of its own. `tools/list` and `tools/call` go to the editor
and come back untouched, because the bridge already answers in MCP's own wire
shape (`{tools:[{name,description,inputSchema}]}` and
`{content:[...],isError,structuredContent}`). Every tool the editor gains is
visible here the moment it exists, with no change to this file.

Only the standard library. No `pip install`, no virtualenv, any Python from 3.8:
the "Add to Claude" button in Preferences promises a registration that works,
and a promise chained to a package the editor does not ship is not one it can
keep (docs/mcp-editor-integration-plan.md, section 6.2).

Usage:
    he_mcp.py [--endpoint PATH]      run as an MCP stdio server (what a client does)
    he_mcp.py --selftest [--endpoint PATH]
                                     connect, authenticate, list tools, print one
                                     JSON line, exit 0/1 — the check a human or
                                     the editor's Tool Status page can run

Where the endpoint file comes from, in order:
    --endpoint PATH  →  $HE_MCP_ENDPOINT  →  the per-user data directory
The first two are the normal way: the editor knows the path and passes it along
when it registers the server, so the two sides cannot drift apart. The third
mirrors `GlobalState::userDataDir()` and exists for running this by hand.

Diagnostics go to stderr, never to stdout: one stray line there and the client's
JSON stream is poisoned. `HE_MCP_DEBUG=1` makes them verbose.
"""

import json
import os
import socket
import struct
import sys

ENDPOINT_FILENAME = "mcp-endpoint.json"

SERVER_NAME = "horizon-editor"
SERVER_VERSION = "1.0.0"

# Answered when the client does not name one. A passthrough supports whatever
# the client speaks, so the requested version is echoed when there is one.
FALLBACK_PROTOCOL_VERSION = "2025-06-18"

# The same ceiling the bridge enforces (`McpBridge.h:63`), checked on this side
# too so a corrupt length prefix cannot turn into a huge allocation here.
MAX_FRAME_BYTES = 4 * 1024 * 1024

CONNECT_TIMEOUT_S = 5.0

# The bridge answers inside the editor's frame loop. A modal dialog or a long
# import blocks that loop, and a shim without a timeout would hang until the
# client gives up — with no way to tell the user why. On timeout the socket is
# dropped, so the next request reconnects rather than reading a stale answer.
DEFAULT_REQUEST_TIMEOUT_S = 30.0


class ShimError(Exception):
    """Anything that stops us from reaching the editor. The message is shown to
    the user (via the client's error display or --selftest), so it says what to
    do, not just what failed."""


def _diag(message):
    if os.environ.get("HE_MCP_DEBUG"):
        sys.stderr.write("he_mcp: %s\n" % message)
        sys.stderr.flush()


# ── The endpoint file ────────────────────────────────────────────────────────


def default_endpoint_path():
    """Mirror of `GlobalState::userDataDir()` (GlobalState.cpp:112-129)."""
    root = ""
    if sys.platform == "win32":
        appdata = os.environ.get("APPDATA")
        if appdata:
            root = os.path.join(appdata, "HorizonEngine")
    elif sys.platform == "darwin":
        home = os.environ.get("HOME")
        if home:
            root = os.path.join(home, "Library", "Application Support", "HorizonEngine")
    else:
        xdg = os.environ.get("XDG_CONFIG_HOME")
        home = os.environ.get("HOME")
        if xdg:
            root = os.path.join(xdg, "HorizonEngine")
        elif home:
            root = os.path.join(home, ".config", "HorizonEngine")
    return os.path.join(root, ENDPOINT_FILENAME) if root else ""


def resolve_endpoint_path(explicit=None):
    if explicit:
        return explicit
    from_env = os.environ.get("HE_MCP_ENDPOINT")
    if from_env:
        return from_env
    return default_endpoint_path()


def read_endpoint(path):
    """The file the bridge writes on start and deletes on a clean stop."""
    if not path:
        raise ShimError(
            "no endpoint file to read: pass --endpoint or set HE_MCP_ENDPOINT "
            "(this platform has no per-user data directory)")
    try:
        with open(path, "rb") as handle:
            raw = handle.read()
    except OSError as exc:
        raise ShimError(
            "cannot read the endpoint file %s (%s) — the editor writes it while "
            "Preferences > Editor > Remote Control is switched on"
            % (path, exc.strerror or exc))
    try:
        doc = json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        raise ShimError("the endpoint file %s is not valid JSON" % path)
    if not isinstance(doc, dict):
        raise ShimError("the endpoint file %s is not a JSON object" % path)

    port = doc.get("port")
    token = doc.get("token")
    if not isinstance(port, int) or isinstance(port, bool) or not 0 < port < 65536:
        raise ShimError("the endpoint file %s has no usable 'port'" % path)
    if not isinstance(token, str) or not token:
        raise ShimError("the endpoint file %s has no usable 'token'" % path)

    pid = doc.get("pid")
    if not isinstance(pid, int) or isinstance(pid, bool):
        pid = 0
    # Spelled out in the file rather than assumed: the listener is IPv4 loopback
    # only, and "localhost" resolves to ::1 first on macOS.
    host = doc.get("host")
    if not isinstance(host, str) or not host:
        host = "127.0.0.1"
    return {"host": host, "port": port, "token": token, "pid": pid, "path": path}


def pid_alive(pid):
    """True / False / None (cannot tell). Advisory only — the real liveness test
    is whether the connect and the handshake go through."""
    if not pid or pid <= 0:
        return None
    if sys.platform == "win32":
        # os.kill with a signal other than CTRL_C/CTRL_BREAK does not probe on
        # Windows, it calls TerminateProcess — asking the question would kill
        # the editor. Skipping costs a nicer error message, nothing else.
        return None
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except OSError:
        return None
    return True


# ── Framing: [uint32 big-endian length][UTF-8 JSON] ──────────────────────────


def _send_frame(sock, obj):
    payload = json.dumps(obj).encode("utf-8")
    if len(payload) > MAX_FRAME_BYTES:
        raise ShimError("message of %d bytes exceeds the 4 MiB frame limit"
                        % len(payload))
    sock.sendall(struct.pack(">I", len(payload)) + payload)


def _recv_exactly(sock, count):
    chunks = []
    remaining = count
    while remaining > 0:
        try:
            chunk = sock.recv(min(remaining, 65536))
        except socket.timeout:
            raise ShimError("the editor did not answer within %.0fs — it may be "
                            "busy or showing a modal dialog" % _request_timeout())
        except OSError as exc:
            raise ShimError("the connection to the editor broke (%s)" % exc)
        if not chunk:
            raise ShimError("the editor closed the connection")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def _recv_frame(sock):
    (length,) = struct.unpack(">I", _recv_exactly(sock, 4))
    if length > MAX_FRAME_BYTES:
        raise ShimError("the editor announced a %d byte frame, over the 4 MiB "
                        "limit" % length)
    body = _recv_exactly(sock, length)
    try:
        return json.loads(body.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        raise ShimError("the editor sent a frame that is not valid JSON")


def _request_timeout():
    raw = os.environ.get("HE_MCP_TIMEOUT")
    if raw:
        try:
            value = float(raw)
            if value > 0:
                return value
        except ValueError:
            pass
    return DEFAULT_REQUEST_TIMEOUT_S


# ── The link to the editor ───────────────────────────────────────────────────


class Bridge:
    """One connection to one editor, re-established on demand.

    The editor can be restarted under us: new port, new token, dead socket. So
    nothing about a connection is cached across a failure — the endpoint file is
    read again on every reconnect, which is what makes a restarted editor
    reachable without restarting the client.
    """

    def __init__(self, endpoint_path):
        self._endpoint_path = endpoint_path
        self._sock = None
        self._info = {}
        self._next_id = 0

    @property
    def info(self):
        return dict(self._info)

    @property
    def connected(self):
        return self._sock is not None

    def close(self):
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def ensure(self):
        if self._sock is None:
            self._open()

    def request(self, method, params=None):
        """Send one JSON-RPC request, return the whole reply object.

        A failure closes the socket and raises. It never re-sends: the request
        may already be on the wire, and a `tools/call` that mutates the scene
        must not be applied twice because the answer got lost.
        """
        self.ensure()
        self._next_id += 1
        rpc_id = self._next_id
        message = {"jsonrpc": "2.0", "id": rpc_id, "method": method}
        if params is not None:
            message["params"] = params
        try:
            _send_frame(self._sock, message)
            return self._await(self._sock, rpc_id)
        except ShimError:
            self.close()
            raise

    @staticmethod
    def _await(sock, rpc_id):
        # The bridge answers one request at a time on its main thread, but skip
        # anything that is not ours rather than assuming it.
        while True:
            reply = _recv_frame(sock)
            if isinstance(reply, dict) and reply.get("id") == rpc_id:
                return reply
            _diag("ignoring an unsolicited frame from the editor")

    def _open(self):
        # Two attempts, because the one race that matters is narrow and cheap to
        # lose: the editor rewrote the file (restart, new token) between our read
        # and our connect. The bridge answers a wrong token by closing without a
        # word, so re-reading and trying once more is the whole recovery.
        failure = None
        last_info = None
        for attempt in (0, 1):
            info = read_endpoint(self._endpoint_path)
            last_info = info
            sock = self._connect(info)
            try:
                self._authenticate(sock, info)
            except ShimError as exc:
                try:
                    sock.close()
                except OSError:
                    pass
                failure = exc
                _diag("handshake failed on attempt %d: %s" % (attempt + 1, exc))
                continue
            self._sock = sock
            self._info = info
            _diag("connected to %s:%d (pid %s)"
                  % (info["host"], info["port"], info["pid"] or "?"))
            return
        raise ShimError(
            "the editor on %s:%d refused the handshake — the token in %s does not "
            "match the listener (%s)"
            % (last_info["host"], last_info["port"], self._endpoint_path, failure))

    def _connect(self, info):
        try:
            sock = socket.create_connection((info["host"], info["port"]),
                                            CONNECT_TIMEOUT_S)
        except OSError as exc:
            raise ShimError(self._unreachable_message(info, exc))
        sock.settimeout(_request_timeout())
        return sock

    def _unreachable_message(self, info, exc):
        alive = pid_alive(info["pid"])
        if alive is False:
            return ("the endpoint file %s belongs to process %d, which is gone — "
                    "the editor was closed or crashed. Start it and switch "
                    "Preferences > Editor > Remote Control on."
                    % (info["path"], info["pid"]))
        return ("nothing is listening on %s:%d (%s) — is the editor running with "
                "Preferences > Editor > Remote Control switched on?"
                % (info["host"], info["port"], exc))

    def _authenticate(self, sock, info):
        self._next_id += 1
        rpc_id = self._next_id
        _send_frame(sock, {
            "jsonrpc": "2.0",
            "id": rpc_id,
            "method": "auth",
            # The bridge only reads `token`; `client` is there so a future
            # version can say in its log WHO authenticated (6.7).
            "params": {"token": info["token"], "client": "he_mcp/1"},
        })
        reply = self._await(sock, rpc_id)
        if isinstance(reply.get("error"), dict):
            raise ShimError("the editor rejected the token: %s"
                            % reply["error"].get("message", "auth failed"))
        result = reply.get("result")
        if not isinstance(result, dict) or result.get("ok") is not True:
            raise ShimError("the editor answered the handshake with something "
                            "unexpected")


# ── The stdio side ───────────────────────────────────────────────────────────


def _result(rpc_id, payload):
    return {"jsonrpc": "2.0", "id": rpc_id, "result": payload}


def _error(rpc_id, code, message):
    return {"jsonrpc": "2.0", "id": rpc_id, "error": {"code": code, "message": message}}


class Shim:
    """Newline-delimited JSON-RPC on stdin/stdout, five methods, no tools."""

    def __init__(self, bridge):
        self._bridge = bridge
        self.exit_code = 0

    def serve(self, stdin, stdout):
        while True:
            line = stdin.readline()
            if not line:            # the client closed our stdin: a normal end
                break
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line.decode("utf-8"))
            except (ValueError, UnicodeDecodeError):
                self._write(stdout, _error(None, -32700, "parse error"))
                continue
            if not isinstance(message, dict):
                self._write(stdout, _error(None, -32600, "invalid request"))
                continue
            response = self.handle(message)
            if response is not None:
                self._write(stdout, response)
            if self.exit_code:
                break
        return self.exit_code

    @staticmethod
    def _write(stdout, obj):
        stdout.write(json.dumps(obj).encode("utf-8") + b"\n")
        stdout.flush()

    def handle(self, message):
        method = message.get("method")
        rpc_id = message.get("id")
        # No id means a notification: it gets no answer, ever. Answering one is
        # a protocol error on the client's stream.
        is_notification = "id" not in message or rpc_id is None
        params = message.get("params")
        if not isinstance(params, dict):
            params = {}

        if not isinstance(method, str) or not method:
            return None if is_notification else _error(rpc_id, -32600, "invalid request")

        if is_notification:
            _diag("notification %s" % method)
            return None

        if method == "initialize":
            return self._initialize(rpc_id, params)
        if method == "ping":
            return _result(rpc_id, {})
        if method in ("tools/list", "tools/call"):
            return self._forward(rpc_id, method, params)
        return _error(rpc_id, -32601, "method not found: %s" % method)

    def _initialize(self, rpc_id, params):
        # The connection is made HERE, not lazily on the first tools/list. A
        # shim that reported success with the editor closed would make the Tool
        # Status page's "Connected" row a lie — `claude mcp get` only runs the
        # handshake, so this is the one place where the order carries the claim.
        try:
            self._bridge.ensure()
        except ShimError as exc:
            sys.stderr.write("he_mcp: %s\n" % exc)
            sys.stderr.flush()
            self.exit_code = 1
            return _error(rpc_id, -32000, str(exc))

        requested = params.get("protocolVersion")
        version = requested if isinstance(requested, str) and requested \
            else FALLBACK_PROTOCOL_VERSION
        return _result(rpc_id, {
            "protocolVersion": version,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
        })

    def _forward(self, rpc_id, method, params):
        try:
            reply = self._bridge.request(method, params)
        except ShimError as exc:
            # Not fatal: the editor may come back. The socket is already closed,
            # so the next call re-reads the endpoint file and reconnects.
            return _error(rpc_id, -32000, str(exc))
        if isinstance(reply.get("error"), dict):
            return {"jsonrpc": "2.0", "id": rpc_id, "error": reply["error"]}
        return _result(rpc_id, reply.get("result", {}))


# ── --selftest ───────────────────────────────────────────────────────────────


def selftest(endpoint_path):
    """One line of JSON on stdout, exit 0 or 1. No MCP involved — this is the
    check that answers "is the editor reachable at all", without dragging the
    `claude` CLI into the question."""
    bridge = Bridge(endpoint_path)
    try:
        bridge.ensure()
        reply = bridge.request("tools/list")
        if isinstance(reply.get("error"), dict):
            raise ShimError("tools/list failed: %s"
                            % reply["error"].get("message", "unknown error"))
        tools = reply.get("result", {}).get("tools", [])
        info = bridge.info
        print(json.dumps({
            "ok": True,
            "endpoint": endpoint_path,
            "host": info.get("host"),
            "port": info.get("port"),
            "pid": info.get("pid"),
            "tools": len(tools) if isinstance(tools, list) else 0,
        }))
        return 0
    except ShimError as exc:
        print(json.dumps({"ok": False, "endpoint": endpoint_path, "error": str(exc)}))
        return 1
    finally:
        bridge.close()


# ── Entry point ──────────────────────────────────────────────────────────────


def main(argv):
    endpoint = None
    want_selftest = False
    rest = list(argv)
    while rest:
        arg = rest.pop(0)
        if arg == "--selftest":
            want_selftest = True
        elif arg == "--endpoint":
            if not rest:
                sys.stderr.write("he_mcp: --endpoint needs a path\n")
                return 2
            endpoint = rest.pop(0)
        elif arg.startswith("--endpoint="):
            endpoint = arg.split("=", 1)[1]
        elif arg in ("-h", "--help"):
            sys.stdout.write(__doc__)
            return 0
        elif arg == "--version":
            sys.stdout.write("%s %s\n" % (SERVER_NAME, SERVER_VERSION))
            return 0
        else:
            sys.stderr.write("he_mcp: unknown argument %s\n" % arg)
            return 2

    endpoint_path = resolve_endpoint_path(endpoint)
    if want_selftest:
        return selftest(endpoint_path)

    bridge = Bridge(endpoint_path)
    try:
        # Binary streams on purpose: Windows text mode would turn every \n into
        # \r\n on the way out, and the encoding of the pipe is not ours to guess.
        return Shim(bridge).serve(sys.stdin.buffer, sys.stdout.buffer)
    finally:
        bridge.close()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
