// MIT License © 2026 Binary Dice Games
/// @file du.cpp
/// @brief Client-side runner for the du embedded app.
///
/// The Du form (server-side) owns and renders the UI, but has no access to
/// the client's local machine. This runner is the other half: it scans a
/// local folder when the form emits `on_scan_requested`, keeps the whole
/// scanned tree in memory, and hands the form one folder of it at a time
/// through `show_directory()` -- the root after a scan, and whichever
/// folder the user opens after an `on_navigate`.
///
/// Scans and navigation run as jobs on one `common::command_worker` thread
/// (modules/bdg/common/command_worker.hpp), never inside an event handler,
/// so the UI keeps rendering during a long scan. Progress goes to the
/// form's own status line and its Rescan button turns into Stop; only the
/// cancel flag is set straight from the event handler, since a queued job
/// would wait behind the very scan it is meant to stop.
///
/// The folder to analyze may be passed after `--` on the command line, e.g.
/// `wish client --run=du -- /var/log`; the default is the current directory.
#include "modules/bdg/desktop/du/client/du.hpp"

#include "modules/bdg/desktop/du/client/du_scan.hpp"

#include "modules/bdg/common/command_worker.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace bdg::wish {

using namespace bison;

namespace {

namespace fs = std::filesystem;

/// A table listing longer than this is cut, the remainder shown as one row.
constexpr size_t kMaxRows = 1000;

/// @brief What the worker thread knows: the last completed scan and the
/// folder of it on display. Worker thread only.
struct du_state {
  du_scan::entry tree;
  bool scanned{false};
  std::string rel_path;
};

// bison::dynamic has no 64-bit integer field; counts are clamped (a folder
// with more than 2^31 entries is not a display concern).
int32_t clamp_count(std::uint64_t n) {
  return static_cast<int32_t>(std::min<std::uint64_t>(n, std::numeric_limits<int32_t>::max()));
}

void report(const std::shared_ptr<rmi::proxy::dynamic>& tool, const std::string& status, bool scanning) {
  dynamic patch;
  patch["status"_key] = status;
  patch["scanning"_key] = scanning;
  tool->set(std::move(patch)).get();
}

std::string join_lines(const std::vector<std::string>& lines) {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i > 0)
      out += '\n';
    out += lines[i];
  }
  return out;
}

// Sends the folder at `state.rel_path` to the form, in the shape
// du::do_show_directory() documents.
void show_directory(const std::shared_ptr<rmi::proxy::dynamic>& tool, const du_state& state) {
  const du_scan::entry* dir = du_scan::find(state.tree, state.rel_path);
  if (!dir || !dir->is_dir)
    return;

  dynamic entries;
  size_t i = 0;
  auto add_entry = [&](const std::string& name, const char* type, std::uint64_t size, std::uint64_t items) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = name;
    (*e)["type"_key] = std::string{type};
    (*e)["size"_key] = std::to_string(size);
    (*e)["items"_key] = clamp_count(items);
    entries[i++] = dynamic_ptr{e};
  };
  // Children are sorted largest first, so the cut drops the smallest.
  const size_t shown = std::min(dir->children.size(), kMaxRows);
  for (size_t k = 0; k < shown; ++k) {
    const du_scan::entry& c = dir->children[k];
    add_entry(c.name, c.is_dir ? "dir" : "file", c.size, c.files + c.dirs);
  }
  if (shown < dir->children.size()) {
    std::uint64_t rest = 0;
    for (size_t k = shown; k < dir->children.size(); ++k)
      rest += dir->children[k].size;
    const std::uint64_t count = dir->children.size() - shown;
    add_entry("(" + du_scan::format_count(count) + " smaller items)", "rest", rest, count);
  }

  const du_scan::treemap map = du_scan::build_treemap(*dir);
  std::vector<std::string> details(map.labels.size());
  for (size_t k = 0; k < details.size(); ++k)
    details[k] = du_scan::format_bytes(map.bytes[k]);

  dynamic args;
  args["root"_key] = state.tree.name;
  args["path"_key] = state.rel_path;
  args["size"_key] = std::to_string(dir->size);
  args["files"_key] = clamp_count(dir->files);
  args["dirs"_key] = clamp_count(dir->dirs);
  args["entries"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(entries))};
  args["parents"_key] = map.parents;
  args["sizes"_key] = map.sizes;
  args["colors"_key] = map.colors;
  args["kinds"_key] = map.kinds;
  args["labels"_key] = join_lines(map.labels);
  args["details"_key] = join_lines(details);
  tool->call("show_directory"_key, std::move(args)).get();
}

