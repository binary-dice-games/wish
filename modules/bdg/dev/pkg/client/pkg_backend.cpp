// MIT License © 2026 Binary Dice Games
/// @file pkg_backend.cpp
/// @brief Per-manager commands and output parsers for the pkg module.
#include "pkg_backend.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace bdg::wish::pkg {

namespace {

std::string trim(const std::string& s) {
  const char* ws = " \t\r\n";
  auto b = s.find_first_not_of(ws);
  if (b == std::string::npos)
    return {};
  auto e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream iss(text);
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    out.push_back(line);
  }
  return out;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(sep, start);
    if (pos == std::string::npos) {
      out.push_back(s.substr(start));
      return out;
    }
    out.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
}

std::vector<std::string> words(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string w;
  while (iss >> w)
    out.push_back(w);
  return out;
}

bool starts_with(const std::string& s, const char* prefix) {
  return s.rfind(prefix, 0) == 0;
}

// dnf names packages "name.arch": "python3.12.x86_64" -> "python3.12".
std::string strip_arch(const std::string& name) {
  const size_t dot = name.rfind('.');
  return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

command with_args(std::vector<std::string> argv, const std::vector<std::string>& more, bool needs_root) {
  argv.insert(argv.end(), more.begin(), more.end());
  return {std::move(argv), needs_root};
}

} // namespace

// ── Managers ───────────────────────────────────────────────────────────────

const std::vector<manager>& all_managers() {
  static const std::vector<manager> all = {manager::apt, manager::dnf, manager::pacman, manager::brew};
  return all;
}

const char* manager_name(manager m) {
  switch (m) {
  case manager::apt:
    return "apt";
  case manager::dnf:
    return "dnf";
  case manager::pacman:
    return "pacman";
  case manager::brew:
    return "brew";
  }
  return "";
}

std::optional<manager> manager_from_name(const std::string& name) {
  for (manager m : all_managers()) {
    if (name == manager_name(m))
      return m;
  }
  return std::nullopt;
}

std::string manager_names() {
  std::string out;
  for (manager m : all_managers())
    out += (out.empty() ? "" : ", ") + std::string{manager_name(m)};
  return out;
}

std::optional<elevation> elevation_from_name(const std::string& name) {
  if (name == "none")
    return elevation::none;
  if (name == "sudo")
    return elevation::sudo;
  if (name == "pkexec")
    return elevation::pkexec;
  return std::nullopt;
}

const char* elevation_name(elevation e) {
  switch (e) {
  case elevation::none:
    return "none";
  case elevation::sudo:
    return "sudo";
  case elevation::pkexec:
    return "pkexec";
  }
  return "";
}

// ── Commands ───────────────────────────────────────────────────────────────

command probe_command(manager m) {
  switch (m) {
  case manager::apt:
    return {{"apt-get", "--version"}};
  case manager::dnf:
    return {{"dnf", "--version"}};
  case manager::pacman:
    return {{"pacman", "--version"}};
  case manager::brew:
    return {{"brew", "--version"}};
  }
  return {};
}

command list_command(manager m) {
  switch (m) {
  case manager::apt:
    // TAB-separated, one line per package known to dpkg; parse_installed()
    // keeps the installed ones ("ii"). binary:Package carries the
    // architecture qualifier dpkg needs for a multi-arch package.
    return {{"dpkg-query", "-W", "-f", "${binary:Package}\t${Version}\t${db:Status-Abbrev}\t${binary:Summary}\n"}};
  case manager::dnf:
    return {{"rpm", "-qa", "--qf", "%{NAME}\t%{VERSION}-%{RELEASE}\t%{SUMMARY}\n"}};
  case manager::pacman:
    return {{"pacman", "-Q"}};
  case manager::brew:
    return {{"brew", "list", "--versions"}};
  }
  return {};
}

command outdated_command(manager m) {
  switch (m) {
  case manager::apt:
    return {{"apt", "list", "--upgradable"}};
  case manager::dnf:
    return {{"dnf", "-q", "check-update"}};
  case manager::pacman:
    return {{"pacman", "-Qu"}};
  case manager::brew:
    return {{"brew", "outdated", "--verbose"}};
  }
  return {};
}

