// MIT License © 2026 Binary Dice Games
/**
 * @file desktop_config.cpp
 * @brief Loading and parsing the `wish desktop` configuration file.
 */
#include "app/wish_cli/desktop/desktop_config.hpp"

#include "src/client/app_registry.hpp"
#include "src/ui/yaml_json.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace bdg::wish::desktop {

using json = nlohmann::ordered_json;

namespace {

// ── Helpers ──────────────────────────────────────────────────────────────────

[[noreturn]] void fail(const std::string& path, const std::string& what) {
  throw config_error(path + ": " + what);
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
  if (from.empty())
    return s;
  for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size()))
    s.replace(pos, from.size(), to);
  return s;
}

/// Substring placeholders ({config_file}, {home}).
std::string expand(const std::string& s, const placeholders& ph) {
  return replace_all(replace_all(s, "{config_file}", ph.config_file), "{home}", ph.home);
}

/// A scalar as text (YAML reads `1`/`true` as numbers/booleans).
std::string scalar_text(const json& v, const std::string& path) {
  if (v.is_string())
    return v.get<std::string>();
  if (v.is_boolean())
    return v.get<bool>() ? "true" : "false";
  if (v.is_number_integer())
    return std::to_string(v.get<long long>());
  if (v.is_number())
    return v.dump();
  fail(path, "expected a text value");
}

std::string optional_text(const json& obj, const char* key, const std::string& path) {
  auto it = obj.find(key);
  if (it == obj.end() || it->is_null())
    return {};
  return scalar_text(*it, path + "." + key);
}

bool optional_bool(const json& obj, const char* key, const std::string& path) {
  auto it = obj.find(key);
  if (it == obj.end() || it->is_null())
    return false;
  if (!it->is_boolean())
    fail(path + "." + key, "expected true or false");
  return it->get<bool>();
}

/// "Python apps" -> "python-apps".
std::string slug(const std::string& label) {
  std::string out;
  bool dash = false;
  for (unsigned char c : label) {
    if (std::isalnum(c)) {
      out += char(std::tolower(c));
      dash = false;
    } else if (!out.empty() && !dash) {
      out += '-';
      dash = true;
    }
  }
  while (!out.empty() && out.back() == '-')
    out.pop_back();
  return out;
}

std::string capitalize(std::string s) {
  if (!s.empty())
    s[0] = char(std::toupper(static_cast<unsigned char>(s[0])));
  return s;
}

/// The `--run=<name>` (or `--run <name>`) argument of a command, if any.
std::string run_target(const std::vector<std::string>& argv) {
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i].rfind("--run=", 0) == 0)
      return argv[i].substr(6);
    if (argv[i] == "--run" && i + 1 < argv.size())
      return argv[i + 1];
  }
  return {};
}

// ── Parser ───────────────────────────────────────────────────────────────────

class parser {
 public:
  explicit parser(const placeholders& ph) : ph_(ph) {}

  std::vector<menu_entry> entries(const json& list, const std::string& path, const std::string& parent_id) {
    if (!list.is_array())
      fail(path, "expected a list of entries");
    std::vector<menu_entry> out;
    for (size_t i = 0; i < list.size(); ++i) {
      std::string p = path + "[" + std::to_string(i) + "]";
      const json& item = list[i];
      if (!item.is_object())
        fail(p, "expected an entry (a map)");
      if (item.contains("apps")) {
        auto apps = expand_apps(item, p, parent_id);
        out.insert(out.end(), std::make_move_iterator(apps.begin()), std::make_move_iterator(apps.end()));
      } else {
        out.push_back(entry(item, p, parent_id));
      }
    }
    return out;
  }

  /// Ids are unique across the whole configuration.
  std::string claim_id(std::string id, const std::string& path, bool explicit_id) {
    if (id.empty())
      id = "entry";
    if (used_ids_.insert(id).second)
      return id;
    if (explicit_id)
      fail(path + ".id", "duplicate id '" + id + "'");
    for (int n = 2;; ++n) {
      std::string candidate = id + "-" + std::to_string(n);
      if (used_ids_.insert(candidate).second)
        return candidate;
    }
  }

