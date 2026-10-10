// MIT License © 2026 Binary Dice Games
/// @file curl_source.cpp
/// @brief Implementation of curl_source.
#include "curl_source.hpp"

#include "curl_response_parser.hpp"
#include "src/client/wish_app_host.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish::curl {

using namespace bdg::bison;

namespace {

// ── User store encoding ─────────────────────────────────────────────────
//
// Collections, Environments and History persist in the session's user
// store (docs/persistent-store.md) as one entry each, so sending a request
// only rewrites the History entry. Each entry is
// {"version": 1, "items": [<item>, ...]}; items use the same field names as
// the request_state / saved_request / environment / history_entry members.

constexpr const char* kCollectionsEntry = "bdg.dev.curl.collections";
constexpr const char* kEnvironmentsEntry = "bdg.dev.curl.environments";
constexpr const char* kHistoryEntry = "bdg.dev.curl.history";
constexpr int32_t kStoreVersion = 1;

dynamic_ptr kv_array_to_store(const std::vector<kv_entry>& entries) {
  auto arr = std::make_shared<dynamic>();
  size_t i = 0;
  for (auto& e : entries) {
    auto o = std::make_shared<dynamic>();
    (*o)["key"_key] = e.key;
    (*o)["value"_key] = e.value;
    (*o)["enabled"_key] = e.enabled;
    (*arr)[i++] = dynamic_ptr{o};
  }
  return dynamic_ptr{arr};
}

std::vector<kv_entry> kv_array_from_store(const dynamic& o, key_t field_key) {
  std::vector<kv_entry> out;
  const auto* arr = o.findField<dynamic_ptr>(field_key);
  if (!arr || !*arr)
    return out;
  (*arr)->forEach([&](key_t, const field& f) {
    if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
      return;
    const auto& e = *f.as<dynamic_ptr>();
    kv_entry kv;
    kv.key = e.get_as<std::string>("key"_key, std::string{});
    kv.value = e.get_as<std::string>("value"_key, std::string{});
    kv.enabled = e.get_as<bool>("enabled"_key, true);
    out.push_back(std::move(kv));
  });
  return out;
}

/// @brief Writes @p s's fields into @p o (alongside an item's own id/name/...).
void state_to_store(const request_state& s, dynamic& o) {
  o["method"_key] = s.method;
  o["url"_key] = s.url;
  o["params"_key] = kv_array_to_store(s.params);
  o["headers"_key] = kv_array_to_store(s.headers);
  o["body_mode"_key] = s.body_mode;
  o["body_text"_key] = s.body_text;
  o["form_fields"_key] = kv_array_to_store(s.form_fields);
  o["auth_mode"_key] = s.auth_mode;
  o["auth_username"_key] = s.auth_username;
  o["auth_password"_key] = s.auth_password;
  o["auth_token"_key] = s.auth_token;
  o["follow_redirects"_key] = s.follow_redirects;
  o["environment"_key] = s.environment;
}

request_state state_from_store(const dynamic& o) {
  auto gs = [&](key_t k, const char* dflt = "") { return o.get_as<std::string>(k, std::string{dflt}); };
  request_state s;
  s.method = gs("method"_key, "GET");
  s.url = gs("url"_key);
  s.params = kv_array_from_store(o, "params"_key);
  s.headers = kv_array_from_store(o, "headers"_key);
  s.body_mode = gs("body_mode"_key, "none");
  s.body_text = gs("body_text"_key);
  s.form_fields = kv_array_from_store(o, "form_fields"_key);
  s.auth_mode = gs("auth_mode"_key, "none");
  s.auth_username = gs("auth_username"_key);
  s.auth_password = gs("auth_password"_key);
  s.auth_token = gs("auth_token"_key);
  s.follow_redirects = o.get_as<bool>("follow_redirects"_key, true);
  s.environment = gs("environment"_key);
  return s;
}

/// @brief Wraps @p items as a store entry value.
dynamic make_entry(const std::vector<dynamic_ptr>& items) {
  auto arr = std::make_shared<dynamic>();
  size_t i = 0;
  for (auto& item : items)
    (*arr)[i++] = item;
  dynamic entry;
  entry["version"_key] = kStoreVersion;
  entry["items"_key] = dynamic_ptr{arr};
  return entry;
}

/// @brief Calls @p fn for every item object of the store entry @p entry.
template <typename F>
void for_each_item(const dynamic& entry, F&& fn) {
  const auto* arr = entry.findField<dynamic_ptr>("items"_key);
  if (!arr || !*arr)
    return;
  (*arr)->forEach([&](key_t, const field& f) {
    if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>())
      fn(*f.as<dynamic_ptr>());
  });
}

