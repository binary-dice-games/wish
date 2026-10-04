// MIT License © 2026 Binary Dice Games
/// @file pkg_source.cpp
/// @brief Implementation of pkg_source.
#include "pkg_source.hpp"

namespace bdg::wish::pkg {

using namespace bdg::bison;

namespace {

dynamic_ptr to_array(const std::vector<package>& packages, const std::map<std::string, std::string>* latest) {
  dynamic arr;
  size_t i = 0;
  for (auto& p : packages) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = p.name;
    (*e)["version"_key] = p.version;
    (*e)["description"_key] = p.description;
    if (latest) {
      auto it = latest->find(base_name(p.name));
      (*e)["latest"_key] = it == latest->end() ? std::string{} : it->second;
    }
    arr[i++] = dynamic_ptr{e};
  }
  return dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
}

} // namespace

pkg_source::pkg_source(
    std::shared_ptr<bison::rmi::proxy::dynamic> proxy, manager m, elevation how,
    std::shared_ptr<common::command_worker> worker)
    // No launcher: every command is a whole argv (the program differs per
    // manager, and a privileged command is wrapped in sudo / pkexec).
    : tool_source(std::move(proxy), std::move(worker), {}, {}), manager_(m), elevation_(how) {}

void pkg_source::push_environment(const std::string& version_text) {
  dynamic args;
  args["manager"_key] = std::string{manager_name(manager_)};
  args["text"_key] = version_text;
  args["elevation"_key] = std::string{elevation_name(elevation_)};
  call("set_environment"_key, std::move(args));
}

void pkg_source::refresh_all() {
  push_packages();
}

// ── helpers ────────────────────────────────────────────────────────────────

std::string pkg_source::error_text(const process_result& r) const {
  return error_summary(elevation_, common::error_output(r));
}

process_result pkg_source::run_logged(const command& cmd) {
  return tool_source::run_logged(elevated(manager_, cmd, elevation_));
}

void pkg_source::run_and_refresh(const std::string& label, const command& cmd) {
  auto r = run_logged(cmd);
  // Refresh first: update_packages rewrites the status label with a package
  // count, which would otherwise overwrite this command's outcome.
  refresh_all();
  // Something the user asked for: its failure (a refused privilege request,
  // most often) is shown in the dialog even when it failed at once.
  report(label, "packages", r.ok(), error_text(r), /*always_show=*/true);
}

// ── snapshots ──────────────────────────────────────────────────────────────

void pkg_source::push_packages() {
  auto r = run_logged(list_command(manager_));

  dynamic args;
  args["packages"_key] = to_array(r.ok() ? parse_installed(manager_, r.stdout_text) : std::vector<package>{}, &latest_);
  args["outdated_checked"_key] = outdated_checked_;
  if (!call("update_packages"_key, std::move(args)))
    return;
  if (!r.ok())
    report("list", "packages", false, error_text(r));
}

void pkg_source::on_outdated_requested() {
  auto r = run_logged(outdated_command(manager_));
  const bool ok = outdated_succeeded(manager_, r.exit_code);
  if (ok) {
    latest_.clear();
    for (auto& p : parse_outdated(manager_, r.stdout_text))
      latest_[base_name(p.name)] = p.latest;
    outdated_checked_ = true;
  }
  push_packages();
  if (!ok)
    report("check for updates", "packages", false, error_text(r));
}

void pkg_source::on_index_requested() {
  auto r = run_logged(refresh_index_command(manager_));
  if (!r.ok()) {
    report("update index", "packages", false, error_text(r), /*always_show=*/true);
    return;
  }
  on_outdated_requested();
  report("update index", "packages", true, {});
}

void pkg_source::on_upgrade_all_requested() {
  run_and_refresh("upgrade all", upgrade_all_command(manager_));
}

// ── mutating actions ───────────────────────────────────────────────────────

void pkg_source::on_install_requested(const std::string& text) {
  const auto names = split_names(text);
  const std::string label = "install " + common::one_line(text, 80);
  if (names.empty()) {
    report("install", "packages", false, "nothing to install");
    return;
  }
  for (auto& name : names) {
    if (!is_valid_package_name(name)) {
      report(label, "packages", false, "invalid package name '" + name + "'");
      return;
    }
  }
  run_and_refresh(label, install_command(manager_, names));
}

void pkg_source::on_package_action(const std::string& name, const std::string& action) {
  const std::string label = action + " " + name;
  if (!is_valid_package_name(name)) {
    report(label, "packages", false, "invalid package name '" + name + "'");
    return;
  }
  if (action == "upgrade")
    run_and_refresh(label, upgrade_command(manager_, name));
  else if (action == "reinstall")
    run_and_refresh(label, reinstall_command(manager_, name));
  else if (action == "remove")
    run_and_refresh(label, remove_command(manager_, name));
}

// ── read-only queries ──────────────────────────────────────────────────────

void pkg_source::on_search_requested(const std::string& query) {
  const std::string label = "search " + query;
  if (!is_safe_arg(query)) {
    report(label, "search", false, query.empty() ? "nothing to search for" : "invalid search text '" + query + "'");
    return;
  }
  auto r = run_logged(search_command(manager_, query));

  dynamic args;
  args["query"_key] = query;
  args["results"_key] = to_array(r.ok() ? parse_search(manager_, r.stdout_text) : std::vector<package>{}, nullptr);
  if (!call("update_search"_key, std::move(args)))
    return;
  // No match is an empty result (some managers exit non-zero for it), not an
  // error worth reporting -- unless the manager said something.
  if (!r.ok() && !r.stderr_text.empty())
    report(label, "search", false, error_text(r));
}

void pkg_source::on_details_requested(const std::string& kind, const std::string& name) {
  if (kind != "show" && kind != "files")
    return;

  std::string text;
  if (!is_valid_package_name(name)) {
    text = "invalid package name '" + name + "'";
  } else {
    const std::vector<command> commands =
        kind == "show" ? show_commands(manager_, name) : std::vector<command>{files_command(manager_, name)};
    for (auto& cmd : commands) {
      auto r = run_logged(cmd);
      text = r.ok() ? r.stdout_text : error_text(r);
      if (r.ok())
        break;
    }
  }

  dynamic args;
  args["kind"_key] = kind;
  args["name"_key] = name;
  args["title"_key] = kind + ": " + name;
  args["text"_key] = std::move(text);
  call("update_details"_key, std::move(args));
}

} // namespace bdg::wish::pkg
