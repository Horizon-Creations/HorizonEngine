#!/usr/bin/env python3
"""Tests for scripts/he_mcp.py, the stdio MCP shim in front of the editor.

The editor side is faked by a TCP server that speaks the same twelve lines of
handshake the bridge does (`src/HE_Editor/McpBridge.cpp`): length-prefixed
frames, first message must be `auth` with the right token or the connection
closes without a word, then `ping` / `tools/list` / `tools/call`. That is enough
to exercise everything the shim can get wrong, and it runs with no editor, no
GUI and no build.

The shim is driven as a SUBPROCESS over real pipes rather than by importing its
functions. Three of the things it must not do — write anything but JSON-RPC to
stdout, answer a notification, forget to flush — are only visible from the
outside.

Run directly (python3 tests/test_he_mcp.py) or via ctest (he_mcp_shim).
"""

import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest

try:                                    # Python 3.8 does not have queue.SimpleQueue
    from queue import SimpleQueue as _Queue
except ImportError:                     # pragma: no cover
    from queue import Queue as _Queue
import queue as _queue_module

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHIM = os.path.join(REPO_ROOT, "scripts", "he_mcp.py")

TOKEN = "a" * 64
READ_TIMEOUT_S = 15.0


# ── The editor side, faked ───────────────────────────────────────────────────


def _send_frame(sock, obj):
    payload = json.dumps(obj).encode("utf-8")
    sock.sendall(struct.pack(">I", len(payload)) + payload)


def _recv_exactly(sock, count):
    chunks = []
    while count > 0:
        chunk = sock.recv(count)
        if not chunk:
            return None
        chunks.append(chunk)
        count -= len(chunk)
    return b"".join(chunks)


def _recv_frame(sock):
    head = _recv_exactly(sock, 4)
    if head is None:
        return None
    (length,) = struct.unpack(">I", head)
    body = _recv_exactly(sock, length)
    if body is None:
        return None
    return json.loads(body.decode("utf-8"))


class FakeBridge(object):
    """One accept loop, one thread per connection, same handshake as McpBridge."""

    def __init__(self, token=TOKEN, tools=None, on_auth_reject=None,
                 oversized_frame=False):
        self.token = token
        self.tools = tools if tools is not None else [
            {"name": "ping", "description": "liveness", "inputSchema": {"type": "object"}},
            {"name": "scene_info", "description": "about the scene",
             "inputSchema": {"type": "object"}},
        ]
        self.on_auth_reject = on_auth_reject
        self.oversized_frame = oversized_frame
        self.calls = []                 # every tools/call that got through
        self.auth_attempts = 0
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind(("127.0.0.1", 0))
        self._listener.listen(4)
        self.port = self._listener.getsockname()[1]
        self._stopping = False
        self._threads = []
        self._conns = []
        self._accept = threading.Thread(target=self._accept_loop)
        self._accept.daemon = True
        self._accept.start()

    def stop(self):
        """As abrupt as a closed editor: the live connections go too, otherwise
        a shim would happily keep talking to a "stopped" bridge."""
        self._stopping = True
        try:
            self._listener.close()
        except OSError:
            pass
        for conn in self._conns:
            try:
                conn.shutdown(socket.SHUT_RDWR)   # wakes a thread blocked in recv
            except OSError:
                pass
            try:
                conn.close()
            except OSError:
                pass
        for thread in self._threads:
            thread.join(timeout=2.0)

    def _accept_loop(self):
        while not self._stopping:
            try:
                conn, _ = self._listener.accept()
            except OSError:
                return
            self._conns.append(conn)
            thread = threading.Thread(target=self._serve, args=(conn,))
            thread.daemon = True
            thread.start()
            self._threads.append(thread)

    def _serve(self, conn):
        try:
            authed = False
            while True:
                message = _recv_frame(conn)
                if message is None:
                    return
                if not authed:
                    self.auth_attempts += 1
                    given = (message.get("params") or {}).get("token")
                    if message.get("method") != "auth" or given != self.token:
                        # The real bridge answers nothing at all and hangs up.
                        if self.on_auth_reject is not None:
                            self.on_auth_reject(self)
                        return
                    authed = True
                    _send_frame(conn, {
                        "jsonrpc": "2.0", "id": message.get("id"),
                        "result": {"ok": True, "protocolVersion": 1,
                                   "server": "HorizonEditor",
                                   "toolCount": len(self.tools)},
                    })
                    continue
                reply = self._dispatch(message)
                if reply is None:
                    return
                if self.oversized_frame:
                    # A length prefix over the 4 MiB limit, with no body behind
                    # it: the shim has to refuse it on the prefix alone.
                    conn.sendall(struct.pack(">I", 8 * 1024 * 1024))
                    return
                _send_frame(conn, reply)
        except OSError:
            return
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def _dispatch(self, message):
        method = message.get("method")
        rpc_id = message.get("id")
        params = message.get("params") or {}
        if method == "ping":
            return {"jsonrpc": "2.0", "id": rpc_id, "result": {"pong": True, "nowMs": 1}}
        if method == "tools/list":
            return {"jsonrpc": "2.0", "id": rpc_id, "result": {"tools": self.tools}}
        if method == "tools/call":
            name = params.get("name")
            self.calls.append((name, params.get("arguments")))
            if name == "nope":
                return {"jsonrpc": "2.0", "id": rpc_id,
                        "error": {"code": -32601, "message": "no such tool: nope"}}
            payload = {"echo": params.get("arguments", {}), "tool": name}
            is_error = name == "refuses"
            if is_error:
                payload = {"code": "locked_by_other", "message": "someone else holds it"}
            return {"jsonrpc": "2.0", "id": rpc_id, "result": {
                "content": [{"type": "text", "text": json.dumps(payload, indent=2)}],
                "isError": is_error,
                "structuredContent": payload,
            }}
        return {"jsonrpc": "2.0", "id": rpc_id,
                "error": {"code": -32601, "message": "unknown method: %s" % method}}