// ── Small string helpers ────────────────────────────────────────────────

std::string substitute_vars(const std::string& text, const std::vector<kv_entry>& vars) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '{' && i + 1 < text.size() && text[i + 1] == '{') {
      size_t end = text.find("}}", i + 2);
      if (end != std::string::npos) {
        std::string name = text.substr(i + 2, end - (i + 2));
        size_t a = 0, b = name.size();
        while (a < b && std::isspace(static_cast<unsigned char>(name[a])))
          ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(name[b - 1])))
          --b;
        name = name.substr(a, b - a);
        const kv_entry* found = nullptr;
        for (auto& v : vars)
          if (v.key == name) {
            found = &v;
            break;
          }
        if (found) {
          out += found->value;
          i = end + 2;
          continue;
        }
      }
    }
    out += text[i++];
  }
  return out;
}

std::string percent_encode(const std::string& s) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

std::string build_url(const std::string& base, const std::vector<kv_entry>& params) {
  std::string qs;
  for (auto& p : params) {
    if (!p.enabled || p.key.empty())
      continue;
    if (!qs.empty())
      qs += '&';
    qs += percent_encode(p.key) + '=' + percent_encode(p.value);
  }
  if (qs.empty())
    return base;
  return base + (base.find('?') != std::string::npos ? "&" : "?") + qs;
}

std::string build_form_body(const std::vector<kv_entry>& fields) {
  std::string body;
  for (auto& f : fields) {
    if (!f.enabled || f.key.empty())
      continue;
    if (!body.empty())
      body += '&';
    body += percent_encode(f.key) + '=' + percent_encode(f.value);
  }
  return body;
}

bool header_name_eq(const std::string& a, const std::string& b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

bool has_header(const std::vector<kv_entry>& headers, const std::string& name) {
  for (auto& h : headers)
    if (h.enabled && header_name_eq(h.key, name))
      return true;
  return false;
}

} // namespace

// ── curl_source ───────────────────────────────────────────────────────

curl_source::curl_source(
    std::shared_ptr<bison::rmi::proxy::dynamic> proxy, wish_app_host& host,
    std::shared_ptr<common::command_worker> worker)
    : tool_source(std::move(proxy), std::move(worker), "curl"), host_(host) {}

std::string curl_source::new_id(const char* prefix) {
  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::string(prefix) + "_" + std::to_string(now) + "_" + std::to_string(++next_id_);
}

// ── Persistence ──────────────────────────────────────────────────────

void curl_source::load_store() {
  collections_.clear();
  environments_.clear();
  history_.clear();

  if (!host_.has_user_store()) {
    log_persistence(
        false,
        "Collections, Environments and History are not saved: anonymous session "
        "(reconnect with --username to keep them)");
    return;
  }

  try {
    if (auto entry = host_.user_store_get(kCollectionsEntry).get()) {
      for_each_item(*entry, [&](const dynamic& item) {
        saved_request r;
        r.id = item.get_as<std::string>("id"_key, std::string{});
        r.collection = item.get_as<std::string>("collection"_key, std::string{"Default"});
        r.name = item.get_as<std::string>("name"_key, std::string{});
        r.state = state_from_store(item);
        if (!r.id.empty())
          collections_.push_back(std::move(r));
      });
    }
    if (auto entry = host_.user_store_get(kEnvironmentsEntry).get()) {
      for_each_item(*entry, [&](const dynamic& item) {
        environment env;
        env.id = item.get_as<std::string>("id"_key, std::string{});
        env.name = item.get_as<std::string>("name"_key, std::string{});
        env.vars = kv_array_from_store(item, "vars"_key);
        if (!env.id.empty())
          environments_.push_back(std::move(env));
      });
    }
    if (auto entry = host_.user_store_get(kHistoryEntry).get()) {
      for_each_item(*entry, [&](const dynamic& item) {
        history_entry he;
        he.id = item.get_as<std::string>("id"_key, std::string{});
        he.status_code = item.get_as<int32_t>("status_code"_key, 0);
        he.ok = item.get_as<bool>("ok"_key, false);
        he.time_ms = item.get_as<float>("time_ms"_key, 0.0f);
        he.timestamp = item.get_as<std::string>("timestamp"_key, std::string{});
        he.state = state_from_store(item);
        if (!he.id.empty())
          history_.push_back(std::move(he));
      });
    }
  } catch (const std::exception& e) {
    log_persistence(false, std::string{"Could not load saved data: "} + e.what());
  }
  while (history_.size() > kMaxHistory)
    history_.pop_front();
}

