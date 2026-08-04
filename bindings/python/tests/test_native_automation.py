"""End-to-end smoke test for native (ABI-driven) automation -- the SDL3
counterpart to wish.automation's browser-based e2e tests. Drives a real
``wish server --renderer sdl3 ...`` subprocess entirely through
``wish.Client``'s automation methods (get_tree/click/type_text/screenshot/
get_logs), with no browser and no second client -- see
src/automation/DESIGN.md's "Native (ABI-based) automation" section.

Opt-in, like ``wish.automation_testing.wish_ui``: set
``WISH_NATIVE_AUTOMATION_SERVER_CMD`` (a shell-quoted ``wish server
--renderer sdl3 ...`` command line -- no ``--port`` needed, one is picked
and appended automatically) to run this test; skipped otherwise so a plain
``pytest`` run elsewhere doesn't error on a missing binary::

    WISH_NATIVE_AUTOMATION_SERVER_CMD="wish server --renderer sdl3" \\
        python -m pytest bindings/python/tests/test_native_automation.py -v

Headless by default: ``SDL_VIDEODRIVER=dummy`` / ``SDL_RENDER_DRIVER=software``
are set in the subprocess environment unless already present in the test
environment (see ``tests/test_sdl3_renderer.cpp``'s identical headless setup).
"""

import json
import os
import shlex
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from wish import Client
from wish.automation import _free_tcp_port, _wait_for_port

_WINDOW_JSON = json.dumps(
    {
        "type": "Window",
        "title": "Automation Test",
        "children": {
            "ok": {"type": "Button", "label": "OK"},
            "name": {"type": "InputText", "hint": "Type here"},
        },
    }
)


class TestNativeAutomation(unittest.TestCase):
    def setUp(self):
        cmd_str = os.environ.get("WISH_NATIVE_AUTOMATION_SERVER_CMD")
        if not cmd_str:
            self.skipTest("set WISH_NATIVE_AUTOMATION_SERVER_CMD to run native automation e2e tests")

        cmd = shlex.split(cmd_str)
        self.port = _free_tcp_port()
        if "--port" not in cmd:
            cmd += ["--transport", "tcp", "--port", str(self.port)]

        env = dict(os.environ)
        env.setdefault("SDL_VIDEODRIVER", "dummy")
        env.setdefault("SDL_RENDER_DRIVER", "software")

        self.process = subprocess.Popen(cmd, env=env)
        try:
            _wait_for_port("127.0.0.1", self.port, timeout=15.0)
        except Exception:
            self.process.terminate()
            self.process.wait(timeout=5)
            raise

    def tearDown(self):
        process = getattr(self, "process", None)
        if process is None:
            return
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)

    def test_drives_a_form_end_to_end(self):
        client = Client.tcp("127.0.0.1", self.port)
        result = {}

        def session_fn(c: Client) -> None:
            c.register_template("ui", _WINDOW_JSON)
            # Keep the root proxy referenced for the rest of this function --
            # a Proxy that goes out of scope is garbage-collected almost
            # immediately, and its destructor destroys the remote object
            # (see CLAUDE.md's automation gotchas).
            root_proxy = c.instantiate_template("ui", "ui")  # noqa: F841

            c.wait_for(lambda cc: (cc.get_widget("ui.ok") or {}).get("rect") is not None, timeout=10.0)

            tree = c.get_tree()
            result["has_ok_widget"] = any(w["path"] == "ui.ok" for w in tree["widgets"])

            c.click("ui.ok")
            c.type_text("ui.name", "Ada")
            c.wait_for(lambda cc: (cc.get_widget("ui.name") or {}).get("value") == "Ada", timeout=10.0)
            result["name_value"] = c.get_widget("ui.name")["value"]

            result["png_magic"] = c.screenshot()[:4]
            result["logs_is_list"] = isinstance(c.get_logs(), list)

        try:
            client.run(session_fn)
        finally:
            client.destroy()

        self.assertTrue(result.get("has_ok_widget"))
        self.assertEqual(result.get("name_value"), "Ada")
        self.assertEqual(result.get("png_magic"), b"\x89PNG")
        self.assertTrue(result.get("logs_is_list"))


if __name__ == "__main__":
    unittest.main()