# ── The shim side, driven over pipes ─────────────────────────────────────────


class ShimProcess(object):
    """he_mcp.py as a client would start it, with a reader thread so a shim that
    never answers fails the test instead of hanging it."""

    def __init__(self, endpoint_path=None, env=None, args=()):
        environment = dict(os.environ)
        environment.pop("HE_MCP_ENDPOINT", None)
        environment["HE_MCP_TIMEOUT"] = "5"
        if env:
            environment.update(env)
        command = [sys.executable, SHIM]
        if endpoint_path is not None:
            command += ["--endpoint", endpoint_path]
        command += list(args)
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, env=environment)
        self.lines = _Queue()
        self._reader = threading.Thread(target=self._pump)
        self._reader.daemon = True
        self._reader.start()

    def _pump(self):
        for line in iter(self.proc.stdout.readline, b""):
            self.lines.put(line)
        self.lines.put(None)

    def send(self, obj):
        self.proc.stdin.write(json.dumps(obj).encode("utf-8") + b"\n")
        self.proc.stdin.flush()

    def expect(self, timeout=READ_TIMEOUT_S):
        try:
            line = self.lines.get(timeout=timeout)
        except _queue_module.Empty:
            raise AssertionError("the shim wrote nothing within %.0fs" % timeout)
        if line is None:
            raise AssertionError("the shim closed stdout, stderr: %s"
                                 % self.stderr_text())
        return json.loads(line.decode("utf-8"))

    def stderr_text(self):
        try:
            return self.proc.stderr.read().decode("utf-8", "replace")
        except (OSError, ValueError):
            return ""

    def finish(self, timeout=10.0):
        """Close stdin and wait: the exit code is part of the contract."""
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise AssertionError("the shim did not exit after stdin closed")

    def close(self):
        if self.proc.poll() is None:
            try:
                self.proc.stdin.close()
            except OSError:
                pass
            try:
                self.proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5.0)
        self._reader.join(timeout=2.0)   # before the pipe it reads is closed
        for stream in (self.proc.stdin, self.proc.stdout, self.proc.stderr):
            try:
                stream.close()
            except OSError:
                pass


def write_endpoint(path, port, token=TOKEN, pid=None, host="127.0.0.1"):
    doc = {"port": port, "pid": os.getpid() if pid is None else pid,
           "token": token, "protocolVersion": 1, "host": host}
    with open(path, "w") as handle:
        json.dump(doc, handle)
    return path


def closed_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


INITIALIZE = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
              "params": {"protocolVersion": "2025-06-18",
                         "capabilities": {},
                         "clientInfo": {"name": "test", "version": "0"}}}


class ShimTestCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.mkdtemp(prefix="he_mcp_test_")
        self.endpoint = os.path.join(self._tmp, "mcp-endpoint.json")
        self._bridges = []
        self._shims = []

    def tearDown(self):
        for shim in self._shims:
            shim.close()
        for bridge in self._bridges:
            bridge.stop()

    def bridge(self, **kwargs):
        made = FakeBridge(**kwargs)
        self._bridges.append(made)
        return made

    def shim(self, **kwargs):
        made = ShimProcess(**kwargs)
        self._shims.append(made)
        return made

    def start(self, shim=None):
        """A shim process with `initialize` already sent (not yet read)."""
        shim = shim or self.shim(endpoint_path=self.endpoint)
        shim.send(INITIALIZE)
        return shim