// Scans `path` and, unless cancelled or failed, replaces the tree on
// display with the result. A cancelled or failed scan keeps the previous
// tree, so the user is not left with an empty window.
void scan_and_show(
    const std::shared_ptr<rmi::proxy::dynamic>& tool, const std::shared_ptr<du_state>& state,
    const std::shared_ptr<std::atomic<bool>>& cancel, const std::string& path) {
  using clock = std::chrono::steady_clock;
  std::error_code ec;
  fs::path root = fs::absolute(fs::path(path), ec);
  if (ec)
    root = fs::path(path);
  root = root.lexically_normal();
  // "/tmp/" normalizes with an empty last component; keep "/" itself intact.
  if (!root.has_filename() && root.has_parent_path() && root != root.root_path())
    root = root.parent_path();

  *cancel = false;
  report(tool, "Scanning " + root.string() + " ...", true);

  const auto started = clock::now();
  auto last_push = started;
  du_scan::entry tree;
  du_scan::progress totals;
  bool completed = false;
  try {
    completed = du_scan::scan(
        root, tree,
        [&](const du_scan::progress& p) {
          if (cancel->load())
            return false;
          const auto now = clock::now();
          if (now - last_push >= std::chrono::milliseconds(200)) {
            last_push = now;
            report(
                tool,
                "Scanning: " + du_scan::format_count(p.items) + " items, " + du_scan::format_bytes(p.bytes) +
                    "  -  " + p.current,
                true);
          }
          return true;
        },
        &totals);
  } catch (const std::exception& e) {
    report(tool, std::string{"Scan failed: "} + e.what(), false);
    return;
  }
  if (!completed) {
    report(tool, "Scan cancelled.", false);
    return;
  }

  state->tree = std::move(tree);
  state->scanned = true;
  state->rel_path.clear();
  show_directory(tool, *state);

  const std::chrono::duration<double> elapsed = clock::now() - started;
  char seconds[32];
  std::snprintf(seconds, sizeof(seconds), "%.1f s", elapsed.count());
  std::string message = "Scanned " + du_scan::format_count(totals.items) + " items (" +
      du_scan::format_bytes(totals.bytes) + ") in " + seconds + ".";
  if (totals.errors > 0)
    message += " " + du_scan::format_count(totals.errors) + " folders could not be read.";
  report(tool, message, false);
}

} // namespace

void run_du(wish_app_host& s) {
  auto tool = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "Du"_key).get());
  auto state = std::make_shared<du_state>();
  auto cancel = std::make_shared<std::atomic<bool>>(false);

  auto worker = std::make_shared<common::command_worker>(s, "Disk Usage");
  worker->start();

  worker->on(*tool, "on_scan_requested"_key, [tool, state, cancel](const dynamic& payload) {
    scan_and_show(tool, state, cancel, payload.as<std::string>("path"_key));
  });

  // Not a worker job: it would queue behind the scan it is meant to stop.
  tool->onEvent("on_cancel_requested"_key, [cancel](dynamic) { *cancel = true; });

  worker->on(*tool, "on_navigate"_key, [tool, state](const dynamic& payload) {
    if (!state->scanned)
      return;
    const auto rel_path = payload.as<std::string>("path"_key);
    const du_scan::entry* dir = du_scan::find(state->tree, rel_path);
    if (!dir || !dir->is_dir)
      return;
    state->rel_path = rel_path;
    show_directory(tool, *state);
  });

  tool->onEvent("closed"_key, [&s, worker, cancel](dynamic) {
    *cancel = true;
    worker->shutdown();
    s.signal_done();
  });

  // `wish client --run=du -- <folder>`: analyze that folder, else the
  // current directory.
  std::string start = s.app_args().empty() ? fs::current_path().string() : s.app_args().front();
  worker->post([tool, state, cancel, start] { scan_and_show(tool, state, cancel, start); });

  // on_session() blocks until signal_done() is called.
}

namespace {
struct du_app_registrar {
  du_app_registrar() {
    register_app({
        .name = "du",
        .organization = WISH_MODULE_BDG_DESKTOP_DU_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DESKTOP_DU_COLLECTION,
        .description = "Disk usage analyzer: scan a local folder and see what takes the space, as a "
                       "size-sorted table and a treemap",
        .params = {{"folder", "Folder to analyze at startup (default: the current directory)"}},
        .run = run_du,
    });
  }
};
const du_app_registrar du_app_registrar_instance;
} // namespace

} // namespace bdg::wish