bool outdated_succeeded(manager m, int exit_code) {
  if (exit_code == 0)
    return true;
  if (m == manager::dnf)
    return exit_code == 100; // updates are available.
  if (m == manager::pacman)
    return exit_code == 1; // nothing to update.
  return false;
}

command search_command(manager m, const std::string& query) {
  switch (m) {
  case manager::apt:
    return {{"apt-cache", "search", "--", query}};
  case manager::dnf:
    return {{"dnf", "-q", "search", "--", query}};
  case manager::pacman:
    return {{"pacman", "-Ss", "--", query}};
  case manager::brew:
    return {{"brew", "search", query}};
  }
  return {};
}

std::vector<command> show_commands(manager m, const std::string& name) {
  switch (m) {
  case manager::apt:
    return {{{"apt-cache", "show", "--", name}}};
  case manager::dnf:
    return {{{"dnf", "-q", "info", "--", name}}};
  case manager::pacman:
    // -Qi knows an installed package (with its install date, size on disk);
    // -Si one that is only in the repositories.
    return {{{"pacman", "-Qi", "--", name}}, {{"pacman", "-Si", "--", name}}};
  case manager::brew:
    return {{{"brew", "info", name}}};
  }
  return {};
}

command files_command(manager m, const std::string& name) {
  switch (m) {
  case manager::apt:
    return {{"dpkg", "-L", "--", name}};
  case manager::dnf:
    return {{"rpm", "-ql", "--", name}};
  case manager::pacman:
    return {{"pacman", "-Ql", "--", name}};
  case manager::brew:
    return {{"brew", "list", "--verbose", name}};
  }
  return {};
}

command install_command(manager m, const std::vector<std::string>& names) {
  switch (m) {
  case manager::apt:
    return with_args({"apt-get", "install", "-y", "--"}, names, true);
  case manager::dnf:
    return with_args({"dnf", "install", "-y", "--"}, names, true);
  case manager::pacman:
    return with_args({"pacman", "-S", "--noconfirm", "--needed", "--"}, names, true);
  case manager::brew:
    return with_args({"brew", "install"}, names, false);
  }
  return {};
}

command remove_command(manager m, const std::string& name) {
  switch (m) {
  case manager::apt:
    return {{"apt-get", "remove", "-y", "--", name}, true};
  case manager::dnf:
    return {{"dnf", "remove", "-y", "--", name}, true};
  case manager::pacman:
    return {{"pacman", "-R", "--noconfirm", "--", name}, true};
  case manager::brew:
    return {{"brew", "uninstall", name}, false};
  }
  return {};
}

command upgrade_command(manager m, const std::string& name) {
  switch (m) {
  case manager::apt:
    return {{"apt-get", "install", "-y", "--only-upgrade", "--", name}, true};
  case manager::dnf:
    return {{"dnf", "upgrade", "-y", "--", name}, true};
  case manager::pacman:
    // pacman has no "upgrade one": re-syncing the package installs the
    // newest indexed version.
    return {{"pacman", "-S", "--noconfirm", "--", name}, true};
  case manager::brew:
    return {{"brew", "upgrade", name}, false};
  }
  return {};
}

command reinstall_command(manager m, const std::string& name) {
  switch (m) {
  case manager::apt:
    return {{"apt-get", "install", "-y", "--reinstall", "--", name}, true};
  case manager::dnf:
    return {{"dnf", "reinstall", "-y", "--", name}, true};
  case manager::pacman:
    return {{"pacman", "-S", "--noconfirm", "--", name}, true};
  case manager::brew:
    return {{"brew", "reinstall", name}, false};
  }
  return {};
}

command upgrade_all_command(manager m) {
  switch (m) {
  case manager::apt:
    return {{"apt-get", "upgrade", "-y"}, true};
  case manager::dnf:
    return {{"dnf", "upgrade", "-y"}, true};
  case manager::pacman:
    return {{"pacman", "-Su", "--noconfirm"}, true};
  case manager::brew:
    return {{"brew", "upgrade"}, false};
  }
  return {};
}