# ── initialize ───────────────────────────────────────────────────────────────


class TestInitialize(ShimTestCase):
    def test_connects_and_answers_the_handshake(self):
        bridge = self.bridge()
        write_endpoint(self.endpoint, bridge.port)
        shim = self.start()
        reply = shim.expect()
        self.assertEqual(reply["id"], 1)
        self.assertEqual(reply["result"]["protocolVersion"], "2025-06-18")
        self.assertEqual(reply["result"]["capabilities"], {"tools": {}})
        self.assertEqual(reply["result"]["serverInfo"]["name"], "horizon-editor")
        # The connection is made DURING initialize, not lazily: that is what
        # makes "Connected" in the editor's Tool Status page mean something.
        self.assertEqual(bridge.auth_attempts, 1)
        self.assertEqual(shim.finish(), 0)

    def test_fails_and_exits_nonzero_when_the_editor_is_not_running(self):
        write_endpoint(self.endpoint, closed_port())
        shim = self.start()
        reply = shim.expect()
        self.assertEqual(reply["error"]["code"], -32000)
        self.assertIn("Remote Control", reply["error"]["message"])
        self.assertEqual(shim.proc.wait(timeout=10), 1)

    def test_names_a_dead_editor_process(self):
        # A pid that cannot be running: the endpoint file outlived a crash.
        write_endpoint(self.endpoint, closed_port(), pid=2 ** 22 - 1)
        shim = self.start()
        reply = shim.expect()
        message = reply["error"]["message"]
        if sys.platform == "win32":      # no pid probe there, on purpose
            self.assertIn("nothing is listening", message)
        else:
            self.assertIn("gone", message)
        self.assertEqual(shim.proc.wait(timeout=10), 1)

    def test_missing_endpoint_file_says_where_it_comes_from(self):
        shim = self.start()
        reply = shim.expect()
        self.assertIn("Remote Control", reply["error"]["message"])
        self.assertEqual(shim.proc.wait(timeout=10), 1)

    def test_broken_endpoint_file_is_not_a_traceback(self):
        with open(self.endpoint, "w") as handle:
            handle.write("{not json")
        shim = self.start()
        reply = shim.expect()
        self.assertIn("not valid JSON", reply["error"]["message"])
        self.assertEqual(shim.proc.wait(timeout=10), 1)


# ── The passthrough ──────────────────────────────────────────────────────────