 private:
  menu_entry entry(const json& item, const std::string& path, const std::string& parent_id) {
    menu_entry e;
    e.label = optional_text(item, "label", path);
    e.icon = optional_text(item, "icon", path);
    e.tooltip = optional_text(item, "tooltip", path);
    e.app = optional_text(item, "app", path);
    e.pinned = optional_bool(item, "pinned", path);
    e.on_desktop = optional_bool(item, "desktop", path);

    int kinds = int(item.contains("run")) + int(item.contains("action")) + int(item.contains("items")) +
        int(item.contains("separator"));
    if (kinds != 1)
      fail(path, "an entry needs exactly one of run, action, items, separator or apps");

    std::string explicit_id = optional_text(item, "id", path);
    std::string base = explicit_id.empty() ? (parent_id.empty() ? "" : parent_id + "/") + slug(e.label) : explicit_id;

    if (item.contains("separator")) {
      e.kind = entry_kind::separator;
      e.id = claim_id(
          explicit_id.empty() ? (parent_id.empty() ? "" : parent_id + "/") + "separator" : explicit_id,
          path,
          !explicit_id.empty());
      return e;
    }
    if (e.label.empty())
      fail(path + ".label", "missing");
    e.id = claim_id(base, path, !explicit_id.empty());

    if (item.contains("items")) {
      e.kind = entry_kind::folder;
      e.children = entries(item["items"], path + ".items", e.id);
    } else if (item.contains("action")) {
      e.kind = entry_kind::action;
      e.action = scalar_text(item["action"], path + ".action");
      const auto& known = known_actions();
      if (std::find(known.begin(), known.end(), e.action) == known.end())
        fail(path + ".action", "unknown action '" + e.action + "'");
      if (auto it = item.find("args"); it != item.end() && !it->is_null()) {
        if (!it->is_object())
          fail(path + ".args", "expected a map");
        e.args = *it;
      }
    } else {
      e.kind = entry_kind::run;
      e.argv = command(item["run"], path + ".run");
      e.cwd = expand(optional_text(item, "cwd", path), ph_);
      if (auto it = item.find("env"); it != item.end() && !it->is_null()) {
        if (!it->is_object())
          fail(path + ".env", "expected a map of NAME: value");
        for (const auto& [k, v] : it->items())
          e.env.emplace_back(k, expand(scalar_text(v, path + ".env." + k), ph_));
      }
      if (e.app.empty())
        e.app = run_target(e.argv);
    }
    return e;
  }

  std::vector<std::string> command(const json& run, const std::string& path) {
    std::vector<std::string> raw;
    if (run.is_string()) {
      raw = split_command(run.get<std::string>());
    } else if (run.is_array()) {
      for (size_t i = 0; i < run.size(); ++i)
        raw.push_back(scalar_text(run[i], path + "[" + std::to_string(i) + "]"));
    } else {
      fail(path, "expected a list or a string");
    }
    std::vector<std::string> argv;
    for (const auto& tok : raw) {
      if (tok == "{wish_client}")
        argv.insert(argv.end(), ph_.wish_client.begin(), ph_.wish_client.end());
      else
        argv.push_back(expand(tok, ph_));
    }
    if (argv.empty() || argv[0].empty())
      fail(path, "empty command");
    return argv;
  }

  /// `apps: all` -> a folder per collection; `apps: <collection>` -> that
  /// collection's apps, inline.
  std::vector<menu_entry> expand_apps(const json& item, const std::string& path, const std::string& parent_id) {
    std::string which = scalar_text(item["apps"], path + ".apps");
    std::map<std::string, std::vector<const app_info*>> by_collection;
    for (const auto& [name, info] : registered_apps())
      by_collection[info.collection].push_back(&info);
    for (auto& [c, list] : by_collection)
      std::sort(list.begin(), list.end(), [](const app_info* a, const app_info* b) { return a->name < b->name; });

    auto app_entry = [&](const app_info& a) {
      menu_entry e;
      e.kind = entry_kind::run;
      std::string q = qualified_app_name(a);
      e.id = claim_id(q, path, false);
      e.label = a.name;
      e.icon = a.icon;
      e.tooltip = a.description;
      e.app = q;
      e.argv = ph_.wish_client;
      e.argv.push_back("--run=" + q);
      return e;
    };

    std::vector<menu_entry> out;
    if (which == "all") {
      for (const auto& [collection, list] : by_collection) {
        menu_entry folder;
        folder.kind = entry_kind::folder;
        folder.label = collection.empty() ? std::string{"Apps"} : capitalize(collection);
        folder.icon = "res/icons/folder.png";
        folder.id = claim_id((parent_id.empty() ? "" : parent_id + "/") + slug(folder.label), path, false);
        for (const app_info* a : list)
          folder.children.push_back(app_entry(*a));
        out.push_back(std::move(folder));
      }
    } else {
      auto it = by_collection.find(which);
      if (it != by_collection.end())
        for (const app_info* a : it->second)
          out.push_back(app_entry(*a));
    }
    return out;
  }

