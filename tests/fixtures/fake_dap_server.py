#!/usr/bin/env python3
# MIT License (c) 2026 Binary Dice Games
#
# A tiny scripted stand-in for a `debugpy` debug server, used by
# tests/test_python_debug_backend.cpp to exercise python_debug_backend's
# socket plumbing, DAP Content-Length framing, request/response correlation,
# the delayed-`attach`-response handshake, and `stopped` -> on_stop dispatch
# on a host with no `debugpy` installed.
#
# It implements just enough of the Debug Adapter Protocol for one canned
# session: initialize -> attach -> setBreakpoints -> configurationDone ->
# threads / stackTrace / evaluate -> continue (breakpoint hit) -> stepIn
# (step) -> disconnect. It touches no real process.
#
# Usage: fake_dap_server.py <port>   (listens on 127.0.0.1:<port>, serves one
# connection, exits when the client disconnects).
import json
import socket
import sys

FILE = "/tmp/fake_fixture.py"


def frame(func, line, fid, level=None):
    return {
        "id": fid,
        "name": func,
        "line": line,
        "column": 1,
        "source": {"path": FILE, "sourceReference": 0},
    }


class Conn:
    def __init__(self, sock):
        self.sock = sock
        self.buf = b""
        self.seq = 0

    def send(self, msg):
        msg["seq"] = self.seq = self.seq + 1
        body = json.dumps(msg).encode()
        self.sock.sendall(b"Content-Length: %d\r\n\r\n%s" % (len(body), body))

    def response(self, req, body=None, success=True, message=None):
        m = {
            "type": "response",
            "request_seq": req["seq"],
            "success": success,
            "command": req["command"],
        }
        if body is not None:
            m["body"] = body
        if message is not None:
            m["message"] = message
        self.send(m)

    def event(self, event, body=None):
        m = {"type": "event", "event": event}
        if body is not None:
            m["body"] = body
        self.send(m)

    def messages(self):
        while True:
            while b"\r\n\r\n" not in self.buf:
                chunk = self.sock.recv(4096)
                if not chunk:
                    return
                self.buf += chunk
            header, _, rest = self.buf.partition(b"\r\n\r\n")
            length = None
            for line in header.split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":")[1])
            while len(rest) < length:
                rest += self.sock.recv(4096)
            yield json.loads(rest[:length])
            self.buf = rest[length:]


def serve(conn):
    deferred_attach = None
    for msg in conn.messages():
        if msg.get("type") != "request":
            continue
        cmd = msg["command"]

        if cmd == "initialize":
            conn.response(msg, {"supportsConfigurationDoneRequest": True})
        elif cmd == "attach":
            # Real debugpy delays the attach response until after
            # configurationDone; emit `initialized` now and hold the response.
            deferred_attach = msg
            conn.event("initialized")
        elif cmd == "setBreakpoints":
            bps = msg.get("arguments", {}).get("breakpoints", [])
            conn.response(
                msg,
                {"breakpoints": [{"verified": True, "line": b["line"]} for b in bps]},
            )
            # A running debuggee hits a freshly-set breakpoint almost at
            # once (see the real behaviour probed in test_python_debug_backend's
            # notes) -- model that so the test needs no resume() call.
            if bps:
                conn.event(
                    "stopped",
                    {"reason": "breakpoint", "threadId": 1, "allThreadsStopped": True},
                )
        elif cmd == "configurationDone":
            conn.response(msg)
            if deferred_attach is not None:
                conn.response(deferred_attach)
                deferred_attach = None
                conn.event(
                    "process",
                    {"name": FILE, "systemProcessId": 4242, "startMethod": "attach"},
                )
        elif cmd == "threads":
            conn.response(msg, {"threads": [{"id": 1, "name": "MainThread"}]})
        elif cmd == "stackTrace":
            levels = msg.get("arguments", {}).get("levels", 0)
            frames = [
                frame("inner_function", 17, 2, 0),
                frame("outer_function", 21, 3, 1),
                frame("<module>", 25, 4, 2),
            ]
            if levels == 1:
                frames = frames[:1]
            conn.response(msg, {"stackFrames": frames, "totalFrames": len(frames)})
        elif cmd == "evaluate":
            conn.response(msg, {"result": "3", "type": "int"})
        elif cmd == "continue":
            conn.response(msg, {"allThreadsContinued": True})
            conn.event("continued", {"threadId": 1, "allThreadsContinued": True})
            conn.event(
                "stopped",
                {"reason": "breakpoint", "threadId": 1, "allThreadsStopped": True},
            )
        elif cmd in ("stepIn", "next", "stepOut"):
            conn.response(msg)
            conn.event("continued", {"threadId": 1, "allThreadsContinued": True})
            conn.event(
                "stopped",
                {"reason": "step", "threadId": 1, "allThreadsStopped": True},
            )
        elif cmd == "pause":
            conn.response(msg)
            conn.event(
                "stopped",
                {"reason": "pause", "threadId": 1, "allThreadsStopped": True},
            )
        elif cmd == "disconnect":
            conn.response(msg)
            return
        else:
            conn.response(msg, success=False, message="unhandled: " + cmd)


def main():
    port = int(sys.argv[1])
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    sock, _ = srv.accept()
    try:
        serve(Conn(sock))
    finally:
        sock.close()
        srv.close()


if __name__ == "__main__":
    main()
