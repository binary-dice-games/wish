"""Stopwatch: Start/Stop, Lap and Reset, with a table of lap times.

    python bindings/python/examples/desktop/stopwatch.py

Connects to the wish server or ``wish desktop`` named by the WISH_*
environment variables (default tcp 127.0.0.1:7070); see README.md.
"""

import json
import threading
import time

from desktop_app import run_app

MAX_LAPS = 8

DESC = {
    "type": "Window",
    "title": "Stopwatch",
    "width": 340,
    "height": 400,
    "closable": True,
    "children": {
        "time": {"type": "Label", "text": "00:00.0", "font_size": 48},
        "buttons": {
            "type": "HorizontalLayout",
            "children": {
                "start": {"type": "Button", "label": "Start", "icon": "res/icons/play.png", "width": 96},
                "lap": {"type": "Button", "label": "Lap", "icon": "res/icons/tag.png", "width": 96},
                "reset": {"type": "Button", "label": "Reset", "icon": "res/icons/refresh.png", "width": 96},
            },
        },
        "laps": {
            "type": "Table",
            "columns": 3,
            "headers": True,
            "flags": "RowBg|Borders",
            "children": {
                "c0": {"type": "TableColumn", "label": "Lap"},
                "c1": {"type": "TableColumn", "label": "Lap time"},
                "c2": {"type": "TableColumn", "label": "Total"},
                **{
                    f"row{i}": {
                        "type": "TableRow",
                        "visible": False,
                        "children": {
                            "n": {"type": "Label", "text": ""},
                            "lap": {"type": "Label", "text": ""},
                            "total": {"type": "Label", "text": ""},
                        },
                    }
                    for i in range(MAX_LAPS)
                },
            },
        },
    },
}


def fmt(seconds: float) -> str:
    minutes, rest = divmod(seconds, 60.0)
    return f"{int(minutes):02d}:{rest:04.1f}"


class Stopwatch:
    def __init__(self):
        self.running = False
        self.started_at = 0.0
        self.accumulated = 0.0
        self.laps = []  # totals, in seconds
        self.stop_ticker = threading.Event()
        self.proxies = {}

    def proxy(self, client, path):
        if path not in self.proxies:
            self.proxies[path] = client.proxy_get(f"sw.{path}")
        return self.proxies[path]

    def elapsed(self) -> float:
        return self.accumulated + (time.monotonic() - self.started_at if self.running else 0.0)

    def show_laps(self, client):
        for i in range(MAX_LAPS):
            row = self.proxy(client, f"laps.row{i}")
            if i < len(self.laps):
                total = self.laps[-1 - i]  # newest first
                n = len(self.laps) - i
                prev = self.laps[-2 - i] if len(self.laps) > i + 1 else 0.0
                self.proxy(client, f"laps.row{i}.n")["text"] = str(n)
                self.proxy(client, f"laps.row{i}.lap")["text"] = fmt(total - prev)
                self.proxy(client, f"laps.row{i}.total")["text"] = fmt(total)
                row["visible"] = True
            else:
                row["visible"] = False

    def session(self, client):
        client.register_template("stopwatch", json.dumps(DESC))
        root = client.instantiate_template("stopwatch", "sw")
        root.on_event("closed", lambda _p: client.quit())
        label = self.proxy(client, "time")

        def on_start(_p):
            if self.running:
                self.accumulated = self.elapsed()
                self.running = False
            else:
                self.started_at = time.monotonic()
                self.running = True
            start = self.proxy(client, "buttons.start")
            start.set({"label": "Stop" if self.running else "Start",
                       "icon": "res/icons/pause.png" if self.running else "res/icons/play.png"})

        def on_lap(_p):
            if self.running and len(self.laps) < 99:
                self.laps.append(self.elapsed())
                self.show_laps(client)

        def on_reset(_p):
            self.running = False
            self.accumulated = 0.0
            self.laps.clear()
            self.proxy(client, "buttons.start").set({"label": "Start", "icon": "res/icons/play.png"})
            label["text"] = fmt(0.0)
            self.show_laps(client)

        self.proxy(client, "buttons.start").on_event("clicked", on_start)
        self.proxy(client, "buttons.lap").on_event("clicked", on_lap)
        self.proxy(client, "buttons.reset").on_event("clicked", on_reset)

        # The display ticks from its own thread, ten times a second.
        def tick():
            while not self.stop_ticker.wait(0.1):
                if self.running:
                    try:
                        label["text"] = fmt(self.elapsed())
                    except Exception:
                        return  # disconnected

        ticker = threading.Thread(target=tick, daemon=True)
        ticker.start()
        client.wait()
        self.stop_ticker.set()
        ticker.join()
        client.release("sw")


if __name__ == "__main__":
    run_app(Stopwatch().session, "examples/python/stopwatch", "Stopwatch", "schedule")
