# Settings and the settings tool

Apps keep their settings in the [persistent stores](persistent-store.md). To
let users change them from one place, an app **describes** each setting:
label, description, type, category, icon, and so on. The `bdg/desktop/conf`
tool, **Settings**, builds a macOS System Settings-like editor from those
descriptions: a sidebar of categories and a panel of grouped settings, each
with an editor that fits its type.

```sh
wish client --run conf --username alice    # or Start > Settings on wish desktop
```

## The model

A setting is **one field of a store entry**: `bdg.desktop.example.color` is
the field `color` of the entry `bdg.desktop.example`. The text before the
last dot is the entry, and the rest is the field (letters, digits, `_`,
`-`). Prefix entries with your tool's qualified name, as for any store
entry.

The description lives in the **same store** as the value, in the reserved
entry `<entry>#schema`. Categories go in `wish.category.<id>#schema`. So:

- Settings tools can show and edit the settings of apps that aren't running.
- A client can only describe settings in its own user store.
- Describing a setting doesn't change its value. Apps read it with
  **read setting** (below), or as usual (`user_store_get("bdg.desktop.example")`,
  then the field).

## Metadata

| Key | Meaning |
|-----|---------|
| `name` | `<entry>.<field>`, required. |
| `type` | `bool` (a switch), `int`, `float` (a slider when `min` and `max` are set, else a number box), `string`, `path` (a text box), `enum` (a drop-down of `options`) or `color` (`"#RRGGBB"` / `"#RRGGBBAA"`). Default `string`. |
| `label` | Shown name; the field when empty. |
| `description` | One or two sentences under the label. |
| `category` | Sidebar category id. Empty means `general`. |
| `section` | Title of the card the setting is grouped in, inside its category. |
| `default` | Value shown until one is stored; also the "reset" target. Must suit the type. |
| `min`, `max`, `step` | `int` and `float` ranges. |
| `options` | `enum` choices: values, or `{value, label}` pairs. |
| `icon` | Row icon, e.g. `res/icons/tune.png`. |
| `order` | Sort key inside the section (then the label). |
| `restart` | `true` to note that a change takes effect after a restart. |

Categories have `id`, `label`, `description`, `icon`, `color` (the
`"#RRGGBB"` badge behind the icon) and `order`. These ids are built in:

| Id | Shown as |
|----|----------|
| `general` | General |
| `appearance` | Appearance |
| `desktop` | Desktop |
| `server` | Server |

Your own categories come after the built-in ones, sorted by `order`, and a
setting naming an undeclared category gets a plain one. A category shows
only when it has settings (General always shows).

## Describing settings

Calling a describe function again replaces the earlier description, so apps
can simply describe their settings at every start. They fail like
`user_store_*` for an anonymous session, and reject invalid metadata.

**C++** (`wish::client`, `wish::standalone`, `wish_app_host`):

```cpp
wish::settings::setting_info s;
s.name = "bdg.desktop.example.color";
s.label = "Color";
s.category = "example";
s.type = wish::settings::setting_type::color;
s.default_value = std::string{"#2F7BF5"};
client.describe_setting(s).get();
client.describe_settings_category({"example", "Example", "My app's settings", "res/icons/palette.png", "#AF52DE", 10}).get();
```

Server code describes settings directly in a store with
`wish::settings::describe(store, info)` and `describe_category(store, info)`
(`src/context/settings_store.hpp`).

**C ABI** (`wish_client_c.h`). The metadata is a JSON object with the keys
above:

```c
wish_settings_describe(client, "{\"name\":\"bdg.desktop.example.color\",\"type\":\"color\",\"default\":\"#2F7BF5\"}");
wish_settings_describe_category(client, "{\"id\":\"example\",\"label\":\"Example\",\"color\":\"#AF52DE\"}");
```

**Python:**

```python
client.describe_settings_category("example", label="Example", icon="res/icons/palette.png", color="#AF52DE")
client.describe_setting("bdg.desktop.example.mood", type="enum", label="Mood", category="example",
                        options=[("happy", "Happy"), ("calm", "Calm")], default="happy")
```

**The other bindings** follow the same shape:

| Binding | Types | Methods |
|---------|-------|---------|
| C++ binding (`bindings/cpp`) | `setting_info`, `settings_category` | `client::describe_setting()`, `describe_settings_category()` |
| C# | `SettingInfo`, `SettingsCategory` | `Client.DescribeSetting()`, `DescribeSettingsCategory()` |
| Rust | `SettingInfo`, `SettingsCategory` | `describe_setting()`, `describe_settings_category()` |
| Go | `SettingInfo`, `SettingsCategory` | `DescribeSetting()`, `DescribeSettingsCategory()` |
| Java (Android) | `SettingInfo`, `SettingsCategory` (builders) | `describeSetting()`, `describeSettingsCategory()` |

## Reading settings

**Read setting** takes the same metadata and returns the current value: the
stored one when it suits the setting (type, range, option, color format;
an int is fine for a float setting), else the default. An anonymous
session, a missing entry or an invalid stored value all give the default,
so an app can read its settings unconditionally. Only invalid metadata is
an error.

