"""Live chart: a streaming signal plotted in real time, with a frequency
slider, a noise slider and a Pause checkbox.

    python bindings/python/examples/desktop/live_chart.py

Connects to the wish server or ``wish desktop`` named by the WISH_*
environment variables (default tcp 127.0.0.1:7070); see README.md.
"""

import json
import math
import random
import threading
import time

from desktop_app import run_app

POINTS = 200  # samples shown
RATE = 20.0  # samples per second

DESC = {
    "type": "Window",
    "title": "Live Chart",
    "width": 560,
    "height": 400,
    "closable": True,
    "children": {
        "controls": {
            "type": "HorizontalLayout",
            "children": {
                "freq_label": {"type": "Label", "text": "Frequency"},
                "freq": {"type": "SliderFloat", "label": "##freq", "value": 0.5, "min": 0.1, "max": 3.0,
                         "format": "%.1f Hz", "width": 110},
                "noise_label": {"type": "Label", "text": "Noise"},
                "noise": {"type": "SliderFloat", "label": "##noise", "value": 0.1, "min": 0.0, "max": 1.0,
                          "format": "%.2f", "width": 90},
                "pause": {"type": "Checkbox", "label": "Pause"},
            },
        },
        "plot": {
            "type": "Plot",
            "title": "##signal",
            "x_label": "seconds",
            "height": 300,
            "y_min": -2.0,
            "y_max": 2.0,
            "children": {
                "signal": {"type": "PlotLine", "label": "signal"},
                "clean": {"type": "PlotLine", "label": "sine"},
            },
        },
    },
}


class LiveChart:
    def __init__(self):
        self.freq = 0.5
        self.noise = 0.1
        self.paused = False
        self.t = 0.0
        self.xs, self.ys, self.clean = [], [], []
        self.stop = threading.Event()
        self.proxies = {}

    def proxy(self, client, path):
        if path not in self.proxies:
            self.proxies[path] = client.proxy_get(f"chart.{path}")
        return self.proxies[path]

    def sample(self):
        self.t += 1.0 / RATE
        clean = math.sin(2.0 * math.pi * self.freq * self.t)
        self.xs.append(round(self.t, 3))
        self.clean.append(clean)
        self.ys.append(clean + random.uniform(-self.noise, self.noise))
        del self.xs[:-POINTS], self.ys[:-POINTS], self.clean[:-POINTS]

    def session(self, client):
        client.register_template("live_chart", json.dumps(DESC))
        root = client.instantiate_template("live_chart", "chart")
        root.on_event("closed", lambda _p: client.quit())

        def on_freq(p):
            self.freq = float(p["value"])

        def on_noise(p):
            self.noise = float(p["value"])

        def on_pause(p):
            self.paused = bool(p["value"])

        self.proxy(client, "controls.freq").on_event("changed", on_freq)
        self.proxy(client, "controls.noise").on_event("changed", on_noise)
        self.proxy(client, "controls.pause").on_event("changed", on_pause)
        plot = self.proxy(client, "plot")
        signal = self.proxy(client, "plot.signal")
        clean = self.proxy(client, "plot.clean")

        def stream():
            while not self.stop.wait(1.0 / RATE):
                if self.paused:
                    continue
                self.sample()
                try:
                    # Scroll: the x axis follows the newest samples.
                    plot.set({"x_min": self.xs[0], "x_max": self.xs[-1]})
                    signal.set({"xs": list(self.xs), "ys": list(self.ys)})
                    clean.set({"xs": list(self.xs), "ys": list(self.clean)})
                except Exception:
                    return  # disconnected

        for _ in range(POINTS // 2):  # start with some history
            self.sample()
        thread = threading.Thread(target=stream, daemon=True)
        thread.start()
        client.wait()
        self.stop.set()
        thread.join()
        client.release("chart")


if __name__ == "__main__":
    run_app(LiveChart().session, "examples/python/live_chart", "Live Chart", "monitoring")
