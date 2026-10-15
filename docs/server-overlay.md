# Server overlay and notifications

`wish server` has an overlay, like the Steam client's in-game overlay: a
full-screen panel the operator opens on top of every client's windows to
see what the server is doing and act on it. Notifications ("toasts") pop up
in a corner of the screen. The first one, at startup, shows the overlay's
key combination.

```sh
wish server --renderer sdl3            # a toast says: press Shift+F1
wish server --overlay_hotkey=Ctrl+Shift+F12
wish server --overlay_hotkey=          # no overlay, no startup toast
```

## Opening and closing

- **Shift+F1** (`--overlay_hotkey`) toggles it. **Server → Overlay** in the
  menu bar opens it too.
- **Esc** or the round **×** at the top right closes it.
- While it is open, the overlay takes the mouse and keyboard; the apps get
  them back when it closes.

The hotkey uses the same syntax as widget shortcuts (`Ctrl`, `Shift`,
`Alt`, `Super`, plus one key: `F1`-`F24`, letters, digits, ...; see
[ui-elements.md](ui-elements.md#keyboard-navigation-access-keys-and-shortcuts)).
An invalid value prints a warning and disables the shortcut. The flag can be
saved with `--save_defaults`. `wish desktop --with_server` has the overlay
too.

## Pages

| Page | Shows |
|------|-------|
| **Overview** | Tiles: uptime, connected clients and their windows, the server's memory, the frame rate. A **Server** card: version, endpoint, renderer, hotkey, log file, store directory, sandboxes, settings admins. A **This machine** card: host name, OS, user, CPUs, load average, process id, memory in use. The last clients that connected. |
| **Clients** | One row per session: identity (`admin` and `store` badges), session id, language, window titles, time connected, and an **Open** button for its sandbox folder. |
| **Settings** | An editor for the described settings ([settings.md](settings.md)) of the **server store** and of each **user store**: users connected now and users with a store file. A sidebar of categories, a search box, an editor per type (switch, slider, drop-down, color, text), **Reset** to the default, and a *restart* badge on settings that apply at the next start. Changes are saved right away. |
| **Logs** | The end of `wish_logs/server.log` (the last 2000 lines), refreshed live, with a filter, **Follow**, **Copy** and **Open file**. Errors are red, warnings orange. Nothing is logged unless the server runs with `--verbose` (see [cli.md](cli.md#log-verbosity)). |
| **About** | Version, links to the project on GitHub, the documentation and the issue tracker, and the keyboard shortcuts. |

The overlay is the server's local operator, so its Settings page can change
the server store whatever `--settings_admins` says. Only open the server's
window to people you trust with it.

## Quick actions

The bottom bar has:

- **Open sandbox folder**: the `--sandbox_root` directory, or, without one,
  a menu of the connected clients' temporary sandboxes.
- **Open server log** and **Logs folder**.
- **GitHub** and **Documentation**.
- **Shut down server**, after a confirmation that says how many clients
  will be disconnected.

Folders, files and links open with the server machine's default handler
(Explorer / `xdg-open`, or `explorer.exe` under WSL). With `--renderer web`
they open on the machine running the server, not in the browser that shows
it; a failure shows an error toast.

## Notifications for developers

Notifications are generic: any server-side code can post one. They are
thread-safe, drawn on top of everything (the overlay too), stacked in the
bottom-right corner (above `wish desktop`'s taskbar), fade in and out, show
their remaining time, and close on a click.

```cpp
#include <server/notifications.hpp>

wish::notification n;
n.title = "Backup finished";
n.body = "42 files copied to /mnt/backup.";
n.kind = wish::notification_kind::success;   // info, success, warning, error
n.keys = "Ctrl+B";                           // optional key caps under the text
n.duration = std::chrono::seconds{8};        // 0: until clicked
n.tag = "myapp.backup";                      // optional: replaces the previous one with this tag
sess().notifications->post(n);               // from a server form
```

- From a server form or render code: `context::notifications` (null only in
  a context made without a server, e.g. in tests).
- From code that owns the server: `renderer::notifications()`.
- `notification_center::dismiss(id)` removes one early; `post()` returns
  the id.
- At most 16 are kept and 4 drawn; the oldest go first.

Clients can't post notifications over RMI yet.

## Limitations

- An app showing a **modal dialog** keeps the mouse to that dialog, as
  ImGui does for every window, so the overlay can't be clicked until the
  dialog closes. The hotkey and Esc still work.
- `wish standalone` shows notifications but has no overlay.

## Implementation

| Piece | File |
|-------|------|
| Notification model (thread-safe, renderer-agnostic) | `src/server/notifications.hpp` |
| Toast drawing | `src/imgui/imgui_notifications.hpp` |
| The overlay | `src/imgui/imgui_server_overlay.hpp` |
| Host / process facts | `src/server/machine_info.hpp` |
| Wiring, `--overlay_hotkey` | `app/wish_cli/host_renderer.*`, `app/wish_cli/server/wish_server_app.cpp` |

The render loop calls `renderer::render_server_overlay()` after every
session has rendered, so the overlay and the toasts are drawn last.
`host_renderer` keeps the overlay in front of the client windows (but not
in front of its own pop-ups), and asks for continuous frames while it is
open or a toast is showing.
