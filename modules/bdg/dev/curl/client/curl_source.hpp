// MIT License © 2026 Binary Dice Games
/// @file curl_source.hpp
/// @brief Client-side `curl` command orchestration for the curl module.
///
/// Owns the proxy, runs every `curl` invocation via
/// curl_process::run_curl_cli(), parses the response, applies
/// environment-variable substitution, and pushes structured snapshots to
/// the server-side CurlFrontend form via its update_* RMI methods. Also
/// reacts to the form's `*_requested` events (see server/curl.hpp).
///
/// Also owns the module's local persistent store (Collections,
/// Environments, History) -- see DESIGN.md "Persistence". This has no
/// docker/kubectl precedent: both of those reflect purely external, live
/// state with nothing to persist. The store is a small hand-rolled JSON
/// file (never `nlohmann::json` -- its include path isn't available to
/// module-client sources, docker's DESIGN.md "no JSON library" decision)
/// at a per-user config location on the machine running the client,
/// entirely separate from the session sandbox (`session::resource_dir`):
/// this is the client's own local app state, not a server-directed widget
/// file path.
#pragma once

#include "curl_process.hpp"
#include "curl_response_parser.hpp" // for kv_entry
#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish {
class wish_app_host;
}

namespace bdg::wish::curl {

/// @brief The full builder state of one request -- collected from the
/// server's `send_requested`/`save_request_requested` payload, or restored
/// from a stored History/Collections entry.
struct request_state {
  std::string method{"GET"};
  std::string url;
  std::vector<kv_entry> params;
  std::vector<kv_entry> headers;
  std::string body_mode{"none"}; // "none" / "raw" / "json" / "form"
  std::string body_text;
  std::vector<kv_entry> form_fields;
  std::string auth_mode{"none"}; // "none" / "basic" / "bearer"
  std::string auth_username;
  std::string auth_password;
  std::string auth_token;
  bool follow_redirects{true};
  std::string environment; // environment id, or "" for none
};

struct saved_request {
  std::string id;
  std::string collection;
  std::string name;
  request_state state;
};

struct environment {
  std::string id;
  std::string name;
  std::vector<kv_entry> vars;
};

struct history_entry {
  std::string id;
  int32_t status_code{0};
  bool ok{false};
  float time_ms{0.0f};
  std::string timestamp;
  request_state state; ///< the full builder state, so "Load" restores everything.
};

class curl_source {
 public:
  curl_source(std::shared_ptr<bison::rmi::proxy::dynamic> proxy, wish_app_host& host);

  /// @brief Loads the local store (if present) and pushes the initial
  /// Collections / Environments / History snapshots. Called once on
  /// startup, after every event handler is wired (docker's documented
  /// initial-load-race fix -- never from the form's on_init()).
  void refresh_all();

  // ── *_requested event reactions (see server/curl.hpp) ──────────────────
  void on_send_requested(const bison::dynamic& payload);
  void on_save_request_requested(const bison::dynamic& payload);
  void on_load_request_requested(const std::string& id);
  void on_delete_request_requested(const std::string& id);
  void on_duplicate_request_requested(const std::string& id);
  void on_load_history_requested(const std::string& id);
  void on_clear_history_requested();
  void on_new_environment_requested(const std::string& name);
  void on_delete_environment_requested(const std::string& id);
  void on_select_environment_requested(const std::string& id);
  void on_save_environment_vars_requested(const bison::dynamic& payload);

 private:
  // ── Persistence ──────────────────────────────────────────────────────
  void load_store();
  void save_store() const;

  // ── Pushing snapshots to the form ───────────────────────────────────
  void push_collections();
  void push_environments();
  void push_history();
  void push_request_builder(const request_state& s);

  /// @brief Runs `curl` for @p s, applying environment substitution and
  /// the sentinel-trailer response parse (see DESIGN.md "Command
  /// construction & response parsing"), pushes update_response, appends
  /// one History entry, and traces the invocation in the Console window.
  void send_request(const request_state& s);

  request_state decode_request_state(const bison::dynamic& payload) const;
  /// @brief Returns a copy of @p s with every `{{name}}` occurrence (in the
  /// URL, param/header/form-field keys and values, body text, and auth
  /// fields) replaced by the active environment's variable of that name --
  /// looked up by @p s.environment against environments_. A name with no
  /// matching variable is left literal (Postman's behavior).
  request_state resolve_environment(const request_state& s) const;
  /// @brief Returns @p entries as a dynamic array wrapped in a
  /// `dynamic_ptr`, ready to assign to a field (`field`'s variant has no
  /// bare-`dynamic` alternative -- see `bison_object.hpp`).
  bison::dynamic_ptr encode_kv(const std::vector<kv_entry>& entries) const;
  std::vector<kv_entry> decode_kv(const bison::dynamic& args, bison::key_t field_key, bool has_enabled) const;

  /// @brief Pushes one `append_command_log` trace row. Every `curl`
  /// invocation goes through here -- mirrors docker_source::run_logged().
  void push_command_log(const std::vector<std::string>& argv, const process_result& r) const;

  std::shared_ptr<bison::rmi::proxy::dynamic> proxy_;
  wish_app_host& host_;

  std::vector<saved_request> collections_;
  std::vector<environment> environments_;
  std::deque<history_entry> history_;
  static constexpr size_t kMaxHistory = 200;

  size_t next_id_{0}; ///< monotonically-increasing local id source.
  std::string new_id(const char* prefix);
};

} // namespace bdg::wish::curl
