"""Tests for wish.Client.desktop() / run_on_desktop() -- a UI on the local
wish desktop that comes back whenever the desktop does.

A console-rendered wish.Server listening on a per-test pipe plays the
desktop; WISH_DESKTOP_ENDPOINT points Client.desktop() at it.

Build both DLLs first, then run from the repository root::

    cmake -DWISH_BUILD_SERVER_SHARED=ON -B build
    cmake --build build --target wish_client_dll wish_server_dll
    python -m pytest bindings/python/tests/test_desktop.py -v
"""

import os
import sys
import threading
import unittest
import uuid

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

import _wish_home  # noqa: E402,F401  (before any server starts)

from wish import Client, Server

WINDOW = '{"type": "Window", "title": "Service", "children": {}}'


def _unique_endpoint() -> str:
    name = "wish_py_desktop_test_" + uuid.uuid4().hex
    if sys.platform == "win32":
        return "\\\\.\\pipe\\" + name
    return "@" + name


class TestRunOnDesktop(unittest.TestCase):
    def setUp(self):
        self.endpoint = _unique_endpoint()
        os.environ["WISH_DESKTOP_ENDPOINT"] = self.endpoint
        self.server = None
        self.sessions = 0
        self.cond = threading.Condition()

    def tearDown(self):
        self._stop_desktop()
        os.environ.pop("WISH_DESKTOP_ENDPOINT", None)

    def _start_desktop(self):
        self.server = Server.pipe(self.endpoint)
        self.server.start(renderer="console")

    def _stop_desktop(self):
        if self.server:
            self.server.stop()
            self.server.release()
            self.server = None

    def _wait_for_sessions(self, n, timeout=5.0) -> bool:
        with self.cond:
            return self.cond.wait_for(lambda: self.sessions >= n, timeout)

    def _session(self, client):
        client.register_template("ui", WINDOW)
        root = client.instantiate_template("ui", "ui")  # noqa: F841 -- keeps the window
        with self.cond:
            self.sessions += 1
            self.cond.notify_all()
        client.wait()

    def test_session_runs_again_each_time_the_desktop_comes_back(self):
        client = Client.desktop()
        loop = threading.Thread(target=client.run_on_desktop, args=(self._session,), kwargs={"retry": 0.02})
        loop.start()
        try:
            self._start_desktop()
            self.assertTrue(self._wait_for_sessions(1))
            self._stop_desktop()  # the desktop closes: back to waiting
            self._start_desktop()
            self.assertTrue(self._wait_for_sessions(2))
        finally:
            client.stop_desktop()
            loop.join(timeout=5)
        self.assertFalse(loop.is_alive())

    def test_stop_while_waiting_for_a_desktop_returns(self):
        client = Client.desktop()
        loop = threading.Thread(target=client.run_on_desktop, args=(self._session,), kwargs={"retry": 0.02})
        loop.start()
        client.stop_desktop()
        loop.join(timeout=5)
        self.assertFalse(loop.is_alive())

    def test_exception_in_session_ends_the_loop_and_is_raised(self):
        self._start_desktop()
        client = Client.desktop()

        def failing(_client):
            raise ValueError("boom")

        with self.assertRaises(ValueError):
            client.run_on_desktop(failing, retry=0.02)

    def test_from_env_desktop_transport(self):
        os.environ["WISH_TRANSPORT"] = "desktop"
        try:
            client = Client.from_env()
            self.assertTrue(client._handle)
            client.destroy()
        finally:
            os.environ.pop("WISH_TRANSPORT", None)


if __name__ == "__main__":
    unittest.main()