| API | Call | Returns |
|-----|------|---------|
| C ABI | `wish_settings_read(client, setting_json, &out_json)` | JSON text (`true`, `42`, `0.5`, `"#2F7BF5"`, `null`); free with `bison_free_string()` |
| C++ binding | `client::read_setting(info)` | `setting_value` (ints as `int32_t`, floats as `double`) |
| Python | `client.read_setting(name, type=..., default=..., ...)` (same keywords as `describe_setting`) | `bool` / `int` / `float` / `str` |
| C# | `Client.ReadSetting(info)` | `object?`: `bool`, `int`, `double` or `string` |
| Rust | `client.read_setting(&info)` | `SettingValue` |
| Go | `client.ReadSetting(info)` | `any`: `bool`, `float64` or `string` |
| Java (Android) | `client.readSetting(info)` | `Boolean`, `Integer`, `Double` or `String` |
| C++ (`wish_app_host`) | `read_setting(info)`, `read_bool_setting()` ... | `settings::setting_value` |

A setting's metadata can live in one place and serve both calls (see the
demo's `SETTINGS` table):

```python
SIZE = dict(type="int", label="Text size", min=10, max=40, default=18)
client.describe_setting("bdg.desktop.example.size", **SIZE)
size = client.read_setting("bdg.desktop.example.size", **SIZE)
```

`bindings/python/examples/desktop/settings_demo.py` is a complete example.
It declares an "Example" category with one setting of each type, and shows a
banner that follows them live.

## Settings of server-side modules

Modules built into the server (`modules/<org>/<collection>/<name>`) don't
describe their settings in each store. They **register** them once, at
startup, so Settings lists them for every identity, even before the tool
ever ran:

- `settings::register_setting(info)` and `register_category(info)`
  (`src/context/settings_store.hpp`) keep a process-wide list.
- `settings::collect(user_store, true)` adds them to what the user store
  describes. A registered description wins over a stored one with the same
  name.

The bdg modules follow one pattern:

- **Definitions.** A header-only `<name>_settings.hpp` defines the settings
  with the builders in `modules/bdg/common/tool_settings.hpp`. The server
  form and the client runner both include it.
- **Registration.** The module's `register_<name>()` calls
  `common::register_tool_settings(<name>_settings::all())`. Each module has
  its own category: its id is the module name (`bdg.desktop.top`) and its
  label is the tool name.
- **Reading in the server form.** `tool_form::setting_bool()`,
  `setting_int()` and `setting_text()` return the stored value, else the
  default.
- **Remembering a toggle.** `tool_form::save_setting()` stores the new
  value when the user changes a toggle the tool remembers, such as a
  Follow checkbox or a search option.
- **Reading in the client runner.** `wish_app_host::read_bool_setting()`,
  `read_int_setting()` and the like, plus `write_setting()` to store a
  value.

All of these give the default for an anonymous session.

| Module | Settings |
|--------|----------|
| bc | mode, angle unit, digit grouping, show history |
| top | refresh interval, graph history |
| tail | initial lines, scroll to new lines, check interval |
| nano | match case, whole word, search mode |
| mc | show hidden files, keep folders on top |
| zip | compression level |
| docker, kubectl | follow logs, log lines, stats interval |
| curl | follow redirects, request timeout, history size |
| git | commits in history |
| sq | row limit |
| pip | upgrade, user install, pre-releases |
| helm | create namespace, wait |

## Server settings and admins

The server store can't be reached over RMI. The Settings tool is a server
form, so it reads and writes the server store in-process, but only for
sessions whose identity is a **settings admin**:

- `wish server --settings_admins=alice,bob`, or
  `server::set_settings_admins()`, lists them. Nobody is an admin by default.
- `wish standalone` is always admin, since it has one local user.
- Everyone else sees server settings read-only, with a lock icon.

Identities come from the auth module. `local_auth_module` trusts the
client's claimed user name, so only list admins on trusted, local
deployments. The flag can't be saved as a default.

`wish server` and `wish standalone` describe their most common saveable flags
in the server store at every start, as `wish.cli.server.*` and
`wish.cli.standalone.*`. They are the [saved defaults](cli.md#saved-flag-defaults)
`--save_defaults` writes:

- **theme** and **text size**, under Appearance;
- **language**, under General;
- **renderer**, window title and size, and ports, under Server.

They take effect at the next start. `wish desktop` describes its own theme
(`wish.desktop.theme`) under Desktop.

The [server overlay](server-overlay.md) (Shift+F1 on the server's
screen) has a Settings page too: the server's local operator edits the
server store and any user's store there, admin or not.

## The Settings tool

- **Sidebar:** a search box, the signed-in identity (Administrator or User
  settings), and the categories with their colored badges.
- **Panel:**
  - a header card for the selected category;
  - one card per section;
  - per row: the label, the description, and notes such as "Server" or
    "Takes effect after a restart";
  - the editor, and a reset button once the value differs from its default.
- **Search:** typing lists every matching setting (label, description or
  name), grouped by category.
- **Saving:**
  - Each change is validated (type, range, option, color format) and saved
    right away.
  - An invalid value shows an error under the row and isn't saved.
  - The tool re-reads the stores every two seconds, so settings that other
    apps declare or change appear live.
