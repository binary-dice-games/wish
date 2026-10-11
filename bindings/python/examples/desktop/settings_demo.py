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
    common = {"category": "example"}
    client.describe_setting(
        f"{ENTRY}.enabled", type="bool", label="Show the banner", section="Banner", default=True, order=0, **common
    )
    client.describe_setting(
        f"{ENTRY}.message",
        type="string",
        label="Message",
        description="Text of the banner.",
        section="Banner",
        default="Hello from Python",
        order=1,
        **common,
    )
    client.describe_setting(
        f"{ENTRY}.color", type="color", label="Color", section="Banner", default="#2F7BF5", order=2, **common
    )
    client.describe_setting(
        f"{ENTRY}.size",
        type="int",
        label="Text size",
        section="Banner",
        min=10,
        max=40,
        default=18,
        order=3,
        **common,
    )
    client.describe_setting(
        f"{ENTRY}.mood",
        type="enum",
        label="Mood",
        description="Picks the emoji shown next to the message.",
        section="Behavior",
        options=[("happy", "Happy"), ("calm", "Calm"), ("busy", "Busy")],
        default="happy",
        **common,
    )
    client.describe_setting(
        f"{ENTRY}.speed",
        type="float",
        label="Refresh interval",
        description="Seconds between reads of the settings.",
        section="Behavior",
        min=0.5,
        max=3.0,
        default=1.0,
        **common,
    )


def read(client):
    """Current values, with the defaults for the ones never set.

    The entry is read as JSON: field names are stored as hashes ("#<key>"),
    and JSON keeps each value's type (indexing a Dynamic would read a
    missing field, or false, as 0)."""
    from bison import key

    values = {"enabled": True, "message": "Hello from Python", "color": "#2F7BF5", "size": 18, "mood": "happy", "speed": 1.0}
    entry = client.user_store_get(ENTRY)
    if entry is not None:
        stored = json.loads(entry.to_json())
        entry.release()
        for name, default in values.items():
            value = stored.get(f"#{key(name)}")
            if value is not None and isinstance(value, (int, float)) == isinstance(default, (int, float)):
                values[name] = value
    return values


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
