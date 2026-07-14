"""Headless UI automation for wish apps -- drive a live ``wish server
--renderer web`` instance the way Playwright drives a browser.

Built entirely on `web_renderer`'s existing browser/WebSocket surface (see
``src/automation/DESIGN.md``): screenshots and input are plain Playwright
APIs against the page the server already serves; the only wish-specific
piece is the ``window.wish`` tree-query shim
(``resources/embedded/web/client.js``), which this module drives via
``page.evaluate()``. No native library, no ``ctypes`` -- unlike
:mod:`wish.client`, this does not touch ``wish_client_dll`` at all.

Requires the ``playwright`` package (``pip install playwright`` plus a
one-time ``playwright install chromium``, unless a Chromium install is
already configured via ``PLAYWRIGHT_BROWSERS_PATH`` -- e.g. Claude Code's
own execution environment ships one).

Quick start::

    from wish.automation import AutomationClient

    with AutomationClient.launch(server_cmd=["wish", "server", "--renderer", "web"]) as ui:
        tree = ui.get_tree()
        ui.click("dialog.ok")
        ui.type_text("form.name_input", "Ada Lovelace")
        png_bytes = ui.screenshot()
        ui.wait_for("async () => (await window.wish.getWidget('status.label'))?.text === 'Saved'")
"""

from __future__ import annotations

import socket
import subprocess
import time
from typing import Any, Dict, List, Optional, Sequence

__all__ = ["AutomationClient", "AutomationError"]


class AutomationError(RuntimeError):
    """Raised for automation-specific failures (missing widget, launch
    timeout, ...) -- distinct from a Playwright-raised error, which
    propagates unchanged."""


