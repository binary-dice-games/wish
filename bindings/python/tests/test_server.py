"""Tests for wish.Server -- the wish_server_dll Python binding.

Round-trips against the *real* wish.Client binding (Task 1), over the
"console" renderer (no display needed, safe for CI): starts a
wish.Server.tcp(...), connects a wish.Client.tcp(...) to it on a background
thread, registers and instantiates a template, and asserts field values
read back correctly -- proving wish_server_dll's protocol matches a real
wish client, with real per-widget proxies (unlike a server built from the
generic bison RMI ABI alone).

Build both DLLs first, then run from the repository root::

    cmake -DWISH_BUILD_SERVER_SHARED=ON -B build
    cmake --build build --target wish_client_dll wish_server_dll
    python -m pytest bindings/python/tests/test_server.py -v
"""

import os
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from wish import Client, Server


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class TestServerLifecycle(unittest.TestCase):
    def test_tcp_create_and_destroy(self):
        server = Server.tcp("127.0.0.1", _free_port())
        self.assertTrue(server._handle)
        server.release()
        self.assertFalse(server._handle)
        # Idempotent.
        server.release()

    def test_stop_before_start_is_noop(self):
        server = Server.tcp("127.0.0.1", _free_port())
        server.stop()  # must not raise even though start() was never called
        server.release()

    def test_start_with_renderer_params(self):
        # Regression test: start()'s params dict must build a bison_handle
        # against wish_server_dll's *own* loaded library, not bison._native's
        # process-wide singleton (which a same-process wish.Client would bind
        # to a different library -- see _server_native.py's module docstring).
        server = Server.tcp("127.0.0.1", _free_port())
        server.start(renderer="console", title="Custom Title", width=800, height=600)
        server.stop()
        server.release()

    def test_should_quit_false_before_start(self):
        server = Server.tcp("127.0.0.1", _free_port())
        self.assertFalse(server.should_quit())
        server.release()

    def test_bad_renderer_kind_raises(self):
        server = Server.tcp("127.0.0.1", _free_port())
        with self.assertRaises(RuntimeError):
            server.start(renderer="not-a-real-renderer")
        server.release()


class TestServerClientRoundTrip(unittest.TestCase):
    """Starts a console-rendered server and drives it with a real wish.Client."""

    TEMPLATE_DESC = """{
      "type": "Window",
      "title": "Hi",
      "children": {
        "label": { "type": "Label", "text": "hello" }
      }
    }"""

    def setUp(self):
        self.port = _free_port()
        self.server = Server.tcp("127.0.0.1", self.port)
        self.server.start(renderer="console")
        # Give the accept loop a moment to actually start listening.
        time.sleep(0.1)

    def tearDown(self):
        self.server.stop()
        self.server.release()

    def test_register_and_instantiate_template(self):
        result = {}
        ready = threading.Event()

        def session(client):
            client.register_template("ui", self.TEMPLATE_DESC)
            root = client.instantiate_template("ui", "ui")
            result["title"] = root.get()["title"]

            # A real, independently-addressable proxy for the *nested*
            # widget, resolved from the client's local proxy map -- this is
            # exactly the fidelity a generic-ABI server can't provide.
            label = client.proxy_get("ui.label")
            result["label_text"] = label.get()["text"]

            # Field set/get round trip through the real widget object.
            label.set({"text": "updated"})
            result["label_text_after_set"] = label.get()["text"]

            ready.set()
            client.quit()

        client = Client.tcp("127.0.0.1", self.port)
        t = threading.Thread(target=lambda: client.run(session), daemon=True)
        t.start()
        self.assertTrue(ready.wait(timeout=5))
        t.join(timeout=5)

        self.assertEqual(result["title"], "Hi")
        self.assertEqual(result["label_text"], "hello")
        self.assertEqual(result["label_text_after_set"], "updated")


if __name__ == "__main__":
    unittest.main()
