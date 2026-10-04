// MIT License © 2026 Binary Dice Games
/// @file pip_source.cpp
/// @brief Implementation of pip_source.
#include "pip_source.hpp"

#include "pip_parsers.hpp"

#include <filesystem>

namespace bdg::wish::pip {

using namespace bdg::bison;

namespace {

std::string field_of(const json_object& obj, const char* key) {
  auto it = obj.find(key);
  return it == obj.end() ? std::string{} : it->second;
}

} // namespace

std::string resolve_interpreter(const std::string& arg) {
  if (arg.empty()) {
    // `python3` is the unambiguous name on Linux / MSYS2; python.org's
    // Windows installer only provides `python`.
    return dev::run_process({"python3", "--version"}).ok() ? "python3" : "python";
  }
  std::error_code ec;
  const std::filesystem::path p{arg};
  if (!std::filesystem::is_directory(p, ec))
    return arg;
  for (const char* rel : {"bin/python", "bin/python3", "Scripts/python.exe"}) {
    if (std::filesystem::exists(p / rel, ec))
      return (p / rel).string();
  }
  return {};
}

pip_source::pip_source(
    std::shared_ptr<bison::rmi::proxy::dynamic> proxy, const std::string& interpreter,
    std::shared_ptr<common::command_worker> worker)
    : tool_source(
          std::move(proxy), std::move(worker), "pip",
          // --no-input: stdin is closed, so never wait on a prompt. The
          // version check would add an unrelated "new release of pip" notice
          // to stderr.
          {interpreter, "-m", "pip", "--disable-pip-version-check", "--no-input", "--no-color"}),
      interpreter_(interpreter) {}

std::string pip_source::probe_version(std::string& error) const {
  auto r = run({"--version"});
  if (!r.ok()) {
    error = error_text(r);
    return {};
  }
  return dev::trim_eol(r.stdout_text);
}

void pip_source::push_environment(const std::string& version_text) {
  dynamic args;
  args["text"_key] = version_text;
  args["interpreter"_key] = interpreter_;
  call("set_environment"_key, std::move(args));
}

void pip_source::refresh_all() {
  push_packages();
}

// ── helpers ────────────────────────────────────────────────────────────────

std::string pip_source::error_text(const process_result& r) const {
  return error_summary(dev::error_output(r));
}

bool pip_source::run_and_refresh(const std::string& label, const std::vector<std::string>& args) {
  auto r = run_logged(args);
  // Refresh first: update_packages rewrites the status label with a package
  // count, which would otherwise overwrite this command's outcome.
  refresh_all();
  // Something the user asked for: show its failure in the dialog even when
  // it failed at once (an externally-managed environment, typically).
  report(label, "packages", r.ok(), error_text(r), /*always_show=*/true);
  return r.ok();
}

std::vector<std::string> pip_source::install_argv(const install_options& opts) {
  // pip draws no download bar without a terminal anyway; make that explicit.
  std::vector<std::string> argv = {"install", "--progress-bar", "off"};
  if (opts.upgrade)
    argv.push_back("--upgrade");
  if (opts.user)
    argv.push_back("--user");
  if (opts.pre)
    argv.push_back("--pre");
  return argv;
}

// ── snapshots ──────────────────────────────────────────────────────────────

void pip_source::push_packages() {
  auto r = run_logged({"list", "--format=json"});

  dynamic arr;
  size_t i = 0;
  if (r.ok()) {
    for (auto& obj : parse_json_objects(r.stdout_text)) {
      auto e = std::make_shared<dynamic>();
      const std::string name = field_of(obj, "name");
      auto latest = latest_.find(name);
      (*e)["name"_key] = name;
      (*e)["version"_key] = field_of(obj, "version");
      (*e)["latest"_key] = latest == latest_.end() ? std::string{} : latest->second;
      (*e)["location"_key] = field_of(obj, "editable_project_location");
      arr[i++] = dynamic_ptr{e};
    }
  }

  dynamic args;
  args["packages"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  args["outdated_checked"_key] = outdated_checked_;
  if (!call("update_packages"_key, std::move(args)))
    return;
  if (!r.ok())
    report("list", "packages", false, error_text(r));
}

void pip_source::on_outdated_requested() {
  auto r = run_logged({"list", "--outdated", "--format=json"});
  if (r.ok()) {
    latest_.clear();
    for (auto& obj : parse_json_objects(r.stdout_text))
      latest_[field_of(obj, "name")] = field_of(obj, "latest_version");
    outdated_checked_ = true;
  }
  push_packages();
  if (!r.ok())
    report("list --outdated", "packages", false, error_text(r));
}

// ── mutating actions ───────────────────────────────────────────────────────

void pip_source::on_install_requested(const std::string& spec, const install_options& opts) {
  const auto requirements = split_requirements(spec);
  const std::string label = "install " + spec;
  if (requirements.empty()) {
    report("install", "packages", false, "nothing to install");
    return;
  }
  auto argv = install_argv(opts);
  for (auto& req : requirements) {
    if (!is_safe_arg(req)) {
      report(label, "packages", false, "invalid requirement '" + req + "'");
      return;
    }
    argv.push_back(req);
  }
  run_and_refresh(label, argv);
}

void pip_source::on_requirements_requested(const std::string& path, const install_options& opts) {
  const std::string label = "install -r " + path;
  if (!is_safe_arg(path)) {
    report(label, "packages", false, path.empty() ? "no requirements file given" : "invalid path '" + path + "'");
    return;
  }
  auto argv = install_argv(opts);
  argv.push_back("-r");
  argv.push_back(path);
  run_and_refresh(label, argv);
}

void pip_source::on_package_action(const std::string& name, const std::string& action) {
  std::vector<std::string> argv;
  if (action == "upgrade")
    argv = {"install", "--progress-bar", "off", "--upgrade", name};
  else if (action == "reinstall")
    argv = {"install", "--progress-bar", "off", "--force-reinstall", "--no-deps", name};
  else if (action == "uninstall")
    argv = {"uninstall", "-y", name};
  else
    return;

  const std::string label = action + " " + name;
  if (!is_valid_package_name(name)) {
    report(label, "packages", false, "invalid package name '" + name + "'");
    return;
  }
  run_and_refresh(label, argv);
}

// ── read-only queries ──────────────────────────────────────────────────────

void pip_source::on_versions_requested(const std::string& name, bool pre) {
  const std::string label = "index versions " + name;
  if (!is_valid_package_name(name)) {
    report(label, "versions", false, "invalid package name '" + name + "'");
    return;
  }
  std::vector<std::string> argv = {"index", "versions", name};
  if (pre)
    argv.push_back("--pre");
  auto r = run_logged(argv);
  const auto parsed = r.ok() ? parse_index_versions(r.stdout_text) : index_versions{};

  dynamic arr;
  size_t i = 0;
  for (auto& v : parsed.versions)
    arr[i++] = v;

  dynamic args;
  args["name"_key] = name;
  // pip only prints LATEST for an installed package; the list is newest first.
  args["latest"_key] =
      !parsed.latest.empty() || parsed.versions.empty() ? parsed.latest : parsed.versions.front();
  args["versions"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  if (!call("update_versions"_key, std::move(args)))
    return;
  if (!r.ok())
    report(label, "versions", false, error_text(r));
}

void pip_source::on_details_requested(const std::string& kind, const std::string& name) {
  std::vector<std::string> argv;
  std::string title;
  const bool per_package = kind == "show" || kind == "files";

  if (kind == "show")
    argv = {"show", name};
  else if (kind == "files")
    argv = {"show", "-f", name};
  else if (kind == "freeze" || kind == "check")
    argv = {kind};
  else
    return;
  title = per_package ? kind + ": " + name : "pip " + kind;

  std::string text;
  if (per_package && !is_valid_package_name(name)) {
    text = "invalid package name '" + name + "'";
  } else {
    auto r = run_logged(argv);
    // `pip check` reports broken requirements on stdout *and* exits non-zero,
    // so keep stdout and append whatever a failure wrote to stderr.
    text = r.stdout_text;
    if (!r.ok() && !r.stderr_text.empty())
      text += error_summary(r.stderr_text) + "\n";
  }

  dynamic args;
  args["kind"_key] = kind;
  args["name"_key] = name;
  args["title"_key] = title;
  args["text"_key] = std::move(text);
  call("update_details"_key, std::move(args));
}

} // namespace bdg::wish::pip
