"""Shared runner for the desktop example scripts.

Connects with ``Client.from_env()`` -- so a script started from a
``wish desktop`` terminal (or by any process that sets ``WISH_TRANSPORT`` /
``WISH_HOST`` / ``WISH_PORT`` / ``WISH_NAME``) needs no flags -- and sends the
app's identity in the connect params:

* ``app``: a stable identifier, e.g. ``"examples/python/todo"``;
* ``app_label``: the name the desktop's taskbar shows;
* ``app_icon``: an icon, one of the built-in ``res/icons/<name>.png``.

``wish desktop`` uses them to show the script on its taskbar like a built-in
tool; a plain ``wish server`` ignores them.
"""

import os
import sys
import threading
from typing import Callable

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from wish import Client  # noqa: E402


def run_app(session: Callable[[Client], None], app: str, label: str, icon: str) -> None:
    """Connect, run ``session(client)`` until it returns, then disconnect.

    ``session`` runs on the client's worker thread; it builds the UI and
    blocks in ``client.wait()`` until something calls ``client.quit()``
    (typically the window's "closed" event). Ctrl+C quits too.
    """
    client = Client.from_env()
    params = {"app": app, "app_label": label, "app_icon": f"res/icons/{icon}.png"}
    errors: list = []

    def worker() -> None:
        try:
            client.run(session, params=params)
        except Exception as exc:  # reported after the thread ends
            errors.append(exc)

    # client.run() blocks inside native code; keep the main thread free so
    # Ctrl+C is delivered (see examples/calculator_example.py).
    thread = threading.Thread(target=worker, daemon=True)
    thread.start()
    try:
        while thread.is_alive():
            thread.join(timeout=0.2)
    except KeyboardInterrupt:
        client.quit()
        thread.join()
    if errors:
        raise SystemExit(f"[{label}] {errors[0]}")
