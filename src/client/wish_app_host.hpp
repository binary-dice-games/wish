// MIT License © 2025 Binary Dice Games
/**
 * @file wish_app_host.hpp
 * @brief Minimal surface an embedded app runner (bc/nano/
 *        top) needs from whatever is hosting it.
 */
#pragma once

#include <client/app_registry.hpp>
#include <client/resource_sync.hpp>
#include <context/settings_schema.hpp>
#include <server/notifications.hpp>

#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace bdg::wish {

/// @brief Progress callback for the chunked `upload_file`/`download_file`
///        overloads: invoked after each chunk with bytes transferred so far
///        and the total transfer size.
using transfer_progress_callback = std::function<void(std::uint64_t transferred, std::uint64_t total)>;

/**
 * @brief App-runner-facing interface implemented by both `wish_client_session`
 *        (transport-backed, `wish client`) and `wish_standalone_session`
 *        (in-process, `wish standalone`).
 *
 * `run_bc`/`run_nano`/`run_top` are written against
 * this interface instead of a concrete session type so the same app code
 * runs unmodified whether the session talks to a server over a transport or
 * hosts the server logic in-process.
 */
class wish_app_host {
 public:
  virtual ~wish_app_host() = default;

  /// @copydoc bdg::wish::client::instantiate
  virtual std::future<bison::rmi::proxy::dynamic>
  instantiate(bison::key_t ns, bison::key_t klass, bison::dynamic params = bison::dynamic{}) = 0;

  /// @copydoc bdg::wish::client::upload_file
  ///
  /// When @p on_progress is set, the transfer runs chunked and never blocks
  /// the calling thread for the whole transfer in one RMI round trip --
  /// @p on_progress is invoked after each chunk, so a caller running this
  /// from a background thread can post incremental UI updates without
  /// holding any lock for the full transfer duration. See
  /// `bdg::wish::standalone`'s and `bdg::wish::client`'s worker-thread/
  /// dispatch-lock contracts for why an event handler must never block on
  /// the unchunked path directly.
  ///
  /// @param on_progress Invoked after each chunk with bytes sent so far and
  ///                    the total (== `data.size()`). When left default
  ///                    (empty), the transfer is a single monolithic RMI
  ///                    call with no progress reporting.
  virtual std::future<void> upload_file(
      const std::string& name, const std::string& data, transfer_progress_callback on_progress = nullptr) = 0;

  /// @copydoc bdg::wish::client::download_file
  ///
  /// @param on_progress Invoked after each chunk with bytes received so far
  ///                    and the total file size. When left default (empty),
  ///                    the transfer is a single monolithic RMI call with no
  ///                    progress reporting.
  virtual std::future<std::string>
  download_file(const std::string& name, transfer_progress_callback on_progress = nullptr) = 0;

  // ── Private tool directory ──────────────────────────────────────────────
  //
  // Scratch files go in a temp directory inside the tool's private
  // directory, never at the sandbox root shared with other tools (see
  // file_service::app_private_dir()).

  /// @copydoc bdg::wish::client::create_temp_dir
  ///
  /// Not pure: a host without a file service (e.g. a test double) inherits
  /// this default, whose future throws `std::logic_error`.
  virtual std::future<std::string> create_temp_dir(const std::string& qualified_app) {
    (void)qualified_app;
    std::promise<std::string> p;
    p.set_exception(std::make_exception_ptr(std::logic_error("wish: temp dirs not supported by this app host")));
    return p.get_future();
  }

  // ── Resource packages ───────────────────────────────────────────────────

  /// @copydoc bdg::wish::client::sync_resource_package
  ///
  /// Not pure: a host without a file service (e.g. a test double) inherits
  /// this default, whose future throws `std::logic_error` (and which never
  /// calls @p on_done).
  [[nodiscard]] virtual std::future<bool> sync_resource_package(
      const std::string& name,
      const std::filesystem::path& local_zip,
      const std::string& dest_path = "",
      transfer_progress_callback on_progress = nullptr,
      resource_sync_callback on_done = nullptr) {
    (void)name, (void)local_zip, (void)dest_path, (void)on_progress, (void)on_done;
    return unavailable<bool>("resource packages");
  }