void curl_source::save_collections() const {
  std::vector<dynamic_ptr> items;
  for (auto& r : collections_) {
    auto o = std::make_shared<dynamic>();
    state_to_store(r.state, *o);
    (*o)["id"_key] = r.id;
    (*o)["collection"_key] = r.collection;
    (*o)["name"_key] = r.name;
    items.push_back(dynamic_ptr{o});
  }
  write_entry(kCollectionsEntry, make_entry(items));
}

void curl_source::save_environments() const {
  std::vector<dynamic_ptr> items;
  for (auto& e : environments_) {
    auto o = std::make_shared<dynamic>();
    (*o)["id"_key] = e.id;
    (*o)["name"_key] = e.name;
    (*o)["vars"_key] = kv_array_to_store(e.vars);
    items.push_back(dynamic_ptr{o});
  }
  write_entry(kEnvironmentsEntry, make_entry(items));
}

void curl_source::save_history() const {
  std::vector<dynamic_ptr> items;
  for (auto& h : history_) {
    auto o = std::make_shared<dynamic>();
    state_to_store(h.state, *o);
    (*o)["id"_key] = h.id;
    (*o)["status_code"_key] = h.status_code;
    (*o)["ok"_key] = h.ok;
    (*o)["time_ms"_key] = h.time_ms;
    (*o)["timestamp"_key] = h.timestamp;
    items.push_back(dynamic_ptr{o});
  }
  write_entry(kHistoryEntry, make_entry(items));
}

void curl_source::write_entry(const char* name, dynamic value) const {
  if (!host_.has_user_store())
    return; // anonymous session: already reported once by load_store()
  try {
    host_.user_store_set(name, std::move(value)).get();
  } catch (const std::exception& e) {
    log_persistence(false, std::string{"Could not save "} + name + ": " + e.what());
  }
}

void curl_source::log_persistence(bool ok, const std::string& message) const {
  dynamic log;
  log["command"_key] = std::string{"user store"};
  log["exit_code"_key] = int32_t{ok ? 0 : 1};
  log["ok"_key] = ok;
  log["output"_key] = message;
  call("append_command_log"_key, std::move(log));
}

// ── Pushing snapshots ────────────────────────────────────────────────

dynamic_ptr curl_source::encode_kv(const std::vector<kv_entry>& entries) const {
  return kv_array_to_store(entries);
}

std::vector<kv_entry> curl_source::decode_kv(const dynamic& args, key_t field_key, bool has_enabled) const {
  std::vector<kv_entry> out;
  const auto* arr_f = args.findField<dynamic_ptr>(field_key);
  if (!arr_f || !*arr_f)
    return out;
  (*arr_f)->forEach([&](key_t, const field& f) {
    if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
      return;
    auto& e = *f.as<dynamic_ptr>();
    kv_entry kv;
    kv.key = e.as<std::string>("key"_key);
    kv.value = e.as<std::string>("value"_key);
    kv.enabled = true;
    if (has_enabled) {
      if (const auto* ef = e.findField<bool>("enabled"_key))
        kv.enabled = *ef;
    }
    out.push_back(std::move(kv));
  });
  return out;
}

request_state curl_source::decode_request_state(const dynamic& payload) const {
  request_state s;
  s.method = payload.as<std::string>("method"_key);
  s.url = payload.as<std::string>("url"_key);
  s.params = decode_kv(payload, "params"_key, true);
  s.headers = decode_kv(payload, "headers"_key, true);
  s.body_mode = payload.as<std::string>("body_mode"_key);
  s.body_text = payload.as<std::string>("body_text"_key);
  s.form_fields = decode_kv(payload, "form_fields"_key, true);
  s.auth_mode = payload.as<std::string>("auth_mode"_key);
  s.auth_username = payload.as<std::string>("auth_username"_key);
  s.auth_password = payload.as<std::string>("auth_password"_key);
  s.auth_token = payload.as<std::string>("auth_token"_key);
  s.follow_redirects = payload.findField<bool>("follow_redirects"_key) ? payload.as<bool>("follow_redirects"_key) : true;
  s.environment = payload.findField<std::string>("environment"_key) ? payload.as<std::string>("environment"_key) : std::string{};
  return s;
}

