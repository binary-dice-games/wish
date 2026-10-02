// MIT License © 2026 Binary Dice Games
/// @file helm_source.cpp
/// @brief Implementation of helm_source.
#include "helm_source.hpp"

#include "helm_table_parser.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>

namespace bdg::wish::helm {

using namespace bdg::bison;

namespace {

std::string trim_eol(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    s.pop_back();
  return s;
}

// What a failed command had to say: helm's `Error:` message from stderr, or
// stdout when stderr is empty.
std::string error_text(const process_result& r) {
  return error_summary(r.stderr_text.empty() ? r.stdout_text : r.stderr_text);
}

// Parses @p r's table output (empty when the command failed) and builds the
// dynamic array every update_* method expects, calling @p fill once per row.
dynamic_ptr table_to_array(
    const process_result& r, const std::string& header, size_t ncols,
    const std::function<void(dynamic&, const std::vector<std::string>&)>& fill) {
  dynamic arr;
  size_t i = 0;
  if (r.ok()) {
    for (auto& cols : parse_table(r.stdout_text, header, ncols)) {
      auto e = std::make_shared<dynamic>();
      fill(*e, cols);
      arr[i++] = dynamic_ptr{e};
    }
  }
  return dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
}

} // namespace

helm_source::helm_source(
    std::shared_ptr<bison::rmi::proxy::dynamic> proxy, std::shared_ptr<dev::command_worker> worker)
    : proxy_(std::move(proxy)), worker_(std::move(worker)) {}

void helm_source::refresh_all() {
  push_releases();
  push_repos();
}

// ── helpers ────────────────────────────────────────────────────────────────

process_result helm_source::run_logged(const std::vector<std::string>& args) {
  const std::string command = dev::command_text("helm", args);
  auto r = worker_->run(command, [&](const dev::run_hooks* hooks) { return run_helm_cli(args, "helm", hooks); });

  // Single-line preview: helm's tables are TAB-separated and space-padded,
  // so collapse every whitespace run to one space.
  std::string output;
  for (char ch : r.ok() ? trim_eol(r.stdout_text) : error_text(r)) {
    const bool space = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
    if (!space)
      output += ch;
    else if (!output.empty() && output.back() != ' ')
      output += ' ';
  }
  constexpr size_t kMaxOutputPreview = 200;
  if (output.size() > kMaxOutputPreview)
    output = output.substr(0, kMaxOutputPreview) + "...";

  dynamic log;
  log["command"_key] = command;
  log["exit_code"_key] = r.exit_code;
  log["ok"_key] = r.ok();
  log["output"_key] = std::move(output);
  try {
    proxy_->call("append_command_log"_key, std::move(log)).get();
  } catch (const std::exception&) {
    // Best-effort: a torn-down form just swallows the trace row.
  }
  return r;
}

bool helm_source::report(const std::string& label, const std::string& scope, bool ok, const std::string& output) {
  dynamic args;
  args["command"_key] = label;
  args["scope"_key] = scope;
  args["ok"_key] = ok;
  args["output"_key] = ok ? std::string{} : output;
  if (!ok)
    worker_->fail(label + " failed: " + output);
  try {
    proxy_->call("command_result"_key, std::move(args)).get();
  } catch (const std::exception&) {
    return false; // form gone.
  }
  return true;
}

bool helm_source::run_and_refresh(
    const std::string& label, const std::string& scope, const std::vector<std::string>& args) {
  auto r = run_logged(args);
  // Refresh first: each update_* rewrites its window's status label with a
  // row count, which would otherwise overwrite this command's outcome.
  refresh_all();
  report(label, scope, r.ok(), error_text(r));
  return r.ok();
}

bool helm_source::check_args(
    const std::string& label, const std::string& scope, const std::vector<std::string>& values) {
  for (auto& v : values) {
    if (!is_safe_arg(v)) {
      report(label, scope, false, v.empty() ? "a required value is empty" : "invalid value '" + v + "'");
      return false;
    }
  }
  return true;
}

// ── snapshots ──────────────────────────────────────────────────────────────

void helm_source::push_releases() {
  // -A: every namespace (namespace is a column + a client-side filter, the
  // kubectl module's model). The per-status flags ask for every status, not
  // just deployed / failed; helm 3's `-a` shorthand for that was removed in
  // helm 4, while these flags exist in both.
  auto r = run_logged(
      {"list", "-A", "--deployed", "--failed", "--pending", "--uninstalling", "--uninstalled", "--superseded"});

  dynamic args;
  args["releases"_key] =
      table_to_array(r, "NAME", 7, [](dynamic& e, const std::vector<std::string>& c) {
        e["name"_key] = c[0];
        e["namespace"_key] = c[1];
        e["revision"_key] = c[2];
        e["updated"_key] = short_timestamp(c[3]);
        e["status"_key] = c[4];
        e["chart"_key] = c[5];
        e["app_version"_key] = c[6];
      });
  try {
    proxy_->call("update_releases"_key, std::move(args)).get();
  } catch (const std::exception&) {
    return;
  }
  if (!r.ok())
    report("list", "releases", false, error_text(r));
}

void helm_source::push_repos() {
  auto r = run_logged({"repo", "list"});

  dynamic args;
  args["repos"_key] = table_to_array(r, "NAME", 2, [](dynamic& e, const std::vector<std::string>& c) {
    e["name"_key] = c[0];
    e["url"_key] = c[1];
  });
  try {
    proxy_->call("update_repos"_key, std::move(args)).get();
  } catch (const std::exception&) {
    return;
  }
  // `helm repo list` exits non-zero with "no repositories to show" when none
  // are configured -- an empty table, not a failure worth reporting.
  if (!r.ok() && error_text(r).find("no repositories") == std::string::npos)
    report("repo list", "repos", false, error_text(r));
}

// ── mutating actions ───────────────────────────────────────────────────────

void helm_source::on_release_action(
    const std::string& name, const std::string& ns, const std::string& action, const std::string& revision) {
  if (action == "uninstall") {
    if (check_args("uninstall " + name, "releases", {name, ns}))
      run_and_refresh("uninstall " + name, "releases", {"uninstall", name, "-n", ns});
    return;
  }
  if (action != "rollback")
    return;

  const std::string label = "rollback " + name + (revision.empty() ? std::string{} : " to revision " + revision);
  std::vector<std::string> checked = {name, ns};
  std::vector<std::string> argv = {"rollback", name};
  if (!revision.empty()) {
    checked.push_back(revision);
    argv.push_back(revision);
  }
  argv.push_back("-n");
  argv.push_back(ns);
  if (!check_args(label, "releases", checked))
    return;
  run_and_refresh(label, "releases", argv);
  // A rollback appends a revision; the form drops this if the History window
  // is showing some other release.
  on_history_requested(name, ns);
}

void helm_source::on_repo_action(const std::string& name, const std::string& action) {
  bool ran = false;
  if (action == "update") {
    if (name.empty()) {
      ran = run_and_refresh("repo update", "repos", {"repo", "update"});
    } else if (check_args("repo update " + name, "repos", {name})) {
      ran = run_and_refresh("repo update " + name, "repos", {"repo", "update", name});
    }
  } else if (action == "remove") {
    if (check_args("repo remove " + name, "repos", {name}))
      ran = run_and_refresh("repo remove " + name, "repos", {"repo", "remove", name});
  }
  // The set of available charts changed: redo the Charts window's search.
  if (ran)
    on_search_requested(last_query_);
}

void helm_source::on_repo_add(const std::string& name, const std::string& url) {
  if (check_args("repo add " + name, "repos", {name, url}) &&
      run_and_refresh("repo add " + name, "repos", {"repo", "add", name, url}))
    on_search_requested(last_query_);
}

void helm_source::on_install_requested(const install_request& req) {
  const std::string label = std::string{req.upgrade ? "upgrade " : "install "} + req.release;
  std::vector<std::string> checked = {req.release, req.chart, req.ns};
  std::vector<std::string> argv = {req.upgrade ? "upgrade" : "install", req.release, req.chart, "-n", req.ns};
  if (!req.version.empty()) {
    checked.push_back(req.version);
    argv.push_back("--version");
    argv.push_back(req.version);
  }
  if (req.create_namespace && !req.upgrade)
    argv.push_back("--create-namespace");
  if (req.wait)
    argv.push_back("--wait");
  if (!check_args(label, "install", checked))
    return;
  if (!req.upgrade && !is_valid_release_name(req.release)) {
    report(label, "install", false, "invalid release name '" + req.release + "'");
    return;
  }

  // helm takes values as a file. Create it empty, restrict it to the owner
  // (values routinely hold passwords), then write.
  std::filesystem::path values_file;
  if (req.values.find_first_not_of(" \t\r\n") != std::string::npos) {
    std::error_code ec;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    values_file = std::filesystem::temp_directory_path(ec) / ("wish_helm_values_" + std::to_string(stamp) + ".yaml");
    bool written = false;
    if (!ec && std::ofstream(values_file, std::ios::binary).good()) {
      std::filesystem::permissions(
          values_file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, ec);
      std::ofstream out(values_file, std::ios::binary | std::ios::trunc);
      out.write(req.values.data(), static_cast<std::streamsize>(req.values.size()));
      written = out.good();
    }
    if (!written) {
      std::filesystem::remove(values_file, ec);
      report(label, "install", false, "cannot write the values to a temporary file");
      return;
    }
    argv.push_back("-f");
    argv.push_back(values_file.string());
  }

  run_and_refresh(label, "install", argv);

  if (!values_file.empty()) {
    std::error_code ec;
    std::filesystem::remove(values_file, ec);
  }
}

void helm_source::on_install_values_requested(
    int32_t token, const std::string& source, const std::string& chart, const std::string& version,
    const std::string& name, const std::string& ns) {
  std::vector<std::string> argv;
  bool safe = false;
  if (source == "chart") {
    argv = {"show", "values", chart};
    safe = is_safe_arg(chart);
    if (!version.empty()) {
      argv.push_back("--version");
      argv.push_back(version);
      safe = safe && is_safe_arg(version);
    }
  } else if (source == "release") {
    argv = {"get", "values", name, "-n", ns, "-o", "yaml"};
    safe = is_safe_arg(name) && is_safe_arg(ns);
  } else {
    return;
  }

  dynamic args;
  args["token"_key] = token;
  if (!safe) {
    args["error"_key] = std::string{"invalid chart or release reference"};
  } else {
    auto r = run_logged(argv);
    if (!r.ok()) {
      args["error"_key] = error_text(r);
    } else {
      // A release installed without custom values prints a bare "null".
      std::string text = r.stdout_text;
      if (trim_eol(text) == "null")
        text.clear();
      args["text"_key] = std::move(text);
    }
  }
  try {
    proxy_->call("set_install_values"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
}

// ── read-only queries ──────────────────────────────────────────────────────

void helm_source::on_search_requested(const std::string& query) {
  last_query_ = query;
  // --max-col-width: the default (50) truncates the description column.
  std::vector<std::string> argv = {"search", "repo", "--max-col-width", "200"};
  if (!query.empty()) {
    if (!check_args("search " + query, "charts", {query}))
      return;
    argv.push_back(query);
  }
  auto r = run_logged(argv);

  dynamic args;
  args["query"_key] = query;
  args["charts"_key] = table_to_array(r, "NAME", 4, [](dynamic& e, const std::vector<std::string>& c) {
    e["name"_key] = c[0];
    e["version"_key] = c[1];
    e["app_version"_key] = c[2];
    e["description"_key] = c[3];
  });
  try {
    proxy_->call("update_charts"_key, std::move(args)).get();
  } catch (const std::exception&) {
    return;
  }
  // With no repository configured helm fails with "no repositories
  // configured" -- an empty result (the form explains it), not an error.
  if (!r.ok() && error_text(r).find("no repositories") == std::string::npos)
    report("search", "charts", false, error_text(r));
}

void helm_source::on_history_requested(const std::string& name, const std::string& ns) {
  if (!check_args("history " + name, "history", {name, ns}))
    return;
  auto r = run_logged({"history", name, "-n", ns});

  dynamic args;
  args["name"_key] = name;
  args["namespace"_key] = ns;
  args["revisions"_key] = table_to_array(r, "REVISION", 6, [](dynamic& e, const std::vector<std::string>& c) {
    e["revision"_key] = c[0];
    e["updated"_key] = c[1];
    e["status"_key] = c[2];
    e["chart"_key] = c[3];
    e["description"_key] = c[5]; // c[4], APP VERSION, is not shown.
  });
  try {
    proxy_->call("update_history"_key, std::move(args)).get();
  } catch (const std::exception&) {
    return;
  }
  if (!r.ok())
    report("history " + name, "history", false, error_text(r));
}

void helm_source::on_details_requested(
    const std::string& kind, const std::string& name, const std::string& ns, const std::string& version) {
  const bool chart = kind == "chart_values" || kind == "chart_readme";
  std::vector<std::string> argv;
  std::string title;
  std::string invalid;

  if (chart) {
    argv = {"show", kind == "chart_values" ? "values" : "readme", name};
    if (!version.empty()) {
      argv.push_back("--version");
      argv.push_back(version);
    }
    title = (kind == "chart_values" ? "values: " : "readme: ") + name + (version.empty() ? "" : " " + version);
    if (!is_safe_arg(name) || (!version.empty() && !is_safe_arg(version)))
      invalid = "invalid chart reference";
  } else {
    if (kind == "status")
      argv = {"status", name, "-n", ns};
    else if (kind == "values" || kind == "manifest" || kind == "notes")
      argv = {"get", kind, name, "-n", ns};
    else
      return;
    title = kind + ": " + ns + "/" + name;
    if (!is_safe_arg(name) || !is_safe_arg(ns))
      invalid = "invalid release reference";
  }

  std::string text = invalid;
  if (invalid.empty()) {
    auto r = run_logged(argv);
    text = r.ok() ? r.stdout_text : error_text(r);
  }

  dynamic args;
  args["kind"_key] = kind;
  args["name"_key] = name;
  args["namespace"_key] = ns;
  args["title"_key] = title;
  args["text"_key] = std::move(text);
  try {
    proxy_->call("update_details"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
}

} // namespace bdg::wish::helm
