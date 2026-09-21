// MIT License © 2026 Binary Dice Games
/// @file curl_source.cpp
/// @brief Implementation of curl_source.
#include "curl_source.hpp"

#include "curl_response_parser.hpp"
#include "src/client/wish_app_host.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace bdg::wish::curl {

using namespace bdg::bison;

namespace {

// ── Minimal hand-rolled JSON (docker's "no JSON library" decision --
// nlohmann::json's include path isn't available to module-client sources).
// Just enough to round-trip this module's own flat store schema; not a
// general-purpose parser. ─────────────────────────────────────────────────

struct json_value {
  enum class kind { null_, bool_, num_, str_, arr_, obj_ } k{kind::null_};
  bool b{false};
  double n{0};
  std::string s;
  std::vector<json_value> arr;
  std::vector<std::pair<std::string, json_value>> obj;

  static json_value make_str(std::string v) {
    json_value j;
    j.k = kind::str_;
    j.s = std::move(v);
    return j;
  }
  static json_value make_bool(bool v) {
    json_value j;
    j.k = kind::bool_;
    j.b = v;
    return j;
  }
  static json_value make_num(double v) {
    json_value j;
    j.k = kind::num_;
    j.n = v;
    return j;
  }
  static json_value make_arr() {
    json_value j;
    j.k = kind::arr_;
    return j;
  }
  static json_value make_obj() {
    json_value j;
    j.k = kind::obj_;
    return j;
  }

  void set(const std::string& key, json_value v) {
    obj.emplace_back(key, std::move(v));
  }
  const json_value* find(const std::string& key) const {
    for (auto& [k2, v] : obj)
      if (k2 == key)
        return &v;
    return nullptr;
  }
  std::string str(const std::string& def = "") const {
    return k == kind::str_ ? s : def;
  }
  bool boolean(bool def = false) const {
    return k == kind::bool_ ? b : def;
  }
  double num(double def = 0) const {
    return k == kind::num_ ? n : def;
  }
};

void write_json_string(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

void write_json(std::string& out, const json_value& v) {
  switch (v.k) {
    case json_value::kind::null_:
      out += "null";
      break;
    case json_value::kind::bool_:
      out += v.b ? "true" : "false";
      break;
    case json_value::kind::num_: {
      std::ostringstream oss;
      oss << v.n;
      out += oss.str();
      break;
    }
    case json_value::kind::str_:
      write_json_string(out, v.s);
      break;
    case json_value::kind::arr_: {
      out += '[';
      for (size_t i = 0; i < v.arr.size(); ++i) {
        if (i)
          out += ',';
        write_json(out, v.arr[i]);
      }
      out += ']';
      break;
    }
    case json_value::kind::obj_: {
      out += '{';
      for (size_t i = 0; i < v.obj.size(); ++i) {
        if (i)
          out += ',';
        write_json_string(out, v.obj[i].first);
        out += ':';
        write_json(out, v.obj[i].second);
      }
      out += '}';
      break;
    }
  }
}

class json_parser {
 public:
  explicit json_parser(const std::string& text) : s_(text) {}
  bool parse(json_value& out) {
    skip_ws();
    return parse_value(out);
  }

 private:
  const std::string& s_;
  size_t i_{0};

  void skip_ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
      ++i_;
  }
  bool peek(char c) {
    return i_ < s_.size() && s_[i_] == c;
  }
  bool consume(char c) {
    if (peek(c)) {
      ++i_;
      return true;
    }
    return false;
  }

  bool parse_value(json_value& out) {
    skip_ws();
    if (i_ >= s_.size())
      return false;
    char c = s_[i_];
    if (c == '{')
      return parse_object(out);
    if (c == '[')
      return parse_array(out);
    if (c == '"') {
      std::string str;
      if (!parse_string(str))
        return false;
      out = json_value::make_str(std::move(str));
      return true;
    }
    if (c == 't') {
      if (s_.compare(i_, 4, "true") == 0) {
        i_ += 4;
        out = json_value::make_bool(true);
        return true;
      }
      return false;
    }
    if (c == 'f') {
      if (s_.compare(i_, 5, "false") == 0) {
        i_ += 5;
        out = json_value::make_bool(false);
        return true;
      }
      return false;
    }
    if (c == 'n') {
      if (s_.compare(i_, 4, "null") == 0) {
        i_ += 4;
        out = json_value{};
        return true;
      }
      return false;
    }
    size_t start = i_;
    if (c == '-')
      ++i_;
    while (i_ < s_.size() &&
           (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E' ||
            s_[i_] == '+' || s_[i_] == '-'))
      ++i_;
    if (i_ == start)
      return false;
    try {
      out = json_value::make_num(std::stod(s_.substr(start, i_ - start)));
    } catch (const std::exception&) {
      return false;
    }
    return true;
  }

  bool parse_string(std::string& out) {
    if (!consume('"'))
      return false;
    out.clear();
    while (i_ < s_.size() && s_[i_] != '"') {
      char c = s_[i_++];
      if (c == '\\' && i_ < s_.size()) {
        char e = s_[i_++];
        switch (e) {
          case '"':
            out += '"';
            break;
          case '\\':
            out += '\\';
            break;
          case '/':
            out += '/';
            break;
          case 'n':
            out += '\n';
            break;
          case 'r':
            out += '\r';
            break;
          case 't':
            out += '\t';
            break;
          case 'b':
            out += '\b';
            break;
          case 'f':
            out += '\f';
            break;
          case 'u': {
            if (i_ + 4 <= s_.size()) {
              unsigned int cp = 0;
              for (int k = 0; k < 4; ++k) {
                cp <<= 4;
                char h = s_[i_ + static_cast<size_t>(k)];
                if (h >= '0' && h <= '9')
                  cp |= static_cast<unsigned int>(h - '0');
                else if (h >= 'a' && h <= 'f')
                  cp |= static_cast<unsigned int>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F')
                  cp |= static_cast<unsigned int>(h - 'A' + 10);
              }
              i_ += 4;
              // Minimal UTF-8 encode (BMP only, no surrogate pairs -- our
              // own writer never emits \u, so this only matters for a
              // hand-edited store file; an acceptable v1 limitation).
              if (cp < 0x80) {
                out += static_cast<char>(cp);
              } else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
              } else {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
              }
            }
            break;
          }
          default:
            out += e;
        }
      } else {
        out += c;
      }
    }
    return consume('"');
  }

