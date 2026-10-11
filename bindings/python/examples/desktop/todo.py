"""To-do list: type a task and press Add, tick it off, delete it.

    python bindings/python/examples/desktop/todo.py

Connects to the wish server or ``wish desktop`` named by the WISH_*
environment variables (default tcp 127.0.0.1:7070); see README.md.
"""

import json

from desktop_app import run_app

MAX_ITEMS = 10

# Rows are created up front and shown/hidden: simpler than adding widgets
# at runtime, and plenty for a small list.
DESC = {
    "type": "Window",
    "title": "To-do",
    "width": 380,
    "height": 440,
    "closable": True,
    "children": {
        "add_row": {
            "type": "HorizontalLayout",
            "children": {
                # A plain InputText reports every keystroke, so Add always
                # sees the current text (EnterReturnsTrue would only report
                # it on Enter).
                "input": {"type": "InputText", "hint": "New task", "width": 270},
                "add": {"type": "Button", "label": "Add", "icon": "res/icons/add.png"},
            },
        },
        "sep": {"type": "Separator"},
        **{
            f"item{i}": {
                "type": "HorizontalLayout",
                "visible": False,
                "children": {
                    "done": {"type": "Checkbox", "label": f"##task{i}"},
                    "delete": {"type": "Button", "label": "", "icon": "res/icons/delete.png"},
                },
            }
            for i in range(MAX_ITEMS)
        },
        "sep2": {"type": "Separator"},
        "status": {"type": "Label", "text": "Nothing to do"},
    },
}


class Todo:
    def __init__(self):
        self.tasks = []  # [{"text": str, "done": bool}]
        self.typed = ""
        self.proxies = {}  # kept alive: they hold the event callbacks

    def proxy(self, client, path):
        if path not in self.proxies:
            self.proxies[path] = client.proxy_get(f"todo.{path}")
        return self.proxies[path]

    def refresh(self, client):
        for i in range(MAX_ITEMS):
            row = self.proxy(client, f"item{i}")
            if i < len(self.tasks):
                task = self.tasks[i]
                self.proxy(client, f"item{i}.done").set({"label": f"{task['text']}##task{i}", "value": task["done"]})
                row["visible"] = True
            else:
                row["visible"] = False
        done = sum(t["done"] for t in self.tasks)
        text = f"{done} of {len(self.tasks)} done" if self.tasks else "Nothing to do"
        self.proxy(client, "status")["text"] = text

    def add(self, client):
        text = self.typed.strip()
        if text and len(self.tasks) < MAX_ITEMS:
            self.tasks.append({"text": text, "done": False})
            self.typed = ""
            self.proxy(client, "add_row.input")["value"] = ""
            self.refresh(client)

    def session(self, client):
        client.register_template("todo", json.dumps(DESC))
        root = client.instantiate_template("todo", "todo")
        root.on_event("closed", lambda _p: client.quit())

        def on_typed(params):
            self.typed = params["value"]

        self.proxy(client, "add_row.input").on_event("changed", on_typed)

        def on_add(_p):
            self.add(client)

        self.proxy(client, "add_row.add").on_event("clicked", on_add)

        for i in range(MAX_ITEMS):

            def on_done(params, i=i):
                self.tasks[i]["done"] = bool(params["value"])
                self.refresh(client)

            def on_delete(_p, i=i):
                del self.tasks[i]
                self.refresh(client)

            self.proxy(client, f"item{i}.done").on_event("changed", on_done)
            self.proxy(client, f"item{i}.delete").on_event("clicked", on_delete)

        client.wait()
        client.release("todo")


if __name__ == "__main__":
    run_app(Todo().session, "examples/python/todo", "To-do", "check")