request_state curl_source::resolve_environment(const request_state& s) const {
  const environment* env = nullptr;
  if (!s.environment.empty()) {
    for (auto& e : environments_)
      if (e.id == s.environment) {
        env = &e;
        break;
      }
  }
  if (!env)
    return s;
  auto sub = [&](const std::string& in) { return substitute_vars(in, env->vars); };
  request_state r = s;
  r.url = sub(s.url);
  for (auto& p : r.params) {
    p.key = sub(p.key);
    p.value = sub(p.value);
  }
  for (auto& h : r.headers) {
    h.key = sub(h.key);
    h.value = sub(h.value);
  }
  r.body_text = sub(s.body_text);
  for (auto& f : r.form_fields) {
    f.key = sub(f.key);
    f.value = sub(f.value);
  }
  r.auth_username = sub(s.auth_username);
  r.auth_password = sub(s.auth_password);
  r.auth_token = sub(s.auth_token);
  return r;
}

void curl_source::push_collections() {
  dynamic arr;
  size_t i = 0;
  for (auto& r : collections_) {
    auto e = std::make_shared<dynamic>();
    (*e)["id"_key] = r.id;
    (*e)["collection"_key] = r.collection;
    (*e)["name"_key] = r.name;
    (*e)["method"_key] = r.state.method;
    (*e)["url"_key] = r.state.url;
    arr[i++] = dynamic_ptr{e};
  }
  dynamic args;
  args["entries"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  call("update_collections"_key, std::move(args));
}

void curl_source::push_environments() {
  dynamic arr;
  size_t i = 0;
  for (auto& e : environments_) {
    auto o = std::make_shared<dynamic>();
    (*o)["id"_key] = e.id;
    (*o)["name"_key] = e.name;
    (*o)["var_count"_key] = static_cast<int32_t>(e.vars.size());
    arr[i++] = dynamic_ptr{o};
  }
  dynamic args;
  args["entries"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  call("update_environments"_key, std::move(args));
}

void curl_source::push_history() {
  dynamic arr;
  size_t i = 0;
  for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
    auto o = std::make_shared<dynamic>();
    (*o)["id"_key] = it->id;
    (*o)["method"_key] = it->state.method;
    (*o)["url"_key] = it->state.url;
    (*o)["status_code"_key] = it->status_code;
    (*o)["ok"_key] = it->ok;
    (*o)["time_ms"_key] = it->time_ms;
    (*o)["timestamp"_key] = it->timestamp;
    arr[i++] = dynamic_ptr{o};
  }
  dynamic args;
  args["entries"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  call("update_history"_key, std::move(args));
}

void curl_source::push_request_builder(const request_state& s) {
  dynamic args;
  args["method"_key] = s.method;
  args["url"_key] = s.url;
  args["params"_key] = encode_kv(s.params);
  args["headers"_key] = encode_kv(s.headers);
  args["body_mode"_key] = s.body_mode;
  args["body_text"_key] = s.body_text;
  args["form_fields"_key] = encode_kv(s.form_fields);
  args["auth_mode"_key] = s.auth_mode;
  args["auth_username"_key] = s.auth_username;
  args["auth_password"_key] = s.auth_password;
  args["auth_token"_key] = s.auth_token;
  call("update_request_builder"_key, std::move(args));
}

// ── Sending a request ────────────────────────────────────────────────

void curl_source::send_request(const request_state& raw) {
  request_state s = resolve_environment(raw);

  std::vector<std::string> argv = {"-sS", "-i"};
  if (s.follow_redirects)
    argv.push_back("-L");
  const std::string method = s.method.empty() ? std::string{"GET"} : s.method;
  // A HEAD request has no response body, but plain `-X HEAD` doesn't tell
  // curl that -- it still expects Content-Length bytes of body and, when
  // (correctly) none arrive, fails with exit 18 ("transfer closed with N
  // bytes remaining to read"), even though the request itself succeeded.
  // `--head` (curl's own `-I`) is what actually suppresses that
  // expectation; kept alongside `-X HEAD` (rather than replacing it) so
  // the method is still always passed the same uniform way as every
  // other method. Confirmed live: `curl -X HEAD https://httpbin.org/...`
  // reports exit 18, `curl --head -X HEAD` the same URL reports exit 0.
  if (method == "HEAD")
    argv.push_back("--head");
  argv.push_back("-X");
  argv.push_back(method);

  for (auto& h : s.headers) {
    if (!h.enabled || h.key.empty())
      continue;
    argv.push_back("-H");
    argv.push_back(h.key + ": " + h.value);
  }

  // Auth precedence: a manually-added Authorization header always wins
  // over the Auth tab (see server/curl.hpp's class doc comment).
  if (s.auth_mode == "basic") {
    argv.push_back("-u");
    argv.push_back(s.auth_username + ":" + s.auth_password);
  } else if (s.auth_mode == "bearer" && !s.auth_token.empty() && !has_header(s.headers, "Authorization")) {
    argv.push_back("-H");
    argv.push_back("Authorization: Bearer " + s.auth_token);
  }

  const bool has_ct = has_header(s.headers, "Content-Type");
  if (s.body_mode == "raw" || s.body_mode == "json") {
    argv.push_back("--data-raw");
    argv.push_back(s.body_text);
    if (s.body_mode == "json" && !has_ct) {
      argv.push_back("-H");
      argv.push_back("Content-Type: application/json");
    }
  } else if (s.body_mode == "form") {
    argv.push_back("--data-raw");
    argv.push_back(build_form_body(s.form_fields));
    if (!has_ct) {
      argv.push_back("-H");
      argv.push_back("Content-Type: application/x-www-form-urlencoded");
    }
  }

  // One captured stream: `-i` prepends the final status line + headers
  // ahead of the body; this trailer appends status/timing/size after it.
  argv.push_back("-w");
  argv.push_back("\n__WISH_CURL_META__\t%{http_code}\t%{time_total}\t%{size_download}\n");
  argv.push_back(build_url(s.url, s.params));

  // The dialog is captioned with the method and URL only (the argv carries
  // credentials and headers), and shows no output: curl's stdout is the
  // response itself, which belongs in the Response window.
  process_result r = worker_->run(
      "curl " + method + " " + argv.back(),
      [&](const common::run_hooks* hooks) {
        common::process_options options;
        options.hooks = hooks;
        return run(argv, std::move(options));
      },
      /*show_output=*/false);
  log_command(caption(argv), r);

  history_entry he;
  he.id = new_id("h");
  he.state = raw; // the un-substituted builder state, so "Load" restores {{vars}} literally.
  {
    std::time_t now = std::time(nullptr);
    char buf[32];
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    he.timestamp = buf;
  }

  dynamic resp_args;
  if (r.exit_code != 0) {
    std::string err =
        !r.stderr_text.empty() ? r.stderr_text : ("curl exited with code " + std::to_string(r.exit_code));
    while (!err.empty() && (err.back() == '\n' || err.back() == '\r'))
      err.pop_back();
    resp_args["ok"_key] = false;
    resp_args["error"_key] = err;
    he.ok = false;
    he.status_code = 0;
    he.time_ms = 0.0f;
  } else {
    parsed_response pr = parse_curl_output(r.stdout_text);
    resp_args["ok"_key] = true;
    resp_args["status_code"_key] = pr.status_code;
    resp_args["status_text"_key] = pr.status_text;
    resp_args["time_ms"_key] = pr.time_ms;
    resp_args["size_bytes"_key] = pr.size_bytes;
    resp_args["headers"_key] = encode_kv(pr.headers);

    std::string content_type;
    for (auto& h : pr.headers) {
      if (header_name_eq(h.key, "Content-Type")) {
        content_type = h.value;
        break;
      }
    }
    std::string ct_lower = content_type;
    std::transform(ct_lower.begin(), ct_lower.end(), ct_lower.begin(), [](unsigned char c) { return std::tolower(c); });
    bool is_json = ct_lower.find("json") != std::string::npos;
    if (!is_json) {
      size_t p = pr.body.find_first_not_of(" \t\r\n");
      if (p != std::string::npos && (pr.body[p] == '{' || pr.body[p] == '['))
        is_json = true;
    }
    const bool looks_binary = pr.body.find('\0') != std::string::npos;

    std::string body_file;
    if (!looks_binary) {
      // A unique name per response, not a fixed "curl_response.txt" --
      // found live (2026-09, automation module cycling all seven HTTP
      // methods against one URL in a single session): reusing the same
      // filename every time means the TextEditor's file_path field value
      // is byte-for-byte identical response after response even though
      // the underlying sandbox file's *content* legitimately changed, so
      // nothing ever signals the client-side editor to re-fetch it -- the
      // Body pane silently kept showing the very first response forever,
      // while the status line/headers/Console (driven by ordinary RMI
      // field writes, not a file re-fetch) updated correctly every time.
      body_file = new_id("curl_response") + ".txt";
      try {
        host_.upload_file(body_file, pr.body).get();
      } catch (const std::exception&) {
        body_file.clear();
      }
    }
    resp_args["body_file"_key] = body_file;
    resp_args["body_is_json"_key] = is_json;

    he.ok = pr.status_code >= 200 && pr.status_code < 400;
    he.status_code = pr.status_code;
    he.time_ms = pr.time_ms;
  }

  call("update_response"_key, std::move(resp_args));

  history_.push_back(std::move(he));
  while (history_.size() > kMaxHistory)
    history_.pop_front();
  save_history();
  push_history();
}

// ── *_requested event reactions ─────────────────────────────────────

void curl_source::refresh_all() {
  load_store();
  push_collections();
  push_environments();
  push_history();
}

void curl_source::on_send_requested(const dynamic& payload) {
  send_request(decode_request_state(payload));
}

void curl_source::on_save_request_requested(const dynamic& payload) {
  saved_request r;
  r.id = new_id("r");
  r.collection = payload.as<std::string>("collection"_key);
  r.name = payload.as<std::string>("name"_key);
  r.state = decode_request_state(payload);
  collections_.push_back(std::move(r));
  save_collections();
  push_collections();
}

void curl_source::on_load_request_requested(const std::string& id) {
  for (auto& r : collections_) {
    if (r.id == id) {
      push_request_builder(r.state);
      return;
    }
  }
}

void curl_source::on_delete_request_requested(const std::string& id) {
  collections_.erase(
      std::remove_if(collections_.begin(), collections_.end(), [&](const saved_request& r) { return r.id == id; }),
      collections_.end());
  save_collections();
  push_collections();
}

void curl_source::on_duplicate_request_requested(const std::string& id) {
  for (auto& r : collections_) {
    if (r.id == id) {
      saved_request copy = r;
      copy.id = new_id("r");
      copy.name = r.name + " (copy)";
      collections_.push_back(std::move(copy));
      break;
    }
  }
  save_collections();
  push_collections();
}

void curl_source::on_load_history_requested(const std::string& id) {
  for (auto& h : history_) {
    if (h.id == id) {
      push_request_builder(h.state);
      return;
    }
  }
}

void curl_source::on_clear_history_requested() {
  history_.clear();
  save_history();
  push_history();
}

void curl_source::on_new_environment_requested(const std::string& name) {
  environment env;
  env.id = new_id("e");
  env.name = name;
  environments_.push_back(std::move(env));
  save_environments();
  push_environments();
}

void curl_source::on_delete_environment_requested(const std::string& id) {
  environments_.erase(
      std::remove_if(environments_.begin(), environments_.end(), [&](const environment& e) { return e.id == id; }),
      environments_.end());
  save_environments();
  push_environments();

  dynamic args;
  args["environment_id"_key] = std::string{};
  args["name"_key] = std::string{};
  args["vars"_key] = dynamic_ptr{std::make_shared<dynamic>()};
  call("update_environment_vars"_key, std::move(args));
}

void curl_source::on_select_environment_requested(const std::string& id) {
  for (auto& e : environments_) {
    if (e.id == id) {
      dynamic args;
      args["environment_id"_key] = e.id;
      args["name"_key] = e.name;
      args["vars"_key] = encode_kv(e.vars);
      call("update_environment_vars"_key, std::move(args));
      return;
    }
  }
}

void curl_source::on_save_environment_vars_requested(const dynamic& payload) {
  const std::string id = payload.as<std::string>("id"_key);
  for (auto& e : environments_) {
    if (e.id != id)
      continue;
    e.vars = decode_kv(payload, "vars"_key, false);
    save_environments();
    push_environments();
    on_select_environment_requested(id); // re-push so var_count / editor stay in sync
    return;
  }
}

} // namespace bdg::wish::curl