  const placeholders& ph_;
  std::set<std::string> used_ids_;
};

menu_entry* find_entry(std::vector<menu_entry>& list, const std::string& id) {
  for (auto& e : list) {
    if (e.id == id)
      return &e;
    if (menu_entry* found = find_entry(e.children, id))
      return found;
  }
  return nullptr;
}

void apply_id_list(start_menu& m, const json& section, const char* key, bool menu_entry::*flag) {
  auto it = section.find(key);
  if (it == section.end() || it->is_null())
    return;
  if (!it->is_array())
    fail(std::string("start_menu.") + key, "expected a list of entry ids");
  for (size_t i = 0; i < it->size(); ++i) {
    std::string id = scalar_text((*it)[i], std::string("start_menu.") + key + "[" + std::to_string(i) + "]");
    // Unknown ids are ignored: the default lists name apps a build may not
    // include.
    menu_entry* e = find_entry(m.items, id);
    if (!e)
      e = find_entry(m.power, id);
    if (e)
      e->*flag = true;
  }
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────────────

std::vector<const menu_entry*> start_menu::all() const {
  std::vector<const menu_entry*> out;
  // Depth-first, items before power.
  std::function<void(const std::vector<menu_entry>&)> walk = [&](const std::vector<menu_entry>& list) {
    for (const auto& e : list) {
      out.push_back(&e);
      walk(e.children);
    }
  };
  walk(items);
  walk(power);
  return out;
}

const menu_entry* start_menu::find(const std::string& id) const {
  for (const menu_entry* e : all())
    if (e->id == id)
      return e;
  return nullptr;
}

const std::vector<std::string>& wallpaper_fits() {
  static const std::vector<std::string> kFits{"cover", "contain", "stretch", "center", "tile"};
  return kFits;
}

appearance parse_appearance(const json& section, const placeholders& ph) {
  if (!section.is_object())
    fail("appearance", "expected a map");
  appearance a;
  if (std::string w = expand(optional_text(section, "wallpaper", "appearance"), ph); !w.empty()) {
    if (w == "~" || w.rfind("~/", 0) == 0)
      w = ph.home + w.substr(1);
    std::filesystem::path p(w);
    if (p.is_relative() && !ph.config_file.empty())
      p = std::filesystem::path(ph.config_file).parent_path() / p;
    a.wallpaper = p.lexically_normal();
  }
  if (std::string fit = optional_text(section, "wallpaper_fit", "appearance"); !fit.empty()) {
    const auto& fits = wallpaper_fits();
    if (std::find(fits.begin(), fits.end(), fit) == fits.end())
      fail("appearance.wallpaper_fit", "expected cover, contain, stretch, center or tile");
    a.wallpaper_fit = fit;
  }
  a.background = optional_text(section, "background", "appearance");
  if (!a.background.empty()) {
    const std::string& c = a.background;
    bool ok = c[0] == '#' && (c.size() == 7 || c.size() == 9) &&
        std::all_of(c.begin() + 1, c.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; });
    if (!ok)
      fail("appearance.background", "expected a color like \"#1E3A5F\" (#RRGGBB or #RRGGBBAA)");
  }
  a.theme = optional_text(section, "theme", "appearance");
  if (auto it = section.find("desktop_icons"); it != section.end() && !it->is_null())
    a.desktop_icons = optional_bool(section, "desktop_icons", "appearance");
  return a;
}

taskbar_settings parse_taskbar(const json& section) {
  if (!section.is_object())
    fail("taskbar", "expected a map");
  taskbar_settings t;
  if (std::string f = optional_text(section, "time_format", "taskbar"); !f.empty())
    t.time_format = f;
  if (std::string f = optional_text(section, "date_format", "taskbar"); !f.empty())
    t.date_format = f;
  return t;
}

const std::vector<std::string>& known_actions() {
  static const std::vector<std::string> kActions{
      "quit", "shutdown_server", "set_theme", "toggle_theme", "reload_config", "launch"};
  return kActions;
}

start_menu parse_start_menu(const json& section, const placeholders& ph) {
  if (!section.is_object())
    fail("start_menu", "expected a map");
  parser p(ph);
  start_menu m;
  if (auto it = section.find("items"); it != section.end())
    m.items = p.entries(*it, "start_menu.items", "");
  if (auto it = section.find("power"); it != section.end())
    m.power = p.entries(*it, "start_menu.power", "power");
  apply_id_list(m, section, "pinned", &menu_entry::pinned);
  apply_id_list(m, section, "desktop", &menu_entry::on_desktop);
  return m;
}

