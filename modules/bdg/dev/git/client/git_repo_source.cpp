// MIT License © 2025 Binary Dice Games
/// @file git_repo_source.cpp
/// @brief Implementation of git_repo_source.
#include "git_repo_source.hpp"
#include "git_process.hpp"

#include <algorithm>
#include <sstream>

#if defined(_WIN32)
static constexpr const char* kNullDevice = "NUL";
#else
static constexpr const char* kNullDevice = "/dev/null";
#endif

namespace bdg::wish::git {

using namespace bdg::bison;

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t pos = s.find(sep, start);
    if (pos == std::string::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

// Builds a dynamic array of plain-string entries (e.g. a commit's parent
// hash list) -- numeric-keyed string fields, not nested dynamic_ptr objects
// (see git.cpp's read_string_array() on the server side for the matching
// reader and why: bison::field has no vector<string> alternative).
dynamic_ptr string_array(const std::vector<std::string>& items) {
  auto arr = std::make_shared<dynamic>();
  size_t i = 0;
  for (auto& s : items)
    (*arr)[i++] = s;
  return dynamic_ptr{arr};
}

} // namespace

git_repo_source::git_repo_source(std::shared_ptr<bison::rmi::proxy::dynamic> proxy, std::string repo_path)
    : proxy_(std::move(proxy)), repo_path_(std::move(repo_path)) {}

// ── command log (debugging/tracing) ─────────────────────────────────────────

process_result git_repo_source::run_logged(const std::vector<std::string>& args) {
  auto r = run_git(repo_path_, args);
  push_command_log(args, r);
  return r;
}

void git_repo_source::push_command_log(const std::vector<std::string>& args, const process_result& r) {
  std::string command = "git";
  for (auto& a : args)
    command += " " + a;

  std::string output = r.ok() ? r.stdout_text : (r.stderr_text.empty() ? r.stdout_text : r.stderr_text);
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
    output.pop_back();
  std::replace(output.begin(), output.end(), '\n', ' ');
  constexpr size_t kMaxOutputPreview = 200;
  if (output.size() > kMaxOutputPreview)
    output = output.substr(0, kMaxOutputPreview) + "...";

  dynamic args_out;
  args_out["command"_key] = command;
  args_out["exit_code"_key] = r.exit_code;
  args_out["ok"_key] = r.ok();
  args_out["output"_key] = output;

  try {
    proxy_->call("append_command_log"_key, std::move(args_out)).get();
  } catch (const std::exception&) {
  }
}

void git_repo_source::refresh_all() {
  push_refs();
  push_log();
  push_status();
}

// ── refs ─────────────────────────────────────────────────────────────────────

void git_repo_source::push_refs() {
  dynamic args;

  auto head = run_logged({"rev-parse", "--abbrev-ref", "HEAD"});
  std::string current_branch = head.ok() ? head.stdout_text : std::string{};
  while (!current_branch.empty() && (current_branch.back() == '\n' || current_branch.back() == '\r'))
    current_branch.pop_back();
  if (current_branch.empty() || current_branch == "HEAD") {
    auto sha = run_logged({"rev-parse", "--short", "HEAD"});
    std::string s = sha.ok() ? sha.stdout_text : std::string{};
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
      s.pop_back();
    current_branch = "detached at " + s;
  }
  args["current_branch"_key] = current_branch;

  dynamic branches;
  size_t bi = 0;
  // %(upstream:track) reports ahead/behind (or "[gone]" if the configured
  // upstream ref no longer exists, e.g. a deleted/pruned remote branch)
  // directly from refs already loaded by for-each-ref -- no separate
  // `rev-list name...upstream` subprocess needed. That extra call used to
  // run unconditionally whenever a branch had *any* configured upstream,
  // including a "gone" one, where it reliably failed with "fatal: ambiguous
  // argument ... unknown revision" and polluted the command log on every
  // refresh.
  auto refs = run_logged(
      {"for-each-ref",
       "--format=%(refname:short)\t%(upstream:short)\t%(objecttype)\t%(upstream:track)",
       "refs/heads",
       "refs/remotes"});
  if (refs.ok()) {
    std::istringstream iss(refs.stdout_text);
    std::string line;
    while (std::getline(iss, line)) {
      if (line.empty())
        continue;
      auto cols = split(line, '\t');
      if (cols.empty())
        continue;
      std::string name = cols[0];
      std::string upstream = cols.size() > 1 ? cols[1] : std::string{};
      std::string track = cols.size() > 3 ? cols[3] : std::string{};
      const bool is_remote = name.rfind("origin/", 0) == 0 || name.find('/') != std::string::npos;

      int32_t ahead = 0, behind = 0;
      if (!track.empty() && track != "[gone]") {
        auto ahead_pos = track.find("ahead ");
        if (ahead_pos != std::string::npos)
          ahead = std::atoi(track.c_str() + ahead_pos + 6);
        auto behind_pos = track.find("behind ");
        if (behind_pos != std::string::npos)
          behind = std::atoi(track.c_str() + behind_pos + 7);
      }

      auto e = std::make_shared<dynamic>();
      (*e)["name"_key] = name;
      (*e)["is_remote"_key] = is_remote;
      (*e)["upstream"_key] = upstream;
      (*e)["ahead"_key] = ahead;
      (*e)["behind"_key] = behind;
      branches[bi++] = dynamic_ptr{e};
    }
  }
  args["branches"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(branches))};

