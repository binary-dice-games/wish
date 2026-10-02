// MIT License © 2026 Binary Dice Games
/// @file pip_source.cpp
/// @brief Implementation of pip_source.
#include "pip_source.hpp"

#include "pip_parsers.hpp"

#include <chrono>
#include <filesystem>
#include <thread>

namespace bdg::wish::pip {

using namespace bdg::bison;

namespace {

std::string trim_eol(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    s.pop_back();
  return s;
}

// What a failed command had to say: pip's `ERROR:` message from stderr, or
// stdout when stderr is empty.
std::string error_text(const process_result& r) {
  return error_summary(r.stderr_text.empty() ? r.stdout_text : r.stderr_text);
}

std::string field_of(const json_object& obj, const char* key) {
  auto it = obj.find(key);
  return it == obj.end() ? std::string{} : it->second;
}

} // namespace

std::string resolve_interpreter(const std::string& arg) {
  if (arg.empty()) {
    // `python3` is the unambiguous name on Linux / MSYS2; python.org's
    // Windows installer only provides `python`.
    return run_pip_cli({"--version"}, {"python3"}).ok() ? "python3" : "python";
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

pip_source::pip_source(std::shared_ptr<bison::rmi::proxy::dynamic> proxy, const std::string& interpreter)
    : proxy_(std::move(proxy)), interpreter_(interpreter),
      // --no-input: stdin is closed, so never wait on a prompt. The version
      // check would add an unrelated "new release of pip" notice to stderr.
      launcher_({interpreter, "-m", "pip", "--disable-pip-version-check", "--no-input", "--no-color"}) {}

std::string pip_source::probe_version(std::string& error) const {
  auto r = run_pip_cli({"--version"}, launcher_);
  if (!r.ok()) {
    error = error_text(r);
    return {};
  }
  return trim_eol(r.stdout_text);
}

void pip_source::push_environment(const std::string& version_text) {
  dynamic args;
  args["text"_key] = version_text;
  args["interpreter"_key] = interpreter_;
  try {
    proxy_->call("set_environment"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
}

// ── worker thread ──────────────────────────────────────────────────────────

void pip_source::start() {
  // Detached, holding a reference to this object: shutdown() is called from
  // an event handler, which must not block joining a thread that may itself
  // be waiting on an RMI reply.
  std::thread([self = shared_from_this()] { self->work(); }).detach();
}

void pip_source::post(std::function<void()> job) {
  queue_.wlock()->jobs.push_back(std::move(job));
  queue_.notify_one();
}

void pip_source::cancel() {
  cancel_ = true;
  queue_.wlock()->jobs.clear();
}

void pip_source::shutdown() {
  cancel_ = true;
  {
    auto q = queue_.wlock();
    q->jobs.clear();
    q->stop = true;
  }
  queue_.notify_one();
}

void pip_source::work() {
  while (true) {
    std::function<void()> job;
    queue_.wait([&](work_queue& q) {
      if (q.stop)
        return true;
      if (q.jobs.empty())
        return false;
      job = std::move(q.jobs.front());
      q.jobs.pop_front();
      return true;
    });
    if (!job)
      return; // stopped.
    cancel_ = false;
    job();
    if (progress_shown_ && queue_.rlock()->jobs.empty()) {
      push_progress(false, {}, 0.0f, {});
      progress_shown_ = false;
    }
  }
}

void pip_source::push_progress(
    bool active, const std::string& command, float phase, const std::vector<std::string>& lines) {
  dynamic arr;
  size_t i = 0;
  for (auto& line : lines)
    arr[i++] = line;

  dynamic args;
  args["active"_key] = active;
  args["command"_key] = command;
  args["phase"_key] = phase;
  args["lines"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  try {
    proxy_->call("set_progress"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
}

void pip_source::refresh_all() {
  push_packages();
}

// ── helpers ────────────────────────────────────────────────────────────────

process_result pip_source::run_logged(const std::vector<std::string>& args) {
  std::string command = "pip";
  for (auto& a : args)
    command += ' ' + a;

  // Progress dialog: opened once this command has run for kProgressDelay
  // (or at once when an earlier command of this busy period opened it),
  // then fed pip's output lines ("Collecting ...", "Downloading ...") on
  // every tick. The bar is indeterminate, animated by the elapsed time.
  constexpr float kProgressDelay = 0.4f; // seconds
  const auto started = std::chrono::steady_clock::now();
  std::string pending;            // output not yet terminated by a newline
  std::vector<std::string> lines; // complete lines not yet pushed
  auto flush = [&] {
    const std::chrono::duration<float> elapsed = std::chrono::steady_clock::now() - started;
    if (!progress_shown_ && elapsed.count() < kProgressDelay)
      return;
    progress_shown_ = true;
    push_progress(true, command, elapsed.count(), lines);
    lines.clear();
  };
  flush();

  run_hooks hooks;
  hooks.on_output = [&](const std::string& chunk) {
    pending += chunk;
    size_t eol;
    while ((eol = pending.find('\n')) != std::string::npos) {
      std::string line = trim_eol(pending.substr(0, eol));
      pending.erase(0, eol + 1);
      const size_t text = line.find_first_not_of(" \t");
      // JSON snapshots (pip list) are data, not progress.
      if (text != std::string::npos && line[text] != '[' && line[text] != '{')
        lines.push_back(std::move(line));
    }
  };
  hooks.on_tick = [&] {
    flush();
    return !cancel_.load();
  };

  auto r = run_pip_cli(args, launcher_, &hooks);
  if (progress_shown_)
    flush(); // the lines since the last tick.
  // Consume the request: the rest of the job (the refresh after a cancelled
  // install, say) must still run.
  if (cancel_.exchange(false) && !r.ok())
    r.stderr_text = "cancelled";

  // Single-line preview: collapse every whitespace run to one space.
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

bool pip_source::report(const std::string& label, const std::string& scope, bool ok, const std::string& output) {
  dynamic args;
  args["command"_key] = label;
  args["scope"_key] = scope;
  args["ok"_key] = ok;
  args["output"_key] = ok ? std::string{} : output;
  try {
    proxy_->call("command_result"_key, std::move(args)).get();
  } catch (const std::exception&) {
    return false; // form gone.
  }
  return true;
}

bool pip_source::run_and_refresh(const std::string& label, const std::vector<std::string>& args) {
  auto r = run_logged(args);
  // Refresh first: update_packages rewrites the status label with a package
  // count, which would otherwise overwrite this command's outcome.
  refresh_all();
  report(label, "packages", r.ok(), error_text(r));
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
  try {
    proxy_->call("update_packages"_key, std::move(args)).get();
  } catch (const std::exception&) {
    return;
  }
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
  try {
    proxy_->call("update_versions"_key, std::move(args)).get();
  } catch (const std::exception&) {
    return;
  }
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
  try {
    proxy_->call("update_details"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
}

} // namespace bdg::wish::pip