command refresh_index_command(manager m) {
  switch (m) {
  case manager::apt:
    return {{"apt-get", "update"}, true};
  case manager::dnf:
    return {{"dnf", "makecache", "--refresh"}, true};
  case manager::pacman:
    return {{"pacman", "-Sy"}, true};
  case manager::brew:
    return {{"brew", "update"}, false};
  }
  return {};
}

std::vector<std::string> elevated(manager m, const command& cmd, elevation how) {
  if (!cmd.needs_root)
    return cmd.argv;

  std::vector<std::string> argv;
  if (how == elevation::sudo)
    argv = {"sudo", "-n"}; // -n: fail rather than ask on a terminal we don't have.
  else if (how == elevation::pkexec)
    argv = {"pkexec"};
  // Through `env`, because sudo and pkexec both start from a clean
  // environment: without this apt would stop to ask configuration questions
  // on a stdin that is closed.
  if (m == manager::apt) {
    argv.push_back("env");
    argv.push_back("DEBIAN_FRONTEND=noninteractive");
  }
  argv.insert(argv.end(), cmd.argv.begin(), cmd.argv.end());
  return argv;
}

// ── Output parsers ─────────────────────────────────────────────────────────

std::vector<package> parse_installed(manager m, const std::string& text) {
  std::vector<package> out;
  for (auto& line : lines_of(text)) {
    if (trim(line).empty())
      continue;
    package p;
    switch (m) {
    case manager::apt: {
      auto f = split(line, '\t');
      // "ii " = wanted installed, and installed; anything else is a removed
      // package's leftover configuration, or a half-finished install.
      if (f.size() < 3 || !starts_with(f[2], "ii"))
        continue;
      p.name = f[0];
      p.version = f[1];
      p.description = f.size() > 3 ? trim(f[3]) : std::string{};
      break;
    }
    case manager::dnf: {
      auto f = split(line, '\t');
      if (f.size() < 2)
        continue;
      p.name = f[0];
      p.version = f[1];
      p.description = f.size() > 2 ? trim(f[2]) : std::string{};
      break;
    }
    case manager::pacman:
    case manager::brew: {
      // "name version" -- brew may list several kept versions; the last is
      // the newest.
      auto w = words(line);
      if (w.size() < 2)
        continue;
      p.name = w.front();
      p.version = w.back();
      break;
    }
    }
    out.push_back(std::move(p));
  }
  return out;
}

std::vector<package> parse_outdated(manager m, const std::string& text) {
  std::vector<package> out;
  for (auto& line : lines_of(text)) {
    auto w = words(line);
    if (w.empty())
      continue;
    package p;
    switch (m) {
    case manager::apt: {
      // "htop/noble-updates 3.3.0-4build1 amd64 [upgradable from: 3.3.0-4]"
      const size_t slash = w[0].find('/');
      if (slash == std::string::npos || w.size() < 2)
        continue; // "Listing...", warnings.
      p.name = w[0].substr(0, slash);
      p.latest = w[1];
      break;
    }
    case manager::dnf: {
      // "bash.x86_64   5.2.26-3.fc40   updates"; the list ends at the
      // "Obsoleting Packages" section.
      if (starts_with(line, "Obsoleting"))
        return out;
      if (w.size() < 3 || w[0].find('.') == std::string::npos)
        continue; // "Last metadata expiration check: ...".
      p.name = strip_arch(w[0]);
      p.latest = w[1];
      break;
    }
    case manager::pacman:
      // "linux 6.8.1.arch1-1 -> 6.8.2.arch1-1"
      if (w.size() < 4 || w[2] != "->")
        continue;
      p.name = w[0];
      p.latest = w[3];
      break;
    case manager::brew:
      // "wget (1.21.4) < 1.24.5", "some-cask (1.0) != 1.1"
      if (w.size() < 4 || (w[w.size() - 2] != "<" && w[w.size() - 2] != "!="))
        continue;
      p.name = w[0];
      p.latest = w.back();
      break;
    }
    out.push_back(std::move(p));
  }
  return out;
}