  dynamic tags;
  size_t ti = 0;
  auto tag_list = run_logged({"tag", "--list"});
  if (tag_list.ok()) {
    std::istringstream iss(tag_list.stdout_text);
    std::string line;
    while (std::getline(iss, line)) {
      if (line.empty())
        continue;
      auto e = std::make_shared<dynamic>();
      (*e)["name"_key] = line;
      tags[ti++] = dynamic_ptr{e};
    }
  }
  args["tags"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(tags))};

  dynamic stashes;
  size_t si = 0;
  auto stash_list = run_logged({"stash", "list", "--format=%gd\t%s"});
  if (stash_list.ok()) {
    std::istringstream iss(stash_list.stdout_text);
    std::string line;
    while (std::getline(iss, line)) {
      if (line.empty())
        continue;
      auto cols = split(line, '\t');
      std::string ref = cols[0]; // "stash@{N}"
      auto lb = ref.find('{');
      auto rb = ref.find('}');
      int32_t index = 0;
      if (lb != std::string::npos && rb != std::string::npos && rb > lb)
        index = std::atoi(ref.substr(lb + 1, rb - lb - 1).c_str());
      auto e = std::make_shared<dynamic>();
      (*e)["index"_key] = index;
      (*e)["message"_key] = cols.size() > 1 ? cols[1] : std::string{};
      stashes[si++] = dynamic_ptr{e};
    }
  }
  args["stashes"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(stashes))};

  try {
    proxy_->call("update_refs"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
}

// ── log / graph ────────────────────────────────────────────────────────────

bool git_repo_source::working_tree_dirty() {
  auto r = run_logged({"status", "--porcelain=v1"});
  return r.ok() && !r.stdout_text.empty();
}

void git_repo_source::push_log() {
  dynamic args;
  args["working_dirty"_key] = working_tree_dirty();

  auto head_sha = run_logged({"rev-parse", "HEAD"});
  std::string head_hash = head_sha.ok() ? head_sha.stdout_text : std::string{};
  while (!head_hash.empty() && (head_hash.back() == '\n' || head_hash.back() == '\r'))
    head_hash.pop_back();
  args["head_hash"_key] = head_hash;

  // \x1f (unit separator) between fields, \x1e (record separator) between
  // commits -- avoids any collision with real commit-message content,
  // unlike a printable delimiter such as '|'.
  auto log = run_logged(
      {"log",
       "--branches",
       "--tags",
       "--topo-order",
       "--date-order",
       "-n",
       "300",
       "--pretty=format:%H%x1f%P%x1f%an%x1f%ad%x1f%s%x1e",
       "--date=format:%Y-%m-%d %H:%M"});

  dynamic commits;
  size_t ci = 0;
  if (log.ok()) {
    for (auto& record : split(log.stdout_text, '\x1e')) {
      if (record.empty() || record == std::string(1, '\n'))
        continue;
      std::string rec = record;
      if (!rec.empty() && rec.front() == '\n')
        rec.erase(rec.begin());
      auto fields = split(rec, '\x1f');
      if (fields.size() < 5)
        continue;
      auto e = std::make_shared<dynamic>();
      (*e)["hash"_key] = fields[0];
      (*e)["parents"_key] = string_array(fields[1].empty() ? std::vector<std::string>{} : split(fields[1], ' '));
      (*e)["author"_key] = fields[2];
      (*e)["date"_key] = fields[3];
      (*e)["subject"_key] = fields[4];
      commits[ci++] = dynamic_ptr{e};
    }
  }
  args["commits"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(commits))};

  try {
    proxy_->call("update_log"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
}

// ── status ───────────────────────────────────────────────────────────────────

void git_repo_source::push_status() {
  dynamic args;
  dynamic staged, unstaged;
  size_t sti = 0, ui = 0;

  auto status = run_logged({"status", "--porcelain=v1"});
  if (status.ok()) {
    std::istringstream iss(status.stdout_text);
    std::string line;
    while (std::getline(iss, line)) {
      if (line.size() < 3)
        continue;
      char x = line[0];
      char y = line[1];
      std::string path = line.substr(3);
      auto arrow = path.find(" -> ");
      if (arrow != std::string::npos)
        path = path.substr(arrow + 4);

      if (x != ' ' && x != '?') {
        auto e = std::make_shared<dynamic>();
        (*e)["path"_key] = path;
        (*e)["status"_key] = std::string(1, x);
        staged[sti++] = dynamic_ptr{e};
      }
      if (y != ' ') {
        auto e = std::make_shared<dynamic>();
        (*e)["path"_key] = path;
        (*e)["status"_key] = std::string(1, x == '?' ? '?' : y);
        unstaged[ui++] = dynamic_ptr{e};
      }
    }
  }
  args["staged"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(staged))};
  args["unstaged"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(unstaged))};

  try {
    proxy_->call("update_status"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
}

// ── commit files / diff ───────────────────────────────────────────────────────

void git_repo_source::on_commit_files_requested(const std::string& hash) {
  dynamic args;
  args["hash"_key] = hash;

  dynamic files;
  size_t fi = 0;
  auto r = run_logged({"show", "--name-status", "--format=", hash});
  if (r.ok()) {
    std::istringstream iss(r.stdout_text);
    std::string line;
    while (std::getline(iss, line)) {
      if (line.empty())
        continue;
      auto tab = line.find('\t');
      if (tab == std::string::npos)
        continue;
      std::string status = line.substr(0, tab).substr(0, 1);
      std::string path = line.substr(tab + 1);
      auto e = std::make_shared<dynamic>();
      (*e)["path"_key] = path;
      (*e)["status"_key] = status;
      files[fi++] = dynamic_ptr{e};
    }
  }
  args["files"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(files))};

  try {
    proxy_->call("update_commit_files"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
}

void git_repo_source::on_diff_requested(const std::string& hash, const std::string& path, bool staged) {
  process_result r;
  if (hash.empty()) {
    r = run_logged(staged ? std::vector<std::string>{"diff", "--no-color", "--cached", "--", path}
                                    : std::vector<std::string>{"diff", "--no-color", "--", path});
    if (r.ok() && r.stdout_text.empty()) {
      // Nothing under version control to diff against -- likely a brand-new
      // untracked file; show its whole content as an all-added diff instead
      // of a blank panel.
      r = run_logged({"diff", "--no-color", "--no-index", "--", kNullDevice, path});
    }
  } else {
    r = run_logged({"show", "--no-color", "--format=", hash, "--", path});
  }

  dynamic args;
  args["path"_key] = path;
  // Echoed back so the server's do_update_diff() can tell a stale response
  // (for a selection the user has since navigated away from) apart from
  // the current one -- see that method's own comment. Without these, the
  // server had no way to validate the response at all: "path" alone can't
  // distinguish e.g. the same file re-selected under a different commit,
  // or a staged/unstaged toggle of the same working-tree path.
  args["hash"_key] = hash;
  args["staged"_key] = staged;

  dynamic lines;
  size_t li = 0;
  std::istringstream iss(r.stdout_text);
  std::string line;
  while (std::getline(iss, line)) {
    std::string kind = "context";
    if (line.rfind("@@", 0) == 0)
      kind = "header";
    else if (
        line.rfind("diff --git", 0) == 0 || line.rfind("index ", 0) == 0 || line.rfind("--- ", 0) == 0 ||
        line.rfind("+++ ", 0) == 0)
      kind = "header";
    else if (!line.empty() && line[0] == '+')
      kind = "add";
    else if (!line.empty() && line[0] == '-')
      kind = "del";

    auto e = std::make_shared<dynamic>();
    (*e)["kind"_key] = kind;
    (*e)["text"_key] = line;
    lines[li++] = dynamic_ptr{e};
  }
  args["lines"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(lines))};

  try {
    proxy_->call("update_diff"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
}

// ── mutating actions ─────────────────────────────────────────────────────────

void git_repo_source::run_and_refresh(const std::string& command_label, const std::vector<std::string>& args) {
  auto r = run_logged(args);

  dynamic report;
  report["command"_key] = command_label;
  report["ok"_key] = r.ok();
  report["output"_key] = r.ok() ? std::string{} : (r.stderr_text.empty() ? r.stdout_text : r.stderr_text);
  try {
    proxy_->call("command_result"_key, std::move(report)).get();
  } catch (const std::exception&) {
    return; // form already torn down.
  }

  refresh_all();
}

void git_repo_source::on_stage(const std::string& path) {
  run_and_refresh("stage", {"add", "--", path});
}

void git_repo_source::on_unstage(const std::string& path) {
  run_and_refresh("unstage", {"restore", "--staged", "--", path});
}

void git_repo_source::on_commit(const std::string& message) {
  // Safe as a plain argv entry (no shell escaping needed) -- run_git()
  // execs "git" directly via uv_spawn with a real argv array, never through
  // a shell.
  run_and_refresh("commit", {"commit", "-m", message});
}

void git_repo_source::on_checkout(const std::string& ref) {
  auto r = run_logged({"switch", ref});
  if (!r.ok() && ref.find('/') != std::string::npos) {
    // ref looks like a remote-tracking ref (e.g. "origin/feature") -- `git
    // switch` refuses to check that out directly (it insists on --detach
    // for a non-branch ref, unlike `git checkout`'s DWIM). Try the
    // equivalent local branch name first: it may already exist (checked
    // out before, or created independently of this remote-tracking ref),
    // in which case just switching to it is correct and `-c` would fail
    // with "a branch named ... already exists". Only create+track a new
    // local branch if none exists yet.
    std::string local = ref.substr(ref.find('/') + 1);
    r = run_logged({"switch", local});
    if (!r.ok())
      r = run_logged({"switch", "--track", "-c", local, ref});
  }
  dynamic report;
  report["command"_key] = std::string{"checkout"};
  report["ok"_key] = r.ok();
  report["output"_key] = r.ok() ? std::string{} : r.stderr_text;
  try {
    proxy_->call("command_result"_key, std::move(report)).get();
  } catch (const std::exception&) {
    return;
  }
  refresh_all();
}

void git_repo_source::on_create_branch(const std::string& name, const std::string& start_point) {
  std::vector<std::string> args{"branch", name};
  if (!start_point.empty())
    args.push_back(start_point);
  run_and_refresh("create branch", args);
}

void git_repo_source::on_delete_branch(const std::string& name, bool force) {
  run_and_refresh("delete branch", {"branch", force ? "-D" : "-d", name});
}

void git_repo_source::on_fetch() {
  run_and_refresh("fetch", {"fetch"});
}

void git_repo_source::on_pull() {
  run_and_refresh("pull", {"pull"});
}

void git_repo_source::on_push() {
  run_and_refresh("push", {"push"});
}

void git_repo_source::on_merge(const std::string& ref) {
  auto r = run_logged({"merge", "--ff-only", ref});
  if (!r.ok())
    r = run_logged({"merge", ref});
  dynamic report;
  report["command"_key] = std::string{"merge"};
  report["ok"_key] = r.ok();
  report["output"_key] = r.ok() ? std::string{} : r.stderr_text;
  try {
    proxy_->call("command_result"_key, std::move(report)).get();
  } catch (const std::exception&) {
    return;
  }
  refresh_all();
}

void git_repo_source::on_stash_push() {
  run_and_refresh("stash", {"stash", "push"});
}

void git_repo_source::on_stash_pop(int32_t index) {
  run_and_refresh("stash pop", {"stash", "pop", "stash@{" + std::to_string(index) + "}"});
}

void git_repo_source::on_stash_apply(int32_t index) {
  run_and_refresh("stash apply", {"stash", "apply", "stash@{" + std::to_string(index) + "}"});
}

void git_repo_source::on_stash_drop(int32_t index) {
  run_and_refresh("stash drop", {"stash", "drop", "stash@{" + std::to_string(index) + "}"});
}

} // namespace bdg::wish::git
