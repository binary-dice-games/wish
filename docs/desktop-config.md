# Desktop configuration file

`wish desktop` reads its configuration from a per-user YAML (or JSON) file,
`~/.wish/users/<user>/desktop/desktop.yaml` by default, in the profile of
the user the desktop connects as (`--upstream_username`, else the OS user;
see [wish-home.md](wish-home.md)). `--config PATH` uses another one. If the file doesn't exist, the desktop writes the default configuration
there and uses it. The desktop watches the file: save it in any editor and the
running desktop reloads it. The default Start menu has a pinned **Settings** entry (the
[Settings tool](settings.md), `bdg/desktop/conf`) and an **Edit desktop
settings** entry that opens this file in nano.

The file is a map of sections, each configuring one part of the desktop. A
missing section or setting keeps its default; unknown sections and settings
are ignored, so files stay valid as new ones are added.

```yaml
version: 1
appearance:   # wallpaper, background, theme, desktop icons
  ...
taskbar:      # the clock
  ...
start_menu:   # the Start menu, pins and desktop icons
  ...
```

## `appearance`

```yaml
appearance:
  wallpaper: ~/Pictures/beach.jpg
  wallpaper_fit: cover
  background: "#1E3A5F"
  theme: dark
  desktop_icons: true
```

| Setting | Default | Meaning |
|---------|---------|---------|
| `wallpaper` | *(none)* | Image file (png, jpg, ...) on the desktop's machine. A relative path is relative to this file's directory; `~/` and `{home}` expand. Empty draws the built-in gradient. The `--wallpaper` flag overrides it. |
| `wallpaper_fit` | `cover` | How the image fills the screen: `cover` (fill, cropping the overflow), `contain` (whole image, centered), `stretch`, `center` (actual size) or `tile`. |
| `background` | *(gradient)* | Color as `"#RRGGBB"` or `"#RRGGBBAA"` (quote it: `#` starts a YAML comment). Replaces the built-in gradient and shows around a wallpaper that doesn't cover the screen. |
| `theme` | *(none)* | Theme preset (`wish`, `light`, `dark`, `classic`, or any theme the server registered) applied when the desktop starts and whenever you change it here. In between, the tray's theme menu still switches it. Leave it out to keep the theme last chosen in the menu. |
| `desktop_icons` | `true` | Shows the desktop icons. |

The image is uploaded to the server the desktop draws on, so it doesn't need
to be on that machine. The desktop also watches the image file: saving a new
image under the same name updates the wallpaper. If the image can't be read,
the desktop shows why at the top of the Start menu and draws the background
instead.

## `taskbar`

```yaml
taskbar:
  time_format: "%H:%M"      # 14:05
  date_format: "%d/%m/%Y"   # 11/10/2026
```

`strftime()` formats of the two lines of the taskbar clock, e.g.
`"%I:%M %p"` for 02:05 PM or `"%a %d %b"` for Sun 11 Oct.

## `start_menu`

```yaml
start_menu:
  pinned: [bdg/desktop/nano, py.todo]   # taskbar pins, by entry id
  desktop: [bdg/desktop/nano]           # desktop icons, by entry id
  items:                                # the Start menu tree
    - apps: all                         # every built-in app, a folder per collection
    - label: Python apps                # a folder
      icon: res/icons/code.png
      items:
        - label: To-do
          id: py.todo
          icon: res/icons/check.png
          tooltip: A to-do list
          run: [python3, "{home}/scripts/todo.py"]
          app: examples/python/todo
          env: {WISH_LIB: /opt/wish/lib/libwish_client.so}
    - separator: true
    - label: Dark theme
      icon: res/icons/dark_mode.png
      action: set_theme
      args: {preset: dark}
  power:                                # the power button's popup
    - label: Quit desktop
      icon: res/icons/logout.png
      action: quit
```

`pinned` and `desktop` set the initial taskbar pins and desktop icons; ids
that match no entry are ignored. Once you pin, unpin or move an icon from the
desktop itself, your choices are saved in the upstream server's
[user store](persistent-store.md) (entry `wish.desktop`) and win over these
lists from then on. `power` lists the entries of the power button at the
bottom of the Start menu; without it the button is hidden.