  /// @brief Registration info of the app this host is running, or `nullptr`
  ///        if it was not started from the app registry.
  const app_info* running_app() const {
    return running_app_;
  }

  /// @brief Records the app this host runs. Hosts call this before
  ///        `app_info::run`.
  void set_running_app(const app_info* info) {
    running_app_ = info;
  }

  /// @brief `create_temp_dir()` for the running app (see `running_app()`).
  /// @throws std::logic_error if no running app is set.
  std::future<std::string> create_app_temp_dir() {
    if (!running_app_)
      throw std::logic_error("wish: create_app_temp_dir() needs a running app");
    return create_temp_dir(qualified_app_name(*running_app_));
  }

  // ── Notifications ───────────────────────────────────────────────────────
  //
  // Toasts on the server's screen. Not pure: a host without them (e.g. a
  // test double) inherits these defaults, which report them as unavailable.

  /// @copydoc bdg::wish::client::has_notifications
  virtual bool has_notifications() const {
    return false;
  }

  /// @copydoc bdg::wish::client::notify
  virtual std::future<uint64_t> notify(const notification& n) {
    (void)n;
    return unavailable<uint64_t>("notifications");
  }

  /// @copydoc bdg::wish::client::dismiss_notification
  virtual std::future<bool> dismiss_notification(uint64_t id) {
    (void)id;
    return unavailable<bool>("notifications");
  }

  // ── User store ──────────────────────────────────────────────────────────
  //
  // The session's persistent, per-identity store of named bison objects
  // (see docs/persistent-store.md). Not pure: a host without one (e.g. a
  // test double) inherits these defaults, which report it as unavailable.

  /// @copydoc bdg::wish::client::has_user_store
  virtual bool has_user_store() const {
    return false;
  }

  /// @copydoc bdg::wish::client::user_store_get
  virtual std::future<std::optional<bison::dynamic>> user_store_get(const std::string& name) {
    (void)name;
    return unavailable_user_store<std::optional<bison::dynamic>>();
  }

  /// @copydoc bdg::wish::client::user_store_set
  virtual std::future<void> user_store_set(const std::string& name, bison::dynamic value) {
    (void)name;
    (void)value;
    return unavailable_user_store<void>();
  }

  /// @copydoc bdg::wish::client::user_store_erase
  virtual std::future<bool> user_store_erase(const std::string& name) {
    (void)name;
    return unavailable_user_store<bool>();
  }

  /// @copydoc bdg::wish::client::user_store_keys
  virtual std::future<std::vector<std::string>> user_store_keys() {
    return unavailable_user_store<std::vector<std::string>>();
  }

  // ── Settings metadata ─────────────────────────────────────────────────

  /// @copydoc bdg::wish::client::describe_setting
  /// The default implementation reads, merges and writes the schema entry
  /// through user_store_get()/user_store_set().
  virtual std::future<void> describe_setting(const settings::setting_info& info) {
    settings::check(info);
    return std::async(std::launch::async, [this, info]() {
      const std::string name = settings::schema_entry_name(settings::parse_name(info.name).entry);
      bison::dynamic schema = user_store_get(name).get().value_or(bison::dynamic{});
      settings::merge_setting(schema, info);
      user_store_set(name, std::move(schema)).get();
    });
  }

  /// @copydoc bdg::wish::client::describe_settings_category
  virtual std::future<void> describe_settings_category(const settings::category_info& info) {
    settings::check(info);
    return std::async(std::launch::async, [this, info]() {
      const std::string name = settings::category_entry_name(info.id);
      bison::dynamic schema = user_store_get(name).get().value_or(bison::dynamic{});
      settings::merge_category(schema, info);
      user_store_set(name, std::move(schema)).get();
    });
  }

  /**
   * @brief The current value of @p info in the user store: the stored one
   *        when it suits the setting (settings::validate()), else the default.
   *
   * Blocks on user_store_get(). Never throws: an anonymous session, a store
   * error or a missing/invalid value all give the default.
   */
  settings::setting_value read_setting(const settings::setting_info& info) {
    if (!has_user_store())
      return info.default_value;
    try {
      auto entry = user_store_get(settings::parse_name(info.name).entry).get();
      return settings::stored_or_default(entry ? &*entry : nullptr, info);
    } catch (const std::exception&) {
      return info.default_value;
    }
  }