  bool parse_array(json_value& out) {
    if (!consume('['))
      return false;
    out = json_value::make_arr();
    skip_ws();
    if (consume(']'))
      return true;
    while (true) {
      json_value elem;
      if (!parse_value(elem))
        return false;
      out.arr.push_back(std::move(elem));
      skip_ws();
      if (consume(','))
        continue;
      if (consume(']'))
        break;
      return false;
    }
    return true;
  }

  bool parse_object(json_value& out) {
    if (!consume('{'))
      return false;
    out = json_value::make_obj();
    skip_ws();
    if (consume('}'))
      return true;
    while (true) {
      skip_ws();
      std::string key;
      if (!parse_string(key))
        return false;
      skip_ws();
      if (!consume(':'))
        return false;
      json_value val;
      if (!parse_value(val))
        return false;
      out.obj.emplace_back(std::move(key), std::move(val));
      skip_ws();
      if (consume(','))
        continue;
      if (consume('}'))
        break;
      return false;
    }
    return true;
  }
};

// ── Store <-> json_value conversion ─────────────────────────────────────

json_value kv_to_json(const kv_entry& e, bool has_enabled) {
  json_value o = json_value::make_obj();
  o.set("key", json_value::make_str(e.key));
  o.set("value", json_value::make_str(e.value));
  if (has_enabled)
    o.set("enabled", json_value::make_bool(e.enabled));
  return o;
}
kv_entry kv_from_json(const json_value& v) {
  kv_entry e;
  if (auto* f = v.find("key"))
    e.key = f->str();
  if (auto* f = v.find("value"))
    e.value = f->str();
  e.enabled = v.find("enabled") ? v.find("enabled")->boolean(true) : true;
  return e;
}
json_value kv_array_to_json(const std::vector<kv_entry>& v, bool has_enabled) {
  json_value a = json_value::make_arr();
  for (auto& e : v)
    a.arr.push_back(kv_to_json(e, has_enabled));
  return a;
}
std::vector<kv_entry> kv_array_from_json(const json_value* v) {
  std::vector<kv_entry> out;
  if (!v || v->k != json_value::kind::arr_)
    return out;
  for (auto& e : v->arr)
    out.push_back(kv_from_json(e));
  return out;
}