class TestPassthrough(ShimTestCase):
    def setUp(self):
        ShimTestCase.setUp(self)
        self.editor = self.bridge()
        write_endpoint(self.endpoint, self.editor.port)
        self.shim_proc = self.start()
        self.shim_proc.expect()          # the initialize result

    def test_tools_list_arrives_unchanged(self):
        self.shim_proc.send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        reply = self.shim_proc.expect()
        self.assertEqual(reply["id"], 2)
        self.assertEqual(reply["result"]["tools"], self.editor.tools)

    def test_tools_call_arguments_and_result_survive_the_trip(self):
        self.shim_proc.send({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                             "params": {"name": "scene_info",
                                        "arguments": {"deep": True, "n": 7}}})
        reply = self.shim_proc.expect()
        self.assertEqual(self.editor.calls, [("scene_info", {"deep": True, "n": 7})])
        self.assertEqual(reply["result"]["structuredContent"],
                         {"echo": {"deep": True, "n": 7}, "tool": "scene_info"})
        self.assertEqual(reply["result"]["content"][0]["type"], "text")
        self.assertFalse(reply["result"]["isError"])

    def test_a_refusing_tool_stays_a_result(self):
        # `isError: true` is a message for the model, not a transport failure —
        # turning it into a JSON-RPC error would hide it from the conversation.
        self.shim_proc.send({"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                             "params": {"name": "refuses", "arguments": {}}})
        reply = self.shim_proc.expect()
        self.assertNotIn("error", reply)
        self.assertTrue(reply["result"]["isError"])
        self.assertEqual(reply["result"]["structuredContent"]["code"],
                         "locked_by_other")

    def test_a_bridge_error_stays_an_error(self):
        self.shim_proc.send({"jsonrpc": "2.0", "id": 5, "method": "tools/call",
                             "params": {"name": "nope"}})
        reply = self.shim_proc.expect()
        self.assertEqual(reply["error"]["code"], -32601)
        self.assertIn("nope", reply["error"]["message"])

    def test_ping_is_answered_locally(self):
        self.shim_proc.send({"jsonrpc": "2.0", "id": 6, "method": "ping"})
        self.assertEqual(self.shim_proc.expect()["result"], {})

    def test_unknown_method(self):
        self.shim_proc.send({"jsonrpc": "2.0", "id": 7, "method": "resources/list"})
        reply = self.shim_proc.expect()
        self.assertEqual(reply["error"]["code"], -32601)

    def test_notifications_produce_no_output_at_all(self):
        # Two notifications, then a request: if either had been answered, the
        # next line would carry its id instead of the ping's.
        self.shim_proc.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        self.shim_proc.send({"jsonrpc": "2.0", "method": "notifications/cancelled",
                             "params": {"requestId": 3}})
        self.shim_proc.send({"jsonrpc": "2.0", "id": 8, "method": "ping"})
        reply = self.shim_proc.expect()
        self.assertEqual(reply["id"], 8)

    def test_a_line_that_is_not_json(self):
        self.shim_proc.proc.stdin.write(b"{ this is not json\n")
        self.shim_proc.proc.stdin.flush()
        reply = self.shim_proc.expect()
        self.assertEqual(reply["error"]["code"], -32700)
        self.assertIsNone(reply["id"])
        # …and the stream is still usable afterwards.
        self.shim_proc.send({"jsonrpc": "2.0", "id": 9, "method": "ping"})
        self.assertEqual(self.shim_proc.expect()["id"], 9)

    def test_blank_lines_are_ignored(self):
        self.shim_proc.proc.stdin.write(b"\n\n")
        self.shim_proc.proc.stdin.flush()
        self.shim_proc.send({"jsonrpc": "2.0", "id": 10, "method": "ping"})
        self.assertEqual(self.shim_proc.expect()["id"], 10)

    def test_closing_stdin_ends_it_cleanly(self):
        self.assertEqual(self.shim_proc.finish(), 0)


# ── Reconnecting ─────────────────────────────────────────────────────────────


class TestReconnect(ShimTestCase):
    def test_editor_goes_away_and_comes_back_on_a_new_port(self):
        first = self.bridge()
        write_endpoint(self.endpoint, first.port)
        shim = self.start()
        shim.expect()
        shim.send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        self.assertIn("result", shim.expect())

        first.stop()
        shim.send({"jsonrpc": "2.0", "id": 3, "method": "tools/list"})
        broken = shim.expect()
        # Not fatal, and not a lie either: the client is told the editor is gone
        # and the shim stays up.
        self.assertEqual(broken["error"]["code"], -32000)
        self.assertIsNone(shim.proc.poll())

        # A restarted editor: new port, new token, rewritten endpoint file. The
        # client has not been restarted and must not have to be.
        second_token = "b" * 64
        second = self.bridge(token=second_token,
                             tools=[{"name": "entity_create", "description": "x",
                                     "inputSchema": {"type": "object"}}])
        write_endpoint(self.endpoint, second.port, token=second_token)

        shim.send({"jsonrpc": "2.0", "id": 4, "method": "tools/list"})
        reply = shim.expect()
        self.assertEqual([t["name"] for t in reply["result"]["tools"]],
                         ["entity_create"])
        self.assertEqual(shim.finish(), 0)

    def test_a_token_rewritten_under_us_is_retried_once(self):
        # The narrow race the retry exists for: the editor restarts between our
        # read of the endpoint file and our connect, so the token we send is one
        # generation old. The bridge hangs up without a word; re-reading the
        # file and trying again is the whole recovery.
        good_token = "c" * 64
        editor = self.bridge(token=good_token)
        editor.on_auth_reject = lambda b: write_endpoint(
            self.endpoint, b.port, token=good_token)
        write_endpoint(self.endpoint, editor.port, token="stale" * 12)

        shim = self.start()
        reply = shim.expect()
        self.assertNotIn("error", reply)
        self.assertEqual(editor.auth_attempts, 2)
        self.assertEqual(shim.finish(), 0)

    def test_a_token_that_stays_wrong_is_reported_not_retried_forever(self):
        editor = self.bridge(token="d" * 64)
        write_endpoint(self.endpoint, editor.port, token="wrong" * 12)
        shim = self.start()
        reply = shim.expect()
        self.assertIn("refused the handshake", reply["error"]["message"])
        self.assertEqual(editor.auth_attempts, 2)
        self.assertEqual(shim.proc.wait(timeout=10), 1)


class TestFraming(ShimTestCase):
    def test_an_oversized_length_prefix_is_refused_not_allocated(self):
        editor = self.bridge(oversized_frame=True)
        write_endpoint(self.endpoint, editor.port)
        shim = self.start()
        shim.expect()                    # auth frame is normal; the next is not
        shim.send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        reply = shim.expect()
        self.assertEqual(reply["error"]["code"], -32000)
        self.assertIn("4 MiB", reply["error"]["message"])
        self.assertEqual(shim.finish(), 0)


# ── --selftest ───────────────────────────────────────────────────────────────


class TestSelftest(ShimTestCase):
    def run_selftest(self, env=None, endpoint_path="use-default"):
        path = self.endpoint if endpoint_path == "use-default" else endpoint_path
        environment = dict(os.environ)
        environment.pop("HE_MCP_ENDPOINT", None)
        environment["HE_MCP_TIMEOUT"] = "5"
        if env:
            environment.update(env)
        command = [sys.executable, SHIM, "--selftest"]
        if path is not None:
            command += ["--endpoint", path]
        done = subprocess.run(command, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, env=environment, timeout=30)
        return done.returncode, json.loads(done.stdout.decode("utf-8"))

    def test_reports_the_tool_count_when_the_editor_answers(self):
        editor = self.bridge()
        write_endpoint(self.endpoint, editor.port)
        code, doc = self.run_selftest()
        self.assertEqual(code, 0)
        self.assertTrue(doc["ok"])
        self.assertEqual(doc["port"], editor.port)
        self.assertEqual(doc["tools"], len(editor.tools))

    def test_says_why_when_it_cannot_connect(self):
        write_endpoint(self.endpoint, closed_port())
        code, doc = self.run_selftest()
        self.assertEqual(code, 1)
        self.assertFalse(doc["ok"])
        self.assertIn("nothing is listening", doc["error"])

    def test_reads_the_endpoint_path_from_the_environment(self):
        editor = self.bridge()
        write_endpoint(self.endpoint, editor.port)
        code, doc = self.run_selftest(env={"HE_MCP_ENDPOINT": self.endpoint},
                                      endpoint_path=None)
        self.assertEqual(code, 0)
        self.assertEqual(doc["endpoint"], self.endpoint)


# ── Endpoint resolution, at unit level ───────────────────────────────────────


class TestEndpointResolution(unittest.TestCase):
    """The one part that is easier to check in-process than over a pipe: which
    of the three sources wins."""

    @classmethod
    def setUpClass(cls):
        import importlib.util
        spec = importlib.util.spec_from_file_location("he_mcp_under_test", SHIM)
        cls.mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.mod)

    def setUp(self):
        self._saved = dict(os.environ)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._saved)

    def test_explicit_beats_environment(self):
        os.environ["HE_MCP_ENDPOINT"] = "/from/env.json"
        self.assertEqual(self.mod.resolve_endpoint_path("/explicit.json"),
                         "/explicit.json")

    def test_environment_beats_the_platform_default(self):
        os.environ["HE_MCP_ENDPOINT"] = "/from/env.json"
        self.assertEqual(self.mod.resolve_endpoint_path(None), "/from/env.json")

    def test_platform_default_mirrors_the_editor(self):
        os.environ.pop("HE_MCP_ENDPOINT", None)
        path = self.mod.resolve_endpoint_path(None)
        self.assertTrue(path.endswith("mcp-endpoint.json"), path)
        self.assertIn("HorizonEngine", path)
        if sys.platform == "darwin":
            self.assertIn(os.path.join("Library", "Application Support"), path)
        elif sys.platform.startswith("linux"):
            os.environ["XDG_CONFIG_HOME"] = "/tmp/xdg"
            self.assertEqual(self.mod.resolve_endpoint_path(None),
                             "/tmp/xdg/HorizonEngine/mcp-endpoint.json")

    def test_a_port_of_zero_is_not_an_endpoint(self):
        # The editor writes port 0 into its config to mean "let the OS pick",
        # never into the endpoint file — a 0 there is a file we must not trust.
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
            json.dump({"port": 0, "token": TOKEN, "pid": 1}, handle)
            path = handle.name
        try:
            with self.assertRaises(self.mod.ShimError):
                self.mod.read_endpoint(path)
        finally:
            os.unlink(path)

    def test_the_host_defaults_to_ipv4_loopback(self):
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
            json.dump({"port": 4242, "token": TOKEN, "pid": 1}, handle)
            path = handle.name
        try:
            self.assertEqual(self.mod.read_endpoint(path)["host"], "127.0.0.1")
        finally:
            os.unlink(path)


if __name__ == "__main__":
    unittest.main(verbosity=2)
