// MIT License © 2025 Binary Dice Games
/**
 * @file config_flags.cpp
 * @brief Persisted wish CLI flag defaults, kept in the server store.
 */
#include "app/wish_cli/config_flags.hpp"

#include "app/wish_cli/env_flags.hpp"

#include <context/settings_store.hpp>

#include <gflags/gflags.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <string>

DEFINE_bool(
    save_defaults,
    false,
    "Save the flags given on this command line to the server store as the defaults of "
    "later runs (command line > WISH_<FLAG> env var > saved default > built-in default)");
DEFINE_bool(clear_defaults, false, "Erase the flag defaults saved with --save_defaults");

namespace bdg::wish {

namespace {

bool given_on_command_line(const gflags::CommandLineFlagInfo& info) {
  // apply_env_flag_defaults() and apply_config_store_defaults() only use
  // SET_FLAGS_DEFAULT, which leaves is_default set; only the command line
  // clears it.
  return !info.is_default;
}

bool given_in_env(const std::string& flag_name) {
  return std::getenv(env_flag_var_name(flag_name).c_str()) != nullptr;
}

/// True for gflags' own flags (--help, --flagfile, --fromenv, ...), defined
/// in gflags.cc, gflags_reporting.cc and gflags_completions.cc.
bool is_gflags_builtin(const gflags::CommandLineFlagInfo& info) {
  return std::filesystem::path{info.filename}.filename().string().rfind("gflags", 0) == 0;
}

/// True for flags holding a secret: `*password` and inline `*_pem` flags.
bool is_secret(const std::string& name) {
  auto ends_with = [&](const std::string& suffix) {
    return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  return ends_with("password") || ends_with("_pem");
}

} // namespace

std::vector<std::string> config_flag_names(const std::vector<std::string>& excluded_flags) {
  // Never saved in any mode: the flags that drive the saved defaults.
  static const std::set<std::string> kAlwaysExcluded = {"save_defaults", "clear_defaults", "store_dir"};

  std::vector<gflags::CommandLineFlagInfo> flags;
  gflags::GetAllFlags(&flags);
  const std::set<std::string> excluded(excluded_flags.begin(), excluded_flags.end());

  std::vector<std::string> names;
  for (const auto& info : flags) {
    if (is_gflags_builtin(info) || is_secret(info.name) || kAlwaysExcluded.count(info.name) != 0 ||
        excluded.count(info.name) != 0)
      continue;
    names.push_back(info.name);
  }
  return names;
}

// ── Apply / save ─────────────────────────────────────────────────────────────

std::vector<std::string> apply_config_store_defaults(
    const persistent_store& store,
    const std::string& entry_name,
    const std::vector<std::string>& excluded_flags) {
  std::vector<std::string> applied;
  auto entry = store.get(entry_name);
  if (!entry)
    return applied;

  for (const auto& name : config_flag_names(excluded_flags)) {
    gflags::CommandLineFlagInfo info;
    if (!gflags::GetCommandLineFlagInfo(name.c_str(), &info))
      continue;
    if (given_on_command_line(info) || given_in_env(name))
      continue;
    const auto* saved = entry->findField(bison::key_t{name});
    if (saved == nullptr)
      continue;

    std::string value;
    try {
      value = saved->get_as<std::string>();
    } catch (const std::exception&) {
      std::cerr << "warning: ignoring saved default for --" << name << ": unsupported value type\n";
      continue;
    }
    if (gflags::SetCommandLineOptionWithMode(name.c_str(), value.c_str(), gflags::SET_FLAGS_DEFAULT).empty()) {
      std::cerr << "warning: ignoring invalid saved default --" << name << "=" << value << "\n";
      continue;
    }
    applied.push_back(name);
  }
  return applied;
}

std::vector<std::string> save_config_store_defaults(
    persistent_store& store,
    const std::string& entry_name,
    const std::vector<std::string>& excluded_flags) {
  std::vector<std::string> saved;
  bison::dynamic entry = store.get(entry_name).value_or(bison::dynamic{});

  for (const auto& name : config_flag_names(excluded_flags)) {
    gflags::CommandLineFlagInfo info;
    if (!gflags::GetCommandLineFlagInfo(name.c_str(), &info) || !given_on_command_line(info))
      continue;

    const bison::key_t key{name};
    if (info.type == "bool")
      entry[key] = info.current_value == "true";
    else if (info.type == "int32")
      entry[key] = static_cast<int32_t>(std::stol(info.current_value));
    else
      entry[key] = info.current_value;
    saved.push_back(name);
  }

  if (!saved.empty())
    store.set(entry_name, entry);
  return saved;
}

// ── Settings metadata ────────────────────────────────────────────────────────

namespace {

/// The flag's built-in default as a setting value of @p type.
settings::setting_value flag_default(const gflags::CommandLineFlagInfo& info, settings::setting_type type) {
  try {
    switch (type) {
      case settings::setting_type::boolean:
        return info.default_value == "true";
      case settings::setting_type::integer:
        return static_cast<int32_t>(std::stol(info.default_value));
      case settings::setting_type::real:
        return std::stof(info.default_value);
      default:
        return info.default_value;
    }
  } catch (...) {
    return {};
  }
}

} // namespace

void describe_config_flags(
    persistent_store& store,
    const std::string& entry_name,
    const std::vector<std::string>& excluded_flags,
    const std::string& section) {
  using settings::setting_type;
  struct known {
    const char* name;
    const char* label;
    const char* description;
    const char* category;
    setting_type type;
    std::vector<settings::setting_option> options;
    std::optional<float> min, max;
    int32_t order;
  };
  const std::vector<known> kKnown = {
      {"theme",
       "Theme",
       "Theme every new session starts with.",
       "appearance",
       setting_type::choice,
       {{"wish", "Wish"}, {"light", "Light"}, {"dark", "Dark"}, {"classic", "Classic"}},
       {},
       {},
       0},
      {"font_size", "Text size", "Font size in pixels.", "appearance", setting_type::integer, {}, 8.0f, 48.0f, 1},
      {"lang",
       "Language",
       "Language of the built-in dialogs and tools.",
       "general",
       setting_type::choice,
       {{"en", "English"},
        {"es", "Espa\xC3\xB1ol"},
        {"fr",
         "Fran\xC3\xA7"
         "ais"}},
       {},
       {},
       0},
      {"renderer",
       "Renderer",
       "Where the server draws its window.",
       "server",
       setting_type::choice,
       {{"web", "Web browser"}, {"sdl3", "Native window (SDL3)"}, {"none", "None"}},
       {},
       {},
       0},
      {"title", "Window title", "Title of the server's window.", "server", setting_type::text, {}, {}, {}, 1},
      {"width",
       "Window width",
       "Initial window width in pixels.",
       "server",
       setting_type::integer,
       {},
       320.0f,
       7680.0f,
       2},
      {"height",
       "Window height",
       "Initial window height in pixels.",
       "server",
       setting_type::integer,
       {},
       240.0f,
       4320.0f,
       3},
      {"web_port",
       "Web port",
       "HTTP port of the web renderer.",
       "server",
       setting_type::integer,
       {},
       1.0f,
       65535.0f,
       4},
      {"port",
       "Port",
       "Port clients connect to (tcp/tls transports).",
       "server",
       setting_type::integer,
       {},
       1.0f,
       65535.0f,
       5},
  };
  const auto names = config_flag_names(excluded_flags);
  const std::set<std::string> saveable(names.begin(), names.end());
  for (const auto& k : kKnown) {
    gflags::CommandLineFlagInfo info;
    if (!saveable.count(k.name) || !gflags::GetCommandLineFlagInfo(k.name, &info))
      continue;
    settings::setting_info s;
    s.name = entry_name + "." + k.name;
    s.label = k.label;
    s.description = k.description;
    s.category = k.category;
    s.section = section;
    s.type = k.type;
    s.options = k.options;
    // "none" is a renderer only standalone takes.
    if (std::string(k.name) == "renderer" && entry_name != kStandaloneConfigEntry)
      s.options.pop_back();
    s.min = k.min;
    s.max = k.max;
    s.order = k.order;
    s.restart = true;
    s.default_value = flag_default(info, k.type);
    // An empty or unknown built-in default (e.g. --lang "") has no option.
    if (std::holds_alternative<std::string>(s.default_value) && !s.options.empty() &&
        std::none_of(s.options.begin(), s.options.end(), [&](const settings::setting_option& o) {
          return o.value == std::get<std::string>(s.default_value);
        }))
      s.default_value = s.options.front().value;
    try {
      settings::describe(store, s);
    } catch (const std::exception&) {
      // Metadata is a convenience; never fail a start over it.
    }
  }
}

// ── CLI helper ───────────────────────────────────────────────────────────────

void load_cli_config(
    const std::string& store_dir,
    const std::string& entry_name,
    const std::vector<std::string>& excluded_flags,
    const std::string& log_prefix) {
  store_registry stores{store_dir.empty() ? default_store_dir() : std::filesystem::path{store_dir}};
  auto store = stores.server_store();

  try {
    if (FLAGS_clear_defaults && store->erase(entry_name))
      std::cout << log_prefix << " cleared saved defaults in " << store->file().string() << std::endl;

    if (FLAGS_save_defaults) {
      auto saved = save_config_store_defaults(*store, entry_name, excluded_flags);
      if (saved.empty()) {
        std::cout << log_prefix << " --save_defaults: no saveable flag given on the command line" << std::endl;
      } else {
        std::cout << log_prefix << " saved defaults to " << store->file().string() << ":";
        for (const auto& name : saved)
          std::cout << " --" << name;
        std::cout << std::endl;
      }
    }
  } catch (const std::exception& ex) {
    std::cerr << log_prefix << " warning: could not update saved defaults: " << ex.what() << "\n";
  }

  // Before applying: that replaces the flags' built-in defaults.
  describe_config_flags(
      *store, entry_name, excluded_flags, entry_name == kStandaloneConfigEntry ? "wish standalone" : "wish server");
  apply_config_store_defaults(*store, entry_name, excluded_flags);
}

} // namespace bdg::wish
