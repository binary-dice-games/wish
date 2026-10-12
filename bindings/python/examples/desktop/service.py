"""Service: a background process that shows a status window on the desktop
whenever a ``wish desktop`` is running.

    python bindings/python/examples/desktop/service.py

Start it before or after the desktop: it waits for one, shows its window,
and when the desktop closes it keeps working and waits for the next one, so
the window comes back when the desktop is started again. Ctrl+C stops it.

It connects to the desktop's fixed endpoint (``Client.desktop()``), not to
the WISH_* variables the other examples use; see README.md.
"""

import json
import os
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from wish import Client  # noqa: E402

DESC = {
    "type": "Window",
    "title": "Example service",
    "width": 320,
    "height": 170,
    "closable": False,  # the window lives as long as the desktop does
    "children": {
        "uptime": {"type": "Label", "text": ""},
        "jobs": {"type": "Label", "text": ""},
        "sessions": {"type": "Label", "text": ""},
    },
}


class Service:
    """Stands in for real work that runs whether or not a desktop is up."""

    def __init__(self):
        self.started = time.monotonic()
        self.jobs = 0
        self.sessions = 0
        self.stopping = threading.Event()

    def work(self):
        while not self.stopping.wait(0.5):
            self.jobs += 1

    def session(self, client):
        """One desktop connection: show the window, keep it current."""
        self.sessions += 1
        client.register_template("service", json.dumps(DESC))
        # Keep the root proxy: dropping it releases the window.
        root = client.instantiate_template("service", "svc")  # noqa: F841
        labels = {name: client.proxy_get(f"svc.{name}") for name in ("uptime", "jobs", "sessions")}
        labels["sessions"]["text"] = f"Desktop sessions: {self.sessions}"

        session_over = threading.Event()

        def refresh():
            while not session_over.wait(0.5):
                try:
                    labels["uptime"]["text"] = f"Up for {int(time.monotonic() - self.started)} s"
                    labels["jobs"]["text"] = f"Jobs done: {self.jobs}"
                except Exception:
                    return  # the desktop went away

        updater = threading.Thread(target=refresh, daemon=True)
        updater.start()
        client.wait()  # returns when the desktop closes, or on stop_desktop()
        session_over.set()
        updater.join()


def main():
    service = Service()
    threading.Thread(target=service.work, daemon=True).start()

    client = Client.desktop()
    params = {"app": "examples/python/service", "app_label": "Service", "app_icon": "res/icons/settings.png"}
    errors: list = []

    def loop():
        try:
            client.run_on_desktop(service.session, params=params)
        except Exception as exc:  # reported after the thread ends
            errors.append(exc)

    # run_on_desktop() blocks inside native code; keep the main thread free
    # so Ctrl+C is delivered.
    thread = threading.Thread(target=loop, daemon=True)
    thread.start()
    print("[service] running; waiting for a wish desktop (Ctrl+C to stop)")
    try:
        while thread.is_alive():
            thread.join(timeout=0.2)
    except KeyboardInterrupt:
        client.stop_desktop()
        thread.join()
    service.stopping.set()
    if errors:
        raise SystemExit(f"[service] {errors[0]}")


if __name__ == "__main__":
    main()