json_value state_to_json(const request_state& s) {
  json_value o = json_value::make_obj();
  o.set("method", json_value::make_str(s.method));
  o.set("url", json_value::make_str(s.url));
  o.set("params", kv_array_to_json(s.params, true));
  o.set("headers", kv_array_to_json(s.headers, true));
  o.set("body_mode", json_value::make_str(s.body_mode));
  o.set("body_text", json_value::make_str(s.body_text));
  o.set("form_fields", kv_array_to_json(s.form_fields, true));
  o.set("auth_mode", json_value::make_str(s.auth_mode));
  o.set("auth_username", json_value::make_str(s.auth_username));
  o.set("auth_password", json_value::make_str(s.auth_password));
  o.set("auth_token", json_value::make_str(s.auth_token));
  o.set("follow_redirects", json_value::make_bool(s.follow_redirects));
  o.set("environment", json_value::make_str(s.environment));
  return o;
}
request_state state_from_json(const json_value& o) {
  request_state s;
  auto gs = [&](const char* k) { auto* f = o.find(k); return f ? f->str() : std::string{}; };
  s.method = gs("method");
  if (s.method.empty())
    s.method = "GET";
  s.url = gs("url");
  s.params = kv_array_from_json(o.find("params"));
  s.headers = kv_array_from_json(o.find("headers"));
  s.body_mode = gs("body_mode");
  if (s.body_mode.empty())
    s.body_mode = "none";
  s.body_text = gs("body_text");
  s.form_fields = kv_array_from_json(o.find("form_fields"));
  s.auth_mode = gs("auth_mode");
  if (s.auth_mode.empty())
    s.auth_mode = "none";
  s.auth_username = gs("auth_username");
  s.auth_password = gs("auth_password");
  s.auth_token = gs("auth_token");
  s.follow_redirects = o.find("follow_redirects") ? o.find("follow_redirects")->boolean(true) : true;
  s.environment = gs("environment");
  return s;
}

// ── Store file location ─────────────────────────────────────────────────
//
// A per-user config directory on the machine running the client -- not the
// wish session sandbox (session::resource_dir): this is the client's own
// local app state, the same trust boundary as imgui.ini. See DESIGN.md
// "Persistence". Platform difference kept as a narrow #if guard in this
// shared file per CLAUDE.md's platform-support rule, rather than a
// separate _win/_posix file.

std::string store_dir_path() {
#if defined(_WIN32)
  const char* appdata = std::getenv("APPDATA");
  std::string base = (appdata && *appdata) ? appdata : ".";
  return base + "\\wish\\curl";
#else
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  std::string base;
  if (xdg && *xdg) {
    base = xdg;
  } else {
    const char* home = std::getenv("HOME");
    base = (home && *home) ? std::string(home) + "/.config" : std::string(".config");
  }
  return base + "/wish/curl";
#endif
}

