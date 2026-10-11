// MIT License © 2025 Binary Dice Games
/**
 * @file config_flags.cpp
 * @brief Persisted wish CLI flag defaults, kept in the server store.
 */
#include "app/wish_cli/config_flags.hpp"

#include "app/wish_cli/env_flags.hpp"

#include <gflags/gflags.h>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
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

} // namespace

// ── Apply / save ─────────────────────────────────────────────────────────────

std::vector<std::string> apply_config_store_defaults(
    const persistent_store& store,
    const std::string& entry_name,
    const std::vector<std::string>& flag_names) {
  std::vector<std::string> applied;
  auto entry = store.get(entry_name);
  if (!entry)
    return applied;

  for (const auto& name : flag_names) {
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
    const std::vector<std::string>& flag_names) {
  std::vector<std::string> saved;
  bison::dynamic entry = store.get(entry_name).value_or(bison::dynamic{});

  for (const auto& name : flag_names) {
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

// ── CLI helper ───────────────────────────────────────────────────────────────

void load_cli_config(
    const std::string& store_dir,
    const std::string& entry_name,
    const std::vector<std::string>& flag_names,
    const std::string& log_prefix) {
  store_registry stores{store_dir.empty() ? default_store_dir() : std::filesystem::path{store_dir}};
  auto store = stores.server_store();

  try {
    if (FLAGS_clear_defaults && store->erase(entry_name))
      std::cout << log_prefix << " cleared saved defaults in " << store->file().string() << std::endl;

    if (FLAGS_save_defaults) {
      auto saved = save_config_store_defaults(*store, entry_name, flag_names);
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

  apply_config_store_defaults(*store, entry_name, flag_names);
}

} // namespace bdg::wish