std::vector<package> parse_search(manager m, const std::string& text) {
  std::vector<package> out;
  for (auto& line : lines_of(text)) {
    if (trim(line).empty())
      continue;
    package p;
    switch (m) {
    case manager::apt: {
      // "htop - interactive processes viewer"
      const size_t sep = line.find(" - ");
      if (sep == std::string::npos)
        continue;
      p.name = trim(line.substr(0, sep));
      p.description = trim(line.substr(sep + 3));
      break;
    }
    case manager::dnf: {
      // dnf 4: "htop.x86_64 : Interactive process viewer"
      // dnf 5: " htop.x86_64\tInteractive process viewer"
      // Section captions ("==== Name Matched ====", "Matched fields: ...")
      // have no "name.arch" first word.
      auto w = words(line);
      std::string first = w[0];
      if (!first.empty() && first.back() == ':')
        first.pop_back();
      if (w.size() < 2 || first.find('.') == std::string::npos || first.find('=') != std::string::npos)
        continue;
      p.name = strip_arch(first);
      const size_t after = line.find(w[0]) + w[0].size();
      p.description = trim(line.substr(after));
      if (starts_with(p.description, ":"))
        p.description = trim(p.description.substr(1));
      break;
    }
    case manager::pacman: {
      // "extra/htop 3.3.0-3 [installed]" followed by an indented description.
      if (std::isspace(static_cast<unsigned char>(line[0]))) {
        if (!out.empty() && out.back().description.empty())
          out.back().description = trim(line);
        continue;
      }
      auto w = words(line);
      const size_t slash = w[0].find('/');
      if (slash == std::string::npos)
        continue;
      p.name = w[0].substr(slash + 1);
      p.version = w.size() > 1 ? w[1] : std::string{};
      break;
    }
    case manager::brew:
      // One name per line under "==> Formulae" / "==> Casks" captions.
      if (starts_with(line, "==>"))
        continue;
      p.name = trim(line);
      break;
    }
    out.push_back(std::move(p));
  }
  return out;
}

// ── Validation / messages ──────────────────────────────────────────────────

bool is_valid_package_name(const std::string& name) {
  if (name.empty() || name[0] == '-')
    return false;
  for (char ch : name) {
    if (!std::isalnum(static_cast<unsigned char>(ch)) && std::string{"@._+:/-"}.find(ch) == std::string::npos)
      return false;
  }
  return true;
}

bool is_safe_arg(const std::string& value) {
  return !value.empty() && value[0] != '-';
}

std::vector<std::string> split_names(const std::string& text) {
  return words(text);
}

std::string base_name(const std::string& name) {
  return name.substr(0, name.find(':'));
}

std::string error_summary(elevation how, const std::string& stderr_text) {
  auto contains = [&](const char* needle) { return stderr_text.find(needle) != std::string::npos; };

  if (how == elevation::sudo && (contains("a password is required") || contains("authentication is required") ||
                                 contains("a terminal is required")))
    return "administrator rights are needed, and sudo has no cached credentials to give them without asking. "
           "Run `sudo -v` in the terminal this tool was started from and retry, or restart it with `pkexec` "
           "as the second argument (it asks for the password in its own dialog).";
  if (how == elevation::pkexec && (contains("Not authorized") || contains("dismissed") ||
                                   contains("No authentication agent")))
    return "administrator rights were not granted: the authentication dialog was cancelled, or no polkit "
           "agent is running to show it. Retry, or restart the tool with `sudo` as the second argument.";

  // From the manager's own first error line, dropping the progress chatter
  // before it; otherwise the whole text.
  std::string text = stderr_text;
  size_t pos = std::string::npos;
  for (const char* marker : {"E: ", "Error: ", "error: "}) {
    if (starts_with(text, marker)) {
      pos = 0;
      break;
    }
    const size_t at = text.find(std::string{"\n"} + marker);
    if (at != std::string::npos)
      pos = std::min(pos, at + 1);
  }
  if (pos != std::string::npos && pos != 0)
    text = text.substr(pos);
  return trim(text);
}

} // namespace bdg::wish::pkg
