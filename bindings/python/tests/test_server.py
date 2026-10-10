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
import shutil
import socket
import tempfile
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
        with self.assertRaises(RuntimeError) as ctx:
            server.start(renderer="not-a-real-renderer")
        # Regression check: an unknown renderer_kind must map to
        # WISH_SERVER_ERR_BAD_RENDERER specifically, not the generic
        # WISH_SERVER_ERR_EXCEPTION every other internal failure uses.
        self.assertIn("Unknown renderer_kind", str(ctx.exception))
        server.release()

    def test_tls_create_and_destroy(self):
        server = Server.tls("127.0.0.1", _free_port())
        self.assertTrue(server._handle)
        server.release()
        self.assertFalse(server._handle)
        # Idempotent.
        server.release()

    def test_term_create_and_destroy(self):
        # Empty cmd spawns the operator's $SHELL; should_quit() should
        # eventually observe the spawned child exiting on its own once its
        # stdin (the pty master, held open by this handle) is closed.
        server = Server.term("true")
        self.assertTrue(server._handle)
        server.release()
        self.assertFalse(server._handle)
        # Idempotent.
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


class TestUserStore(unittest.TestCase):
    """Client.user_store_* against a real wish_server_dll server."""

    def setUp(self):
        self.store_dir = tempfile.mkdtemp(prefix="wish_py_store_")
        self.port = _free_port()
        self.server = Server.tcp("127.0.0.1", self.port)
        self.server.start(renderer="console", store_dir=self.store_dir)
        time.sleep(0.1)

    def tearDown(self):
        self.server.stop()
        self.server.release()
        shutil.rmtree(self.store_dir, ignore_errors=True)

    def _run(self, session, username=None):
        result = {}
        done = threading.Event()

        def wrapped(client):
            try:
                session(client, result)
            except Exception as e:  # surfaced by the asserting test body
                result["error"] = e
            finally:
                done.set()

        client = Client.tcp("127.0.0.1", self.port)
        params = {"username": username} if username else None
        t = threading.Thread(target=lambda: client.run(wrapped, params=params), daemon=True)
        t.start()
        self.assertTrue(done.wait(timeout=10))
        t.join(timeout=5)
        if "error" in result:
            raise result["error"]
        return result

    def test_identified_client_round_trips_and_persists(self):
        def write(c, r):
            r["has"] = c.has_user_store()
            c.user_store_set("bdg.test", {"filter": "error|warn", "lines": 50})
            r["missing"] = c.user_store_get("nope")
            r["keys"] = c.user_store_keys()

        r = self._run(write, username="alice")
        self.assertTrue(r["has"])
        self.assertIsNone(r["missing"])
        self.assertEqual(r["keys"], ["bdg.test"])

        def read(c, r):
            v = c.user_store_get("bdg.test")
            r["filter"] = v["filter"]
            r["lines"] = v["lines"]
            v.release()
            r["erased"] = c.user_store_erase("bdg.test")
            r["erased_again"] = c.user_store_erase("bdg.test")

        r = self._run(read, username="alice")
        self.assertEqual(r["filter"], "error|warn")
        self.assertEqual(r["lines"], 50)
        self.assertTrue(r["erased"])
        self.assertFalse(r["erased_again"])

    def test_other_user_cannot_see_entries(self):
        self._run(lambda c, r: c.user_store_set("secret", {"v": 1}), username="alice")
        r = self._run(lambda c, r: r.update(keys=c.user_store_keys()), username="bob")
        self.assertEqual(r["keys"], [])

    def test_anonymous_client_has_no_user_store(self):
        from wish import WishError
        from wish import _native as _n

        def session(c, r):
            r["has"] = c.has_user_store()
            try:
                c.user_store_keys()
            except WishError as e:
                r["code"] = e.code

        r = self._run(session)
        self.assertFalse(r["has"])
        self.assertEqual(r["code"], _n.WISH_ERR_UNAVAILABLE)


if __name__ == "__main__":
    unittest.main()
