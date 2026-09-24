// MIT License © 2026 Binary Dice Games
/// @file sq_source.cpp
/// @brief Implementation of sq_source.
#include "sq_source.hpp"

#include "sq_query_guard.hpp"
#include "sq_result_parser.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>

namespace bdg::wish::sq {

using namespace bdg::bison;

namespace {

std::string trim_eol(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    s.pop_back();
  return s;
}

// The first line of sq's stderr ("sq: <what failed>") is the useful one.
std::string error_text(const process_result& r) {
  std::string text = trim_eol(r.stderr_text.empty() ? r.stdout_text : r.stderr_text);
  if (r.exit_code == -1 && text.empty())
    return "could not run `sq`";
  return text;
}

template <typename Fn>
void for_each_entry(const dynamic& parent, key_t field_key, Fn&& fn) {
  const auto* arr_f = parent.findField<dynamic_ptr>(field_key);
  if (!arr_f || !*arr_f)
    return;
  (*arr_f)->forEach([&](key_t, const field& f) {
    if (!f.is<dynamic_ptr>())
      return;
    auto entry_ptr = f.as<dynamic_ptr>();
    if (entry_ptr)
      fn(*entry_ptr);
  });
}

std::string str_of(const dynamic& d, key_t k) {
  const auto* f = d.findField<std::string>(k);
  return f ? *f : std::string{};
}

bool bool_of(const dynamic& d, key_t k, bool fallback) {
  const auto* f = d.findField<bool>(k);
  return f ? *f : fallback;
}

// bison::from_json stores integers as int32_t and non-integers as float.
int32_t int_of(const dynamic& d, key_t k, int32_t fallback) {
  if (const auto* i = d.findField<int32_t>(k))
    return *i;
  if (const auto* f = d.findField<float>(k))
    return static_cast<int32_t>(*f);
  return fallback;
}

// An indexed array of strings (how from_json stores a JSON string array).
std::vector<std::string> strings_of(const dynamic& d, key_t k) {
  std::vector<std::string> out;
  const auto* arr = d.findField<dynamic_ptr>(k);
  if (arr && *arr)
    (*arr)->forEach([&](key_t, const field& f) {
      if (f.is<std::string>())
        out.push_back(f.as<std::string>());
    });
  return out;
}

std::string join(const std::vector<std::string>& v, const char* sep) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i)
    out += (i ? sep : "") + v[i];
  return out;
}

// Parses a `sq ... -j` document. @p wrap_array wraps a top-level array in an
// object first (from_json only accepts an object root).
dynamic_ptr parse_json(const std::string& text, bool wrap_array) {
  return extensions::from_json(wrap_array ? "{\"items\":" + text + "}" : text);
}

void call(const std::shared_ptr<bison::rmi::proxy::dynamic>& proxy, key_t method, dynamic args) {
  try {
    proxy->call(method, std::move(args)).get();
  } catch (const std::exception&) {
    // Best effort: a torn-down form just swallows the call.
  }
}

dynamic_ptr as_array(dynamic&& arr) {
  return dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
}

} // namespace

sq_source::sq_source(std::shared_ptr<bison::rmi::proxy::dynamic> proxy) : proxy_(std::move(proxy)) {}

process_result sq_source::run_logged(const std::vector<std::string>& args, const std::string& stdin_text) {
  auto r = run_sq_cli(args, "sq", stdin_text);

  std::string command = "sq";
  for (auto& a : args)
    command += ' ' + mask_location(a);
  if (command.size() > 300)
    command = command.substr(0, 300) + "...";
  std::string output = trim_eol(r.ok() ? std::string{} : error_text(r));
  std::replace(output.begin(), output.end(), '\n', ' ');
  std::replace(output.begin(), output.end(), '\t', ' ');
  if (output.size() > 200)
    output = output.substr(0, 200) + "...";

  dynamic log;
  log["command"_key] = command;
  log["exit_code"_key] = static_cast<int32_t>(r.exit_code);
  log["ok"_key] = r.ok();
  log["output"_key] = output;
  call(proxy_, "append_command_log"_key, std::move(log));
  return r;
}

void sq_source::report(const std::string& scope, bool ok, const std::string& message) {
  dynamic p;
  p["scope"_key] = scope;
  p["ok"_key] = ok;
  p["message"_key] = message;
  call(proxy_, "command_result"_key, std::move(p));
}

// ── Snapshots ──────────────────────────────────────────────────────────────

