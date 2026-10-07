#!/usr/bin/env python3
# MIT License (c) 2025 Binary Dice Games
"""Screenshot an embedded wish app (``wish client --run=<name>``) headlessly.

Launches ``wish server --transport tcp --renderer web`` under
``wish.automation.AutomationClient``, connects ``wish client --run <name>``
to it, optionally drives a few inputs, and writes a PNG. Works with a build
that has no SDL3 (``wish standalone`` needs SDL3; this does not).

Requires a build configured with ``-DWISH_ENABLE_WEB=ON
-DWISH_ENABLE_AUTOMATION=ON`` (target ``wish-cli``) and the ``playwright``
Python package. See docs/automation.md, "Recipe: screenshot a module".

Example::

    python3 scripts/screenshot_module.py --run pix --out /tmp/pix.png \\
        --type .vbox.toolbar.path_input=/tmp/images --click-class Selectable

``--type``/``--click`` match widgets by dot-path *suffix*, so the
instance-numbered form root (``__pix_0``) need not be known in advance.
"""

import argparse
import glob
import os
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "bindings", "python"))


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _pin_chromium():
    """Point Playwright at a preinstalled Chromium when its own is missing.

    A pip-installed playwright expects a browser build matching its version;
    containers often ship a different one under PLAYWRIGHT_BROWSERS_PATH
    (e.g. /opt/pw-browsers/chromium-1194). Use that instead of downloading.
    """
    root = os.environ.get("PLAYWRIGHT_BROWSERS_PATH", "/opt/pw-browsers")
    candidates = sorted(glob.glob(os.path.join(root, "chromium-*", "chrome-linux*", "chrome")))
    if not candidates:
        return
    from playwright.sync_api._generated import BrowserType

    orig = BrowserType.launch

    def launch(self, **kw):
        try:
            return orig(self, **kw)
        except Exception:
            kw["executable_path"] = candidates[-1]
            return orig(self, **kw)

    BrowserType.launch = launch


def _find(ui, suffix=None, cls=None, timeout=15.0):
    """First rendered widget matching @p suffix / @p cls, polling up to
    @p timeout seconds (client-pushed content can land late)."""
    deadline = time.monotonic() + timeout
    while True:
        for w in ui.get_tree()["widgets"]:
            if (w.get("rect") and (suffix is None or w["path"].endswith(suffix))
                    and (cls is None or w.get("class") == cls)):
                return w
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.5)


def _fail(ui, out, msg):
    """Save what is on screen next to @p out, then exit with @p msg."""
    fail_png = os.path.splitext(out)[0] + ".fail.png"
    with open(fail_png, "wb") as f:
        f.write(ui.screenshot())
    sys.exit(f"{msg} (screen saved to {fail_png})")


def _click_rect(ui, w):
    r = w["rect"]
    # delay=60: ImGui must see press and release on different frames.
    ui._page.mouse.click((r["x0"] + r["x1"]) / 2, (r["y0"] + r["y1"]) / 2, delay=60)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", required=True, help="embedded app name (see `wish client --list`)")
    ap.add_argument("--out", required=True, help="PNG output path")
    ap.add_argument("--wish", default=os.path.join(REPO, "build", "app", "wish"))
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=800)
    ap.add_argument("--type", action="append", default=[], metavar="SUFFIX=TEXT",
                    help="type TEXT (then Enter) into the widget whose path ends with SUFFIX")
    ap.add_argument("--click", action="append", default=[], metavar="SUFFIX",
                    help="click the widget whose path ends with SUFFIX")
    ap.add_argument("--click-class", action="append", default=[], metavar="CLASS",
                    help="click the first rendered widget of this class (e.g. Selectable)")
    ap.add_argument("--arg", action="append", default=[], metavar="ARG",
                    help="positional argument passed to the app after `--` (repeatable)")
    ap.add_argument("--settle", type=float, default=3.0, help="seconds to wait after each action")
    ap.add_argument("--before-shot", type=float, default=3.0,
                    help="extra seconds before the screenshot, for async client work (uploads, previews)")
    ap.add_argument("--dump-tree", action="store_true", help="print top-level widget paths")
    args = ap.parse_args()

    _pin_chromium()
    from wish.automation import AutomationClient

    rmi_port = _free_port()
    server = [args.wish, "server", "--transport", "tcp", "--port", str(rmi_port), "--renderer", "web"]
    with AutomationClient.launch(server_cmd=server) as ui:
        ui._page.set_viewport_size({"width": args.width, "height": args.height})
        client = subprocess.Popen([args.wish, "client", "--transport", "tcp", "--host", "127.0.0.1",
                                   "--port", str(rmi_port), "--run", args.run]
                                  + (["--"] + args.arg if args.arg else []))
        try:
            # Wait until the app has rendered at least one Window.
            ui.wait_for("async () => (await window.wish.getTree()).widgets"
                        ".some(w => w.class === 'Window' && w.rect)")
            time.sleep(1.0)
            if args.dump_tree:
                print("\n".join(w["path"] for w in ui.get_tree()["widgets"] if w["path"].count(".") <= 1))
            for spec in args.type:
                suffix, text = spec.split("=", 1)
                w = _find(ui, suffix=suffix)
                if not w:
                    _fail(ui, args.out, f"no rendered widget ending in {suffix!r}")
                ui.type_text(w["path"], text)
                # delay=60: an instant keydown+keyup can land in one frame and be lost.
                ui._page.keyboard.press("Enter", delay=60)
                time.sleep(args.settle)
            for suffix in args.click:
                w = _find(ui, suffix=suffix)
                if not w:
                    _fail(ui, args.out, f"no rendered widget ending in {suffix!r}")
                _click_rect(ui, w)
                time.sleep(args.settle)
            for cls in args.click_class:
                w = _find(ui, cls=cls)
                if not w:
                    _fail(ui, args.out, f"no rendered {cls} widget")
                _click_rect(ui, w)
                time.sleep(args.settle)
            time.sleep(args.before_shot)
            with open(args.out, "wb") as f:
                f.write(ui.screenshot())
            print(f"wrote {args.out}")
        finally:
            client.terminate()
            client.wait(timeout=5)


if __name__ == "__main__":
    main()