### Entries

Each entry is one of:

| Key | Entry |
|-----|-------|
| `run` | Starts a process: a list of arguments, or one string split on spaces (`'…'` and `"…"` quote). |
| `action` (+ `args`) | Calls a desktop action (see [Actions](#actions)), e.g. `action: set_theme` with `args: {preset: dark}`. |
| `items` | A folder holding more entries. |
| `separator: true` | A separator line. |
| `apps: all` / `apps: <collection>` | Expands to the apps built into wish (all of them, a folder per collection, or one collection's apps). Each gets `id` = `app` = its qualified name (e.g. `bdg/desktop/nano`) and runs `{wish_client} --run=<name>`. |

Optional keys:

| Key | Meaning |
|-----|---------|
| `label` | Name in the Start menu, on the taskbar and under the desktop icon. |
| `id` | Unique id used by `pinned`, `desktop` and the `launch` action. Default: derived from the folder and entry labels (`python-apps/to-do`). |
| `icon` | Image relative to the session's resource folder, e.g. `res/icons/edit.png`. |
| `tooltip` | Tooltip text; also matched by the Start menu search. |
| `app` | (`run`) The `"app"` connect param of the client it starts. It ties the client's windows to this entry, for the taskbar icon and the running mark. Default: the `--run=` value of the command. |
| `env` | (`run`) Environment variables, set on top of the `WISH_TRANSPORT`/`WISH_HOST`/`WISH_PORT`/`WISH_NAME` that point the process at the desktop. |
| `cwd` | (`run`) Working directory. |
| `pinned`, `desktop` | `true` to pin the entry or add its desktop icon, like the section's lists. |

`run`, `cwd` and `env` values expand these placeholders:

| Placeholder | Expands to |
|-------------|------------|
| `{wish_client}` | The command that runs a wish client: `wish client`, or the `wish-client` beside a standalone `wish-desktop` binary. As a whole `run` token it expands to all of its arguments. |
| `{config_file}` | This file's path. |
| `{home}` | Your home directory. |

### Installing a client-ABI app

Any program built on the wish client ABI (`wish_client_c.h` and the bindings)
can be added as a `run` entry, so it can be started, pinned and put on the
desktop like a built-in tool. Give it the `app` it connects as, so its
windows are tied to the entry; the `py.todo` entry above installs
`bindings/python/examples/desktop/todo.py`.

## Errors

Errors name the offending setting, e.g.
`start_menu.items[2].run: expected a list or a string` or
`appearance.wallpaper_fit: expected cover, contain, stretch, center or tile`.
If the file is invalid when the desktop starts, the desktop uses the default
configuration and shows the error in red at the top of the Start menu,
without overwriting your file. If it becomes invalid while running, the
current configuration stays and the error is shown until a valid file is
saved.

## Actions

Actions are what `action:` entries call. Connected apps can call them too,
through a `__WishDesktop` object that the desktop serves itself (it is never
relayed to the upstream server):

| Action | Arguments | Effect |
|--------|-----------|--------|
| `quit` | — | Quits the desktop (and closes the apps it started). |
| `shutdown_server` | — | Asks the upstream server to shut down (its operator confirms first). |
| `set_theme` | `preset` (`wish`, `light`, `dark`, `classic`, ...) | Applies and saves a theme preset. |
| `toggle_theme` | — | Switches to the next of the server's themes (wish, light, dark, classic, then any the server registered), wrapping around. |
| `reload_config` | — | Re-reads the configuration file. |
| `launch` | `id` | Starts a Start menu entry; returns `{ok}`. |
| `items` | — | (RMI only) Returns `{items: [{id, label, app}, ...]}` for the `run`/`action` entries. |

Apps can start only menu entries, by id; they can't run arbitrary commands.

```python
from wish import Client

def session(client):
    desktop = client.instantiate("__WishDesktop", "wish")
    if not desktop.call("launch", {"id": "py.todo"})["ok"]:
        print("no such entry")

Client.from_env().run(session)
```
