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
 *
 * Every registered flag can be saved except the ones a mode excludes (see
 * `config_flag_names()`), so a new flag needs no registration here.
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
 * @brief Names of the registered flags that can be saved: every flag known
 *        to gflags except
 *        - @p excluded_flags (the mode's own exclusions);
 *        - `--save_defaults`, `--clear_defaults` and `--store_dir`;
 *        - gflags' built-in flags (`--help`, `--flagfile`, ...);
 *        - secrets: flags named `*password` or `*_pem`.
 * @return Names in gflags' order (by defining file, then name).
 */
std::vector<std::string> config_flag_names(const std::vector<std::string>& excluded_flags);

/**
 * @brief Applies the flag values saved in @p store entry @p entry_name as
 *        the defaults of the flags in `config_flag_names(excluded_flags)`.
 *
 * A saved value is applied only to a flag that was neither given on the
 * command line nor set through its `WISH_<FLAG>` environment variable. It
 * is applied with `gflags::SET_FLAGS_DEFAULT`, so the flag still counts as
 * "not given explicitly". A saved value gflags can't parse (e.g. `abc` for
 * an int flag) is skipped with a warning on stderr. Saved fields of excluded
 * or no longer registered flags are ignored.
 *
 * @note Call after `gflags::ParseCommandLineFlags()`.
 * @return Names of the flags whose value now comes from the store.
 */
std::vector<std::string> apply_config_store_defaults(
    const persistent_store& store,
    const std::string& entry_name,
    const std::vector<std::string>& excluded_flags);

/**
 * @brief Saves every flag in `config_flag_names(excluded_flags)` that was
 *        given on the command line into @p store entry @p entry_name.
 *
 * Values are merged over the entry's existing ones, so flags not given this
 * time keep their saved value. `bool` and `int32` flags are stored as
 * `bool`/`int32_t` fields, all others as `std::string`; each field is named
 * after its flag. Nothing is written when no saveable flag was given.
 *
 * @note Call after `gflags::ParseCommandLineFlags()`.
 * @return Names of the saved flags.
 * @throws std::runtime_error if the store file cannot be written.
 */
std::vector<std::string> save_config_store_defaults(
    persistent_store& store,
    const std::string& entry_name,
    const std::vector<std::string>& excluded_flags);

/**
 * @brief Records settings metadata (see docs/settings.md) for the saveable
 *        flags users most often change -- theme, font size, language,
 *        renderer, window size and title, ports -- in @p store's
 *        `<entry_name>#schema` entry, so the settings tool (bdg/desktop/conf)
 *        can edit the saved defaults. Defaults come from the flags' built-in
 *        defaults, so call it before apply_config_store_defaults().
 * @param section  Group title shown in the settings tool, e.g. "wish server".
 */
void describe_config_flags(
    persistent_store& store,
    const std::string& entry_name,
    const std::vector<std::string>& excluded_flags,
    const std::string& section);

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
 * @param excluded_flags  The mode's flags that are never saved or loaded.
 * @param log_prefix   Message prefix, e.g. `"[wish server]"`.
 *
 * Also records the flags' settings metadata (describe_config_flags()).
 */
void load_cli_config(
    const std::string& store_dir,
    const std::string& entry_name,
    const std::vector<std::string>& excluded_flags,
    const std::string& log_prefix);

} // namespace bdg::wish
