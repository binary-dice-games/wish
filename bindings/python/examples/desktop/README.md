# Desktop example apps (Python, client ABI)

Three small apps built on the wish client C ABI through the Python binding,
made to run on a [`wish desktop`](../../../../docs/cli.md#wish-desktop):

| Script | What it shows |
|--------|---------------|
| `todo.py` | A to-do list: type a task and press Add, tick it off, delete it. |
| `stopwatch.py` | A stopwatch with Start/Stop, Lap and Reset, and a table of laps. Updated from a background thread. |
| `live_chart.py` | A live, scrolling chart of a noisy sine wave, with frequency and noise sliders and Pause. |

Each one connects with `Client.from_env()` (the `WISH_TRANSPORT` /
`WISH_HOST` / `WISH_PORT` / `WISH_NAME` environment variables, default tcp
`127.0.0.1:7070`) and names itself in its connect params, so the desktop
shows it on the taskbar with its own name and icon, like a built-in tool:

```python
client.run(session, params={"app": "examples/python/todo",   # identifier
                            "app_label": "To-do",            # taskbar name
                            "app_icon": "res/icons/check.png"})  # built-in icon
```

A plain `wish server` ignores these fields, so the scripts work there too.
The shared connect/run code is in `desktop_app.py`.

## Run

Build the client library first (`cmake --build build --target wish_client_dll`).

```sh
build/app/wish server --transport tcp &      # the window the desktop draws in
build/app/wish desktop --upstream_transport tcp
# in the terminal the desktop opens (WISH_* already point at the desktop):
python3 bindings/python/examples/desktop/todo.py &
python3 bindings/python/examples/desktop/stopwatch.py &
python3 bindings/python/examples/desktop/live_chart.py &
```

To start them from the Start menu, pin them, or put them on the desktop,
add them to the [desktop configuration file](../../../../docs/desktop-config.md)
(`~/.wish/desktop/desktop.yaml`):

```yaml
start_menu:
  pinned: [py.todo]
  items:
    - label: Python apps
      icon: res/icons/code.png
      items:
        - label: To-do
          id: py.todo
          icon: res/icons/check.png
          run: [python3, "{home}/wish/bindings/python/examples/desktop/todo.py"]
          app: examples/python/todo            # matches its connect params
          env: {WISH_LIB: "{home}/wish/build/libwish_client.so"}
    - apps: all
```

From any other shell, point them at the desktop yourself:
`WISH_PORT=7071 python3 bindings/python/examples/desktop/todo.py`.
Close a window (or press Ctrl+C) to end its script.