void sq_source::push_drivers() {
  auto r = run_logged({"driver", "ls", "-j"});
  dynamic arr;
  if (r.ok()) {
    try {
      auto root = parse_json(r.stdout_text, true);
      size_t i = 0;
      for_each_entry(*root, "items"_key, [&](const dynamic& d) {
        if (!bool_of(d, "is_sql"_key, false))
          return;
        auto e = std::make_shared<dynamic>();
        (*e)["type"_key] = str_of(d, "type"_key);
        (*e)["description"_key] = str_of(d, "description"_key);
        arr[i++] = dynamic_ptr{e};
      });
    } catch (const std::exception&) {
    }
  }
  dynamic args;
  args["drivers"_key] = as_array(std::move(arr));
  call(proxy_, "update_drivers"_key, std::move(args));
  drivers_pushed_ = true;
}

void sq_source::push_connections() {
  auto r = run_logged({"ls", "-j"});
  dynamic arr;
  std::string sq_active;
  if (r.ok()) {
    try {
      auto root = parse_json(r.stdout_text, false);
      sq_active = str_of(*root, "active.source"_key);
      size_t i = 0;
      for_each_entry(*root, "sources"_key, [&](const dynamic& d) {
        auto e = std::make_shared<dynamic>();
        (*e)["handle"_key] = str_of(d, "handle"_key);
        (*e)["driver"_key] = str_of(d, "driver"_key);
        (*e)["location"_key] = mask_location(str_of(d, "location"_key));
        arr[i++] = dynamic_ptr{e};
      });
    } catch (const std::exception& ex) {
      report("connections", false, std::string{"Could not parse `sq ls` output: "} + ex.what());
    }
  } else {
    report("connections", false, error_text(r));
  }

  // First call: start on sq's own active source. Afterwards keep the
  // session's choice, unless that source was removed.
  bool active_exists = false;
  arr.forEach([&](key_t, const field& f) {
    if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>() && str_of(*f.as<dynamic_ptr>(), "handle"_key) == active_)
      active_exists = true;
  });
  if (!active_exists)
    active_ = sq_active;

  dynamic args;
  args["active"_key] = active_;
  args["entries"_key] = as_array(std::move(arr));
  call(proxy_, "update_connections"_key, std::move(args));
}

void sq_source::push_schema() {
  dynamic args;
  if (active_.empty()) {
    args["handle"_key] = std::string{};
    call(proxy_, "update_schema"_key, std::move(args));
    return;
  }

  args["handle"_key] = active_;
  auto r = run_logged({"inspect", active_, "-j"});
  dynamic tables;
  if (!r.ok()) {
    args["error"_key] = "Could not inspect " + active_ + ": " + error_text(r);
  } else {
    try {
      auto root = parse_json(r.stdout_text, false);
      args["driver"_key] = str_of(*root, "driver"_key);
      args["product"_key] = str_of(*root, "db_product"_key);
      size_t ti = 0;
      for_each_entry(*root, "tables"_key, [&](const dynamic& t) {
        // Column -> "ref_table(ref_col, ...)" for single foreign-key columns.
        std::map<std::string, std::string> fk_of;
        if (const auto* fkf = t.findField<dynamic_ptr>("fk"_key); fkf && *fkf) {
          for_each_entry(**fkf, "outgoing"_key, [&](const dynamic& fk) {
            const auto cols = strings_of(fk, "columns"_key);
            const auto refs = strings_of(fk, "ref_columns"_key);
            for (const auto& c : cols)
              fk_of[c] = str_of(fk, "ref_table"_key) + "(" + join(refs, ", ") + ")";
          });
        }
        auto te = std::make_shared<dynamic>();
        (*te)["name"_key] = str_of(t, "name"_key);
        (*te)["type"_key] = str_of(t, "table_type"_key);
        (*te)["rows"_key] = int_of(t, "row_count"_key, -1);
        dynamic cols;
        size_t ci = 0;
        for_each_entry(t, "columns"_key, [&](const dynamic& c) {
          auto ce = std::make_shared<dynamic>();
          const std::string name = str_of(c, "name"_key);
          std::string type = str_of(c, "column_type"_key);
          if (type.empty())
            type = str_of(c, "base_type"_key);
          (*ce)["name"_key] = name;
          (*ce)["type"_key] = type;
          (*ce)["pk"_key] = bool_of(c, "primary_key"_key, false);
          (*ce)["nullable"_key] = bool_of(c, "nullable"_key, true);
          auto fk = fk_of.find(name);
          (*ce)["fk"_key] = fk == fk_of.end() ? std::string{} : fk->second;
          cols[ci++] = dynamic_ptr{ce};
        });
        (*te)["columns"_key] = as_array(std::move(cols));
        tables[ti++] = dynamic_ptr{te};
      });
    } catch (const std::exception& ex) {
      args["error"_key] = std::string{"Could not parse `sq inspect` output: "} + ex.what();
    }
  }
  args["tables"_key] = as_array(std::move(tables));
  call(proxy_, "update_schema"_key, std::move(args));
}

void sq_source::refresh_all() {
  if (!drivers_pushed_)
    push_drivers();
  push_connections();
  push_schema();
}

// ── Connections ────────────────────────────────────────────────────────────