def _free_tcp_port() -> int:
    """Ask the OS for a currently-unused TCP port on localhost.

    Same "ask the OS, then reuse the number" approach `web_renderer` itself
    uses for `--web_port 0`, done here in Python since we need to know the
    port *before* launching the server subprocess (to build its URL).
    There is an inherent (tiny) race between closing this probe socket and
    the server binding the same port; acceptable for a local dev/test tool.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _wait_for_port(host: str, port: int, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    last_error: Optional[OSError] = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.1)
    raise AutomationError(f"server never opened {host}:{port} within {timeout}s") from last_error


class AutomationClient:
    """Drives one wish session through a headless Chromium tab.

    Construct via :meth:`launch`, not directly -- the constructor assumes
    every field is already populated. Use as a context manager so the
    browser and (if launched) the server subprocess are always cleaned up::

        with AutomationClient.launch(server_cmd=[...]) as ui:
            ...
    """

    def __init__(self, playwright: Any, browser: Any, page: Any, process: Optional[subprocess.Popen]):
        self._playwright = playwright
        self._browser = browser
        self._page = page
        self._process = process
        self._closed = False

    # ── construction ──────────────────────────────────────────────────────

    @classmethod
    def launch(
        cls,
        server_cmd: Optional[Sequence[str]] = None,
        url: Optional[str] = None,
        headless: bool = True,
        startup_timeout: float = 15.0,
    ) -> "AutomationClient":
        """Start (or attach to) a wish web-renderer server and open it in a
        headless Chromium tab.

        Exactly one of @p server_cmd / @p url should be given:

        - **server_cmd**: argv for a ``wish server --renderer web ...``
          subprocess. If the command doesn't already include ``--web_port``,
          a free port is chosen and appended automatically, and the client
          waits for the server to start accepting connections on it before
          navigating.
        - **url**: attach to an already-running server instead of launching
          one (e.g. a server an agent started manually, or in another
          terminal, during interactive use).

        @param server_cmd  Subprocess argv, or `None` to attach via @p url.
        @param url  Existing server's URL (e.g. `"http://127.0.0.1:8080"`),
                     or `None` to launch @p server_cmd.
        @param headless  Whether Chromium runs headless (default `True`; the
                          only sensible choice in a CI/agent environment).
        @param startup_timeout  Seconds to wait for the server's HTTP port
                                  (when launching) and for `window.wish.ready`
                                  (always) before raising `AutomationError`.
        @return  A ready-to-use `AutomationClient`; `window.wish.ready` has
                  already resolved `True` by the time this returns.
        """
        if (server_cmd is None) == (url is None):
            raise AutomationError("launch() needs exactly one of server_cmd= or url=")

        process: Optional[subprocess.Popen] = None
        if server_cmd is not None:
            cmd = list(server_cmd)
            if "--web_port" not in cmd:
                port = _free_tcp_port()
                cmd += ["--web_port", str(port)]
            else:
                port = int(cmd[cmd.index("--web_port") + 1])
            process = subprocess.Popen(cmd)
            url = f"http://127.0.0.1:{port}"
            try:
                _wait_for_port("127.0.0.1", port, startup_timeout)
            except AutomationError:
                process.terminate()
                process.wait(timeout=5)
                raise

        # Imported lazily so importing wish.automation doesn't hard-require
        # playwright for callers who only use wish.client.
        from playwright.sync_api import sync_playwright

        playwright = sync_playwright().start()
        try:
            # Headless Chromium disables GPU/WebGL by default, which leaves
            # wish's WebGL2 canvas blank in screenshot()/get_tree() rects
            # that depend on a rendered frame. Force a software rasterizer
            # so the canvas actually renders in headless mode.
            browser = playwright.chromium.launch(
                headless=headless,
                args=[
                    "--use-gl=angle",
                    "--use-angle=swiftshader",
                    "--enable-unsafe-swiftshader",
                    "--ignore-gpu-blocklist",
                ],
            )
            page = browser.new_page()
            page.goto(url)
            page.wait_for_function("() => window.wish && window.wish.ready === true", timeout=startup_timeout * 1000)
        except Exception:
            playwright.stop()
            if process is not None:
                process.terminate()
                process.wait(timeout=5)
            raise

        return cls(playwright, browser, page, process)

    def __enter__(self) -> "AutomationClient":
        return self

    def __exit__(self, *exc_info: Any) -> None:
        self.close()

    def close(self) -> None:
        """Close the browser, then terminate the server subprocess (if this
        client launched one). Safe to call more than once."""
        if self._closed:
            return
        self._closed = True
        self._browser.close()
        self._playwright.stop()
        if self._process is not None:
            self._process.terminate()
            try:
                self._process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._process.kill()
                self._process.wait(timeout=5)

    # ── tree queries ─────────────────────────────────────────────────────

    def get_tree(self, root: str = "") -> Dict[str, Any]:
        """Return `{"request_id": N, "widgets": [...]}` for @p root and its
        descendants (the whole tree when @p root is empty). See
        `automation::build_tree_snapshot()` (src/automation/automation_query.hpp)
        for each widget entry's shape."""
        return self._page.evaluate("(root) => window.wish.getTree(root)", root)

    def get_widgets(self, root: str = "") -> List[Dict[str, Any]]:
        """Convenience wrapper: just the `widgets` list from `get_tree()`."""
        return self.get_tree(root)["widgets"]

    def get_widget(self, path: str) -> Optional[Dict[str, Any]]:
        """Return one widget's snapshot entry by exact dot-path, or `None`
        if @p path does not currently exist in the tree."""
        return self._page.evaluate("(path) => window.wish.getWidget(path)", path)

    def get_logs(self) -> List[Dict[str, Any]]:
        """Return every log entry received so far, oldest first: each
        `{"seq": N, "timestamp": "...", "level": "info", "message": "..."}`
        (see `logger::log_entry`, `src/context/logger.hpp`).

        Entries are pushed live as LOG_EVENT messages arrive (no server
        round trip here -- this just reads what `window.wish.logs` has
        already accumulated), in the exact order `log()` was called. That
        means a log entry's position relative to this script's own actions
        tells you when it happened without any extra correlation -- e.g.::

            before = len(ui.get_logs())
            ui.click("dialog.ok")
            ui.wait_for(f"() => window.wish.logs.length > {before}")
            assert ui.get_logs()[-1]["message"] == "saved"

        confirms that log line was caused by the click, not something
        logged earlier or later. See `CLAUDE.md`'s "Automation" section.
        """
        return self._page.evaluate("() => window.wish.getLogs()")

    # ── input ────────────────────────────────────────────────────────────

    def _widget_center(self, path: str) -> tuple:
        widget = self.get_widget(path)
        if widget is None:
            raise AutomationError(f"no widget at path {path!r}")
        rect = widget.get("rect")
        if rect is None:
            raise AutomationError(f"widget {path!r} exists but was never rendered (no rect)")
        return ((rect["x0"] + rect["x1"]) / 2, (rect["y0"] + rect["y1"]) / 2)

    def click(self, path: str) -> None:
        """Click the center of the widget at @p path.

        Resolves @p path to a screen rect via `get_widget()`, then issues a
        real `page.mouse.click()` -- a real DOM/CDP mouse event, forwarded
        by `client.js`'s existing listeners into an INPUT WebSocket message
        exactly as a human user's click would be. Raises `AutomationError`
        if @p path doesn't exist or was never rendered.
        """
        cx, cy = self._widget_center(path)
        self._page.mouse.click(cx, cy)

    def type_text(self, path: str, text: str) -> None:
        """Click the widget at @p path to focus it, then type @p text one
        keystroke at a time via `page.keyboard.type()`."""
        self.click(path)
        self._page.keyboard.type(text)

    # ── observation ──────────────────────────────────────────────────────

    def screenshot(self) -> bytes:
        """Return a PNG screenshot of the current page (`page.screenshot()`)
        -- pixel-perfect output of exactly what the browser rendered."""
        return self._page.screenshot()

    def wait_for(self, js_predicate: str, timeout: Optional[float] = None) -> None:
        """Block until @p js_predicate evaluates truthy in the page.

        @p js_predicate may itself be `async` and call `window.wish.getTree()`
        / `getWidget()` (both return Promises) -- Playwright's
        `page.wait_for_function()` awaits a returned Promise on every poll,
        so e.g. `"async () => (await window.wish.getWidget('status.label'))?.text === 'Saved'"`
        works with no extra plumbing on the Python side.

        @param timeout  Seconds to wait before raising (Playwright's own
                          `TimeoutError`); `None` uses Playwright's default.
        """
        self._page.wait_for_function(js_predicate, timeout=timeout * 1000 if timeout is not None else None)

    @property
    def page(self) -> Any:
        """The underlying `playwright.sync_api.Page`, for anything this
        wrapper doesn't cover directly (custom `evaluate()` calls, network
        interception, tracing, ...)."""
        return self._page