const std::string& default_config_text() {
  static const std::string kText = R"yaml(# wish desktop configuration. Edit and save: the running desktop reloads it.
# Format: docs/desktop-config.md in the wish repository.
version: 1

# How the desktop looks.
appearance:
  wallpaper: ""            # image file (relative to this file, or ~/...); empty: built-in gradient
  wallpaper_fit: cover     # cover, contain, stretch, center or tile
  background: ""           # color, e.g. "#1E3A5F": replaces the gradient, fills around the wallpaper
  # theme: dark            # fixes the theme; leave out to keep the tray toggle's choice
  desktop_icons: true

# The taskbar clock (strftime formats).
taskbar:
  time_format: "%H:%M"
  date_format: "%d/%m/%Y"

# The Start menu: a tree of entries. An entry runs a command (run), calls a
# desktop action (action), holds other entries (items), or is a separator.
start_menu:
  pinned: [settings, bdg/desktop/nano, bdg/desktop/mc, bdg/desktop/top]   # taskbar pins, by entry id
  desktop: [bdg/desktop/nano, bdg/desktop/mc, bdg/desktop/top]  # desktop icons, by entry id
  items:
    - apps: all            # every app built into wish, a folder per collection
    - separator: true
    - label: Settings
      id: settings
      icon: res/icons/settings.png
      tooltip: Your settings, your apps' settings and the server's
      run: ["{wish_client}", "--run=bdg/desktop/conf"]
      app: bdg/desktop/conf
    - label: Edit desktop settings
      icon: res/icons/settings.png
      tooltip: Open this file in nano; saving it updates the desktop
      run: ["{wish_client}", "--run=bdg/desktop/nano", "--", "{config_file}"]
      app: bdg/desktop/nano
  power:                   # the power button at the bottom of the Start menu
    - label: Quit desktop
      icon: res/icons/logout.png
      action: quit
    - label: Shut down server
      icon: res/icons/power.png
      action: shutdown_server
)yaml";
  return kText;
}

desktop_config parse_config_text(const std::string& text, const placeholders& ph) {
  json doc = yaml_to_json(text);
  if (doc.is_null())
    doc = json::object();
  if (!doc.is_object())
    throw config_error("expected a map of sections (e.g. start_menu:)");
  desktop_config c;
  if (auto it = doc.find("appearance"); it != doc.end() && !it->is_null())
    c.look = parse_appearance(*it, ph);
  if (auto it = doc.find("taskbar"); it != doc.end() && !it->is_null())
    c.taskbar = parse_taskbar(*it);
  if (auto it = doc.find("start_menu"); it != doc.end() && !it->is_null())
    c.menu = parse_start_menu(*it, ph);
  else // No start_menu section: the default menu.
    c.menu = parse_start_menu(yaml_to_json(default_config_text())["start_menu"], ph);
  return c;
}

loaded_config load_config(const std::filesystem::path& file, const placeholders& ph) {
  loaded_config out;
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::binary);
    if (f << default_config_text())
      out.wrote_default = true;
    out.config = parse_config_text(default_config_text(), ph);
    if (!out.wrote_default)
      out.error = "cannot write " + file.string();
    return out;
  }
  try {
    std::ifstream f(file, std::ios::binary);
    if (!f)
      throw config_error("cannot read " + file.string());
    std::stringstream ss;
    ss << f.rdbuf();
    out.config = parse_config_text(ss.str(), ph);
  } catch (const std::exception& ex) {
    out.error = file.filename().string() + ": " + ex.what();
    out.config = parse_config_text(default_config_text(), ph);
  }
  return out;
}

std::vector<std::string> split_command(const std::string& command) {
  std::vector<std::string> out;
  std::string cur;
  bool have = false;
  char quote = 0;
  for (char c : command) {
    if (quote) {
      if (c == quote)
        quote = 0;
      else
        cur += c;
    } else if (c == '"' || c == '\'') {
      quote = c;
      have = true;
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      if (have || !cur.empty())
        out.push_back(cur);
      cur.clear();
      have = false;
    } else {
      cur += c;
    }
  }
  if (have || !cur.empty())
    out.push_back(cur);
  return out;
}

} // namespace bdg::wish::desktop