void sq_source::on_activate(const std::string& handle) {
  active_ = handle;
  last_sql_.clear();
  last_handle_.clear();
  push_connections();
  push_schema();
}

void sq_source::on_ping(const std::string& handle) {
  auto r = run_logged({"ping", handle});
  report("connections", r.ok(), r.ok() ? handle + " is reachable." : error_text(r));
}

void sq_source::on_remove(const std::string& handle) {
  auto r = run_logged({"rm", handle});
  if (!r.ok()) {
    report("connections", false, error_text(r));
    return;
  }
  if (handle == active_) {
    active_.clear();
    last_sql_.clear();
    last_handle_.clear();
  }
  push_connections();
  push_schema();
  report("connections", true, "Removed " + handle + ".");
}

void sq_source::on_add(
    const std::string& handle, const std::string& location, const std::string& driver, const std::string& password) {
  if (location.empty() || location[0] == '-') {
    report("connections", false, "Enter a location (a connection string or a file path).");
    return;
  }
  std::vector<std::string> args = {"add"};
  if (!handle.empty())
    args.push_back("--handle=" + std::string{handle[0] == '@' ? "" : "@"} + handle);
  if (!driver.empty())
    args.push_back("--driver=" + driver);
  if (!password.empty())
    args.push_back("-p");
  args.push_back("--");
  args.push_back(location);

  // sq reads the password from stdin when it is not a terminal; a trailing
  // newline terminates it.
  auto r = run_logged(args, password.empty() ? std::string{} : password + "\n");
  if (!r.ok()) {
    report("connections", false, error_text(r));
    return;
  }
  // `sq add` makes the new source sq's active one (it may also have chosen
  // the handle); adopt it as this session's active database too.
  active_.clear();
  last_sql_.clear();
  last_handle_.clear();
  push_connections();
  push_schema();
  report("connections", true, "Connection added.");
}

// ── Queries ────────────────────────────────────────────────────────────────

void sq_source::push_result_error(const std::string& message) {
  dynamic args;
  args["ok"_key] = false;
  args["error"_key] = message;
  call(proxy_, "update_result"_key, std::move(args));
}

void sq_source::on_query(const std::string& sql, int32_t max_rows) {
  if (active_.empty()) {
    push_result_error("No active database. Add or select a connection first.");
    return;
  }
  if (auto why = check_read_only_sql(sql)) {
    push_result_error(*why);
    return;
  }

  const auto t0 = std::chrono::steady_clock::now();
  auto r = run_logged({"sql", "--src", active_, "--jsonl", "--", sql});
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  if (!r.ok()) {
    push_result_error(error_text(r));
    return;
  }

  const auto result = parse_jsonl_result(r.stdout_text, static_cast<size_t>(std::max(max_rows, int32_t{1})));
  last_sql_ = sql;
  last_handle_ = active_;

  dynamic cols;
  for (size_t i = 0; i < result.columns.size(); ++i) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = result.columns[i];
    cols[i] = dynamic_ptr{e};
  }
  dynamic rows;
  for (size_t i = 0; i < result.rows.size(); ++i) {
    auto row = std::make_shared<dynamic>();
    for (size_t j = 0; j < result.rows[i].size(); ++j)
      (*row)[j] = result.rows[i][j].is_null ? std::string{"\x01"} : result.rows[i][j].text;
    rows[i] = dynamic_ptr{row};
  }

  dynamic args;
  args["ok"_key] = true;
  args["sql"_key] = sql;
  args["elapsed_ms"_key] = static_cast<int32_t>(elapsed_ms);
  args["total_rows"_key] = static_cast<int32_t>(result.total_rows);
  args["truncated"_key] = result.truncated;
  args["columns"_key] = as_array(std::move(cols));
  args["rows"_key] = as_array(std::move(rows));
  call(proxy_, "update_result"_key, std::move(args));
}

void sq_source::on_export(const std::string& path, bool overwrite) {
  if (last_sql_.empty()) {
    report("results", false, "Run a query first; Export CSV writes the last result.");
    return;
  }
  if (path.empty()) {
    report("results", false, "Enter the CSV file to write.");
    return;
  }

  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path target{path};
  if (fs::is_directory(target, ec)) {
    report("results", false, path + " is a directory.");
    return;
  }
  if (!overwrite && fs::exists(target, ec)) {
    report("results", false, path + " already exists; tick Overwrite to replace it.");
    return;
  }
  const fs::path parent = target.parent_path();
  if (!parent.empty() && !fs::is_directory(parent, ec)) {
    report("results", false, "Directory " + parent.string() + " does not exist.");
    return;
  }

  auto r = run_logged({"sql", "--src", last_handle_, "--csv", "--output", path, "--", last_sql_});
  report("results", r.ok(), r.ok() ? "Exported the full result to " + path : error_text(r));
}

} // namespace bdg::wish::sq