std::string store_file_path() {
#if defined(_WIN32)
  return store_dir_path() + "\\store.json";
#else
  return store_dir_path() + "/store.json";
#endif
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

curl_source::curl_source(std::shared_ptr<bison::rmi::proxy::dynamic> proxy, wish_app_host& host)
    : proxy_(std::move(proxy)), host_(host) {}

std::string curl_source::new_id(const char* prefix) {
  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::string(prefix) + "_" + std::to_string(now) + "_" + std::to_string(++next_id_);
}

// ── Persistence ──────────────────────────────────────────────────────

void curl_source::load_store() {
  collections_.clear();
  environments_.clear();
  history_.clear();

  std::ifstream in(store_file_path(), std::ios::binary);
  if (!in)
    return;
  std::ostringstream buf;
  buf << in.rdbuf();
  const std::string text = buf.str();
  if (text.empty())
    return;

  json_value root;
  json_parser parser(text);
  if (!parser.parse(root) || root.k != json_value::kind::obj_)
    return;

  if (auto* c = root.find("collections")) {
    for (auto& item : c->arr) {
      saved_request r;
      if (auto* f = item.find("id"))
        r.id = f->str();
      r.collection = item.find("collection") ? item.find("collection")->str() : std::string{"Default"};
      if (auto* f = item.find("name"))
        r.name = f->str();
      r.state = state_from_json(item);
      if (!r.id.empty())
        collections_.push_back(std::move(r));
    }
  }
  if (auto* e = root.find("environments")) {
    for (auto& item : e->arr) {
      environment env;
      if (auto* f = item.find("id"))
        env.id = f->str();
      if (auto* f = item.find("name"))
        env.name = f->str();
      env.vars = kv_array_from_json(item.find("vars"));
      if (!env.id.empty())
        environments_.push_back(std::move(env));
    }
  }
  if (auto* h = root.find("history")) {
    for (auto& item : h->arr) {
      history_entry he;
      if (auto* f = item.find("id"))
        he.id = f->str();
      he.status_code = item.find("status_code") ? static_cast<int32_t>(item.find("status_code")->num()) : 0;
      he.ok = item.find("ok") ? item.find("ok")->boolean() : false;
      he.time_ms = item.find("time_ms") ? static_cast<float>(item.find("time_ms")->num()) : 0.0f;
      if (auto* f = item.find("timestamp"))
        he.timestamp = f->str();
      he.state = state_from_json(item);
      if (!he.id.empty())
        history_.push_back(std::move(he));
    }
  }
  while (history_.size() > kMaxHistory)
    history_.pop_front();
}

void curl_source::save_store() const {
  json_value root = json_value::make_obj();

  json_value c_arr = json_value::make_arr();
  for (auto& r : collections_) {
    json_value o = state_to_json(r.state);
    o.set("id", json_value::make_str(r.id));
    o.set("collection", json_value::make_str(r.collection));
    o.set("name", json_value::make_str(r.name));
    c_arr.arr.push_back(std::move(o));
  }
  root.set("collections", std::move(c_arr));

  json_value e_arr = json_value::make_arr();
  for (auto& e : environments_) {
    json_value o = json_value::make_obj();
    o.set("id", json_value::make_str(e.id));
    o.set("name", json_value::make_str(e.name));
    o.set("vars", kv_array_to_json(e.vars, false));
    e_arr.arr.push_back(std::move(o));
  }
  root.set("environments", std::move(e_arr));

  json_value h_arr = json_value::make_arr();
  for (auto& h : history_) {
    json_value o = state_to_json(h.state);
    o.set("id", json_value::make_str(h.id));
    o.set("status_code", json_value::make_num(h.status_code));
    o.set("ok", json_value::make_bool(h.ok));
    o.set("time_ms", json_value::make_num(h.time_ms));
    o.set("timestamp", json_value::make_str(h.timestamp));
    h_arr.arr.push_back(std::move(o));
  }
  root.set("history", std::move(h_arr));

  std::string text;
  write_json(text, root);

  std::error_code ec;
  std::filesystem::create_directories(store_dir_path(), ec);
  std::ofstream out(store_file_path(), std::ios::binary | std::ios::trunc);
  if (out)
    out << text;
}

// ── Pushing snapshots ────────────────────────────────────────────────

dynamic_ptr curl_source::encode_kv(const std::vector<kv_entry>& entries) const {
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
  try {
    proxy_->call("update_collections"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
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
  try {
    proxy_->call("update_environments"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
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
  try {
    proxy_->call("update_history"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
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
  try {
    proxy_->call("update_request_builder"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
}

void curl_source::push_command_log(const std::vector<std::string>& argv, const process_result& r) const {
  std::string command = "curl";
  for (auto& a : argv)
    command += ' ' + a;

  std::string output = r.ok() ? r.stdout_text : (r.stderr_text.empty() ? r.stdout_text : r.stderr_text);
  std::replace(output.begin(), output.end(), '\n', ' ');
  constexpr size_t kMaxOutputPreview = 200;
  if (output.size() > kMaxOutputPreview)
    output = output.substr(0, kMaxOutputPreview) + "...";

  dynamic args;
  args["command"_key] = command;
  args["exit_code"_key] = r.exit_code;
  args["ok"_key] = r.ok();
  args["output"_key] = std::move(output);
  try {
    proxy_->call("append_command_log"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
}

// ── Sending a request ────────────────────────────────────────────────

void curl_source::send_request(const request_state& raw) {
  request_state s = resolve_environment(raw);

  std::vector<std::string> argv = {"-sS", "-i"};
  if (s.follow_redirects)
    argv.push_back("-L");
  argv.push_back("-X");
  argv.push_back(s.method.empty() ? std::string{"GET"} : s.method);

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

  process_result r = run_curl_cli(argv);
  push_command_log(argv, r);

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
      body_file = "curl_response.txt";
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

  try {
    proxy_->call("update_response"_key, std::move(resp_args)).get();
  } catch (const std::exception&) {
  }

  history_.push_back(std::move(he));
  while (history_.size() > kMaxHistory)
    history_.pop_front();
  save_store();
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
  save_store();
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
  save_store();
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
  save_store();
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
  save_store();
  push_history();
}

void curl_source::on_new_environment_requested(const std::string& name) {
  environment env;
  env.id = new_id("e");
  env.name = name;
  environments_.push_back(std::move(env));
  save_store();
  push_environments();
}

void curl_source::on_delete_environment_requested(const std::string& id) {
  environments_.erase(
      std::remove_if(environments_.begin(), environments_.end(), [&](const environment& e) { return e.id == id; }),
      environments_.end());
  save_store();
  push_environments();

  dynamic args;
  args["environment_id"_key] = std::string{};
  args["name"_key] = std::string{};
  args["vars"_key] = dynamic_ptr{std::make_shared<dynamic>()};
  try {
    proxy_->call("update_environment_vars"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
}

void curl_source::on_select_environment_requested(const std::string& id) {
  for (auto& e : environments_) {
    if (e.id == id) {
      dynamic args;
      args["environment_id"_key] = e.id;
      args["name"_key] = e.name;
      args["vars"_key] = encode_kv(e.vars);
      try {
        proxy_->call("update_environment_vars"_key, std::move(args)).get();
      } catch (const std::exception&) {
      }
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
    save_store();
    push_environments();
    on_select_environment_requested(id); // re-push so var_count / editor stay in sync
    return;
  }
}

} // namespace bdg::wish::curl
