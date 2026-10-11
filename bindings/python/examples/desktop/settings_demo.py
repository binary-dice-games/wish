"""Settings demo: declares its settings so the Settings tool (bdg/desktop/conf)
can edit them, and shows a banner that follows them live.

    python bindings/python/examples/desktop/settings_demo.py

Settings live in the user store, so the script connects with a user name:
``WISH_USERNAME`` if set, else your OS user name (on ``wish desktop`` the
desktop's own identity is used). Open Settings (``wish client --run conf``,
or Start > Settings on the desktop) and pick "Example" in the sidebar.
"""

import getpass
import json
import os
import threading

from desktop_app import run_app

ENTRY = "bdg.desktop.example"

DESC = {
    "type": "Window",
    "title": "Settings demo",
    "width": 420,
    "height": 220,
    "closable": True,
    "children": {
        "banner": {"type": "Label", "text": "", "wrap": True},
        "sep": {"type": "Separator"},
        "status": {"type": "Label", "text": "", "text_color": "#8E8E93FF"},
    },
}


# Every setting's metadata, keyed by field. describe_setting() records it for
# the Settings tool; read_setting() takes the same keywords, so it can check
# the stored value against them and fall back to the default.
SETTINGS = {
    "enabled": dict(type="bool", label="Show the banner", section="Banner", default=True, order=0),
    "message": dict(
        type="string", label="Message", description="Text of the banner.", section="Banner",
        default="Hello from Python", order=1,
    ),
    "color": dict(type="color", label="Color", section="Banner", default="#2F7BF5", order=2),
    "size": dict(type="int", label="Text size", section="Banner", min=10, max=40, default=18, order=3),
    "mood": dict(
        type="enum", label="Mood", description="Picks the emoji shown next to the message.", section="Behavior",
        options=[("happy", "Happy"), ("calm", "Calm"), ("busy", "Busy")], default="happy",
    ),
    "speed": dict(
        type="float", label="Refresh interval", description="Seconds between reads of the settings.",
        section="Behavior", min=0.5, max=3.0, default=1.0,
    ),
}


def describe(client):
    """Declares the "Example" category and its settings (safe to repeat)."""
    client.describe_settings_category(
        "example",
        label="Example",
        description="Settings of the Python settings demo.",
        icon="res/icons/palette.png",
        color="#AF52DE",
        order=10,
    )
    for field, meta in SETTINGS.items():
        client.describe_setting(f"{ENTRY}.{field}", category="example", **meta)


def read(client):
    """Current values: the stored ones when valid, else the defaults."""
    return {field: client.read_setting(f"{ENTRY}.{field}", **meta) for field, meta in SETTINGS.items()}


MOODS = {"happy": ":-)", "calm": ":-|", "busy": ":-O"}


def session(client):
    if not client.has_user_store():
        raise RuntimeError("no user store: connect with a user name (WISH_USERNAME)")
    describe(client)
    client.register_template("settings_demo", json.dumps(DESC))
    root = client.instantiate_template("settings_demo", "demo")
    stop = threading.Event()
    root.on_event("closed", lambda _p: (stop.set(), client.quit()))
    banner = client.proxy_get("demo.banner")
    status = client.proxy_get("demo.status")

    def loop():
        while not stop.is_set():
            v = read(client)
            color = v["color"] if len(v["color"]) == 9 else v["color"] + "FF"
            banner.set(
                {
                    "text": f"{v['message']}  {MOODS.get(v['mood'], '')}",
                    "visible": bool(v["enabled"]),
                    "text_color": color,
                    "font_size": float(v["size"]),
                }
            )
            status["text"] = f"Read every {float(v['speed']):.1f} s from the user store entry {ENTRY}"
            print(json.dumps(v), flush=True)
            stop.wait(max(0.5, float(v["speed"])))

    threading.Thread(target=loop, daemon=True).start()
    client.wait()
    stop.set()
    client.release("demo")


if __name__ == "__main__":
    user = os.environ.get("WISH_USERNAME") or getpass.getuser()
    run_app(session, "examples/python/settings_demo", "Settings demo", "tune", {"username": user})
