// MIT License © 2025 Binary Dice Games
/**
 * @file config_flags.hpp
 * @brief Persisted wish CLI flag defaults, kept in the server store.
 *
 * `wish server` and `wish standalone` resolve each of their configurable
 * flags (renderer, font size, language, theme, port, ...) in this order:
 *
 *   1. the command line;
 *   2. the `WISH_<FLAG>` environment variable (see env_flags.hpp);
 *   3. the mode's entry in the server store (`<store_dir>/server_store.bison`,
 *      entry `wish.cli.server` or `wish.cli.standalone`);
 *   4. the default hardcoded in the flag's `DEFINE_*`.
 *
 * `--save_defaults` writes the flags given on the command line into the
 * entry, and `--clear_defaults` erases it. See docs/cli.md.
 */
#pragma once

#include <context/persistent_store.hpp>

#include <string>
#include <vector>

namespace bdg::wish {

/// @brief Server store entry holding `wish server`'s saved flag defaults.
inline constexpr const char* kServerConfigEntry = "wish.cli.server";
/// @brief Server store entry holding `wish standalone`'s saved flag defaults.
inline constexpr const char* kStandaloneConfigEntry = "wish.cli.standalone";

/**
 * @brief Applies the flag values saved in @p store entry @p entry_name as
 *        the defaults of the flags in @p flag_names.
 *
 * A saved value is applied only to a flag that was neither given on the
 * command line nor set through its `WISH_<FLAG>` environment variable. It
 * is applied with `gflags::SET_FLAGS_DEFAULT`, so the flag still counts as
 * "not given explicitly". A saved value gflags can't parse (e.g. `abc` for
 * an int flag) is skipped with a warning on stderr. Unregistered flag names
 * are ignored.
 *
 * @note Call after `gflags::ParseCommandLineFlags()`.
 * @return Names of the flags whose value now comes from the store.
 */
std::vector<std::string> apply_config_store_defaults(
    const persistent_store& store,
    const std::string& entry_name,
    const std::vector<std::string>& flag_names);

/**
 * @brief Saves every flag in @p flag_names that was given on the command
 *        line into @p store entry @p entry_name.
 *
 * Values are merged over the entry's existing ones, so flags not given this
 * time keep their saved value. `bool` and `int32` flags are stored as
 * `bool`/`int32_t` fields, all others as `std::string`; each field is named
 * after its flag. Nothing is written when no listed flag was given.
 *
 * @note Call after `gflags::ParseCommandLineFlags()`.
 * @return Names of the saved flags.
 * @throws std::runtime_error if the store file cannot be written.
 */
std::vector<std::string> save_config_store_defaults(
    persistent_store& store,
    const std::string& entry_name,
    const std::vector<std::string>& flag_names);

/**
 * @brief CLI helper: opens the server store under @p store_dir, handles
 *        `--clear_defaults` and `--save_defaults`, then applies the saved
 *        defaults (see `apply_config_store_defaults()`).
 *
 * The store is closed again before returning, so the server opens its own
 * instance afterwards. Never throws: a store that can't be written prints a
 * warning prefixed with @p log_prefix and the run continues.
 *
 * @param store_dir    `--store_dir`; empty selects `default_store_dir()`.
 * @param entry_name   `kServerConfigEntry` or `kStandaloneConfigEntry`.
 * @param flag_names   Flags that may be saved and loaded.
 * @param log_prefix   Message prefix, e.g. `"[wish server]"`.
 */
void load_cli_config(
    const std::string& store_dir,
    const std::string& entry_name,
    const std::vector<std::string>& flag_names,
    const std::string& log_prefix);

} // namespace bdg::wish