  /// @brief read_setting() as a bool / int / float / string.
  bool read_bool_setting(const settings::setting_info& info) {
    auto v = read_setting(info);
    auto* b = std::get_if<bool>(&v);
    return b ? *b : false;
  }
  int32_t read_int_setting(const settings::setting_info& info) {
    auto v = read_setting(info);
    if (auto* f = std::get_if<float>(&v))
      return static_cast<int32_t>(*f);
    auto* i = std::get_if<int32_t>(&v);
    return i ? *i : 0;
  }
  float read_float_setting(const settings::setting_info& info) {
    auto v = read_setting(info);
    if (auto* i = std::get_if<int32_t>(&v))
      return static_cast<float>(*i);
    auto* f = std::get_if<float>(&v);
    return f ? *f : 0.0f;
  }
  std::string read_text_setting(const settings::setting_info& info) {
    auto v = read_setting(info);
    auto* s = std::get_if<std::string>(&v);
    return s ? *s : std::string{};
  }

  /**
   * @brief Validates @p value and stores it as @p info's field of its entry
   *        (read, modify, write; other fields are kept).
   *
   * Blocks on the user store. Never throws.
   * @return An error message, or empty on success. An anonymous session
   *         gives an error.
   */
  std::string write_setting(const settings::setting_info& info, const settings::setting_value& value) {
    if (!has_user_store())
      return "no user store";
    auto checked = settings::validate(info, value);
    if (auto* err = std::get_if<std::string>(&checked))
      return *err;
    try {
      const settings::setting_location loc = settings::parse_name(info.name);
      bison::dynamic entry = user_store_get(loc.entry).get().value_or(bison::dynamic{});
      settings::write_value(entry, loc.field, std::get<settings::setting_value>(checked));
      user_store_set(loc.entry, std::move(entry)).get();
    } catch (const std::exception& e) {
      return e.what();
    }
    return {};
  }

  /// @brief Store a proxy to keep the remote/local object alive for the session.
  virtual void keep_alive(bison::rmi::proxy::dynamic&& proxy) = 0;

  /// @brief Unblock the app's runner — call from a "closed" event handler.
  virtual void signal_done() = 0;

  /// @brief Set the process exit code the host reports once the app is done.
  ///
  /// For apps that are also command line tools (e.g. nymph's `render`), so a
  /// failure is visible to a calling script or agent, not only on stderr.
  /// Call before `signal_done()`. The default is 0. Hosts with no process
  /// exit code to set (the C ABI's `wish_run_app()`) ignore it.
  ///
  /// @param code  0 for success; non-zero for failure.
  virtual void set_exit_code(int /*code*/) {}

  /// @brief Positional arguments given after `--` on the command line.
  virtual const std::vector<std::string>& app_args() const = 0;

  /// @brief The UI language this session was started with (e.g. `"es"`, from
  ///        `--lang`); empty means the default (`en`).
  ///
  /// The host has already sent it to the server, so server-side forms are
  /// translated; client-side code can use it to pick its own strings.
  virtual std::string language() const {
    return {};
  }

  /// @brief Read one line of console (operator) input, blocking until a
  ///        line is available.
  ///
  /// Always safe to call regardless of transport: `wish_client_session`
  /// routes this through `bison::app::client_app::read_console_line()`,
  /// which knows how to read console input even when the active transport
  /// (e.g. `--transport=term`) also owns stdin for framed RMI traffic.
  /// App runners must use this instead of `std::cin` directly.
  ///
  /// @param line Output line, without the trailing newline.
  /// @return `false` once no more input is available (EOF).
  virtual bool read_console_line(std::string& line) = 0;

 private:
  const app_info* running_app_ = nullptr;

  template <typename T>
  static std::future<T> unavailable(const char* what) {
    std::promise<T> p;
    p.set_exception(
        std::make_exception_ptr(std::logic_error(std::string{"wish: "} + what + " not supported by this app host")));
    return p.get_future();
  }

  template <typename T>
  static std::future<T> unavailable_user_store() {
    return unavailable<T>("user store");
  }
};

} // namespace bdg::wish
