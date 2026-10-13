// MIT License © 2026 Binary Dice Games
/// @file find.cpp
/// @brief Client-side runner for the find embedded app.
///
/// The Find form (server-side) owns and renders the UI, but has no access
/// to the client's machine. This runner is the other half, laid out like
/// FSearch: it indexes the configured folders into an in-memory database
/// (find_db.hpp) and answers every `on_search` from that database, without
/// touching the disk again until the user picks File > Update Database.
///
/// Three `common::command_worker` threads keep the UI responsive:
///   - the index worker builds a database and swaps it in when complete --
///     an update keeps the previous database searchable meanwhile;
///   - the search worker runs queries. Each keystroke queues one, so a job
///     whose generation is no longer the latest is skipped (or stopped
///     midway) instead of being answered;
///   - the open worker launches the system's default application, which
///     may take a moment.
///
/// Folders to index may be passed after `--`, e.g.
/// `wish client --run=find -- /srv /var/log`; otherwise the "Indexed
/// folders" setting is used, and when that is empty, the home folder.
#include "modules/bdg/desktop/find/client/find.hpp"

#include "modules/bdg/desktop/find/client/find_db.hpp"
#include "modules/bdg/desktop/find/find_settings.hpp"

#include "modules/bdg/common/command_worker.hpp"
#include "modules/bdg/common/process.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"
#include "src/i18n/translations.hpp"

#include "src/bison/bison.hpp"
#include "src/bison/bison_sync.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish {

using namespace bison;

namespace {

namespace fs = std::filesystem;

using db_ptr = std::shared_ptr<const find_db::database>;

/// @brief State shared by the event handlers and the workers.
struct find_state {
  /// The database searched; null until the first one is built.
  synchronized<db_ptr> db;
  /// Generation of the latest `on_search`; older searches are dropped.
  std::atomic<int32_t> latest_generation{0};
  /// Set to stop an indexing run (the form closed).
  std::atomic<bool> cancel_index{false};
  /// What to index (the command line, else the settings).
  find_db::index_options index_options;
};

// bison::dynamic has no 64-bit integer field.
int32_t clamp_count(std::uint64_t n) {
  return static_cast<int32_t>(std::min<std::uint64_t>(n, std::numeric_limits<int32_t>::max()));
}

std::vector<std::string> split_list(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    const auto b = cur.find_first_not_of(" \t\r");
    const auto e = cur.find_last_not_of(" \t\r");
    if (b != std::string::npos)
      out.push_back(cur.substr(b, e - b + 1));
    cur.clear();
  };
  for (char c : text) {
    if (c == ';' || c == '\n')
      flush();
    else
      cur += c;
  }
  flush();
  return out;
}

std::string home_folder() {
  for (const char* var : {"HOME", "USERPROFILE"})
    if (const char* v = std::getenv(var); v && *v)
      return v;
  std::error_code ec;
  return fs::current_path(ec).string();
}

// ── Indexing ─────────────────────────────────────────────────────────────────

void report_index(const std::shared_ptr<rmi::proxy::dynamic>& tool, bool indexing, const std::string& status) {
  dynamic patch;
  patch["indexing"_key] = indexing;
  patch["status"_key] = status;
  tool->set(std::move(patch)).get();
}

void build_database(const std::shared_ptr<rmi::proxy::dynamic>& tool, const std::shared_ptr<find_state>& state) {
  using clock = std::chrono::steady_clock;
  state->cancel_index = false;
  report_index(tool, true, "$$FIND_LOADING_DATABASE");

  const auto started = clock::now();
  auto last_push = started;
  auto db = std::make_shared<find_db::database>();
  find_db::index_progress totals;
  bool completed = false;
  try {
    completed = find_db::database::build(
        state->index_options, *db,
        [&](const find_db::index_progress& p) {
          if (state->cancel_index.load())
            return false;
          const auto now = clock::now();
          if (now - last_push >= std::chrono::milliseconds(250)) {
            last_push = now;
            report_index(
                tool, true,
                make_message(
                    "$$FIND_INDEXING_PROGRESS",
                    {find_db::format_count(p.files), find_db::format_count(p.folders), p.current}));
          }
          return true;
        },
        &totals);
  } catch (const std::exception& e) {
    report_index(tool, false, make_message("$$FIND_INDEXING_FAILED", {e.what()}));
    return;
  }
  if (!completed) {
    report_index(tool, false, {});
    return;
  }

  *state->db.wlock() = db;

  const std::chrono::duration<double> elapsed = clock::now() - started;
  char seconds[32];
  std::snprintf(seconds, sizeof(seconds), "%.1f s", elapsed.count());
  dynamic patch;
  patch["indexing"_key] = false;
  patch["status"_key] = std::string{};
  patch["db_files"_key] = clamp_count(db->file_count());
  patch["db_folders"_key] = clamp_count(db->folder_count());
  tool->set(std::move(patch)).get();
  std::fprintf(
      stderr, "find: indexed %s files, %s folders in %s (%s folders unreadable)\n",
      find_db::format_count(db->file_count()).c_str(), find_db::format_count(db->folder_count()).c_str(), seconds,
      find_db::format_count(totals.errors).c_str());

  // Answer the current query from the new database.
  tool->call("search_again"_key, dynamic{}).get();
}

// ── Searching ────────────────────────────────────────────────────────────────

/// Up to this many results are sent to the form; the status bar gives the
/// full count.
constexpr std::size_t kMaxRows = 1000;

void run_search(
    const std::shared_ptr<rmi::proxy::dynamic>& tool, const std::shared_ptr<find_state>& state,
    const dynamic& request) {
  const int32_t generation = request.as<int32_t>("generation"_key);
  auto stale = [&] { return generation != state->latest_generation.load(); };
  if (stale())
    return;
  const db_ptr db = *state->db.rlock();
  if (!db)
    return; // answered by search_again() once the database is ready

  find_db::search_options options;
  options.query = request.as<std::string>("query"_key);
  options.filter = static_cast<find_db::filter_kind>(
      std::clamp(request.as<int32_t>("filter"_key), 0, find_db::kFilterCount - 1));
  options.match_case = request.as<bool>("match_case"_key);
  options.search_in_path = request.as<bool>("search_in_path"_key);
  options.regex = request.as<bool>("regex"_key);
  options.sort = static_cast<find_db::sort_column>(std::clamp(request.as<int32_t>("sort_column"_key), 0, 4));
  options.ascending = request.as<bool>("ascending"_key);
  options.auto_match_case = request.as<bool>("auto_match_case"_key);
  options.auto_search_in_path = request.as<bool>("auto_search_in_path"_key);
  options.max_rows = kMaxRows;

  const find_db::search_result result = find_db::search(*db, options, stale);
  if (result.cancelled || stale())
    return;

  dynamic entries;
  size_t i = 0;
  for (std::uint32_t index : result.rows) {
    const find_db::entry& e = db->at(index);
    auto row = std::make_shared<dynamic>();
    (*row)["name"_key] = e.name;
    (*row)["path"_key] = db->parent_path(index);
    (*row)["type"_key] = find_db::type_label(e);
    (*row)["size"_key] = find_db::format_bytes(e.size);
    (*row)["modified"_key] = find_db::format_time(e.mtime);
    (*row)["dir"_key] = e.is_dir;
    entries[i++] = dynamic_ptr{row};
  }

  dynamic args;
  args["generation"_key] = generation;
  args["total"_key] = clamp_count(result.total);
  args["folders"_key] = clamp_count(result.folders);
  args["entries"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(entries))};
  if (!result.error.empty())
    args["error"_key] = result.error;
  tool->call("show_results"_key, std::move(args)).get();
}

// ── Opening ──────────────────────────────────────────────────────────────────

/// Opens @p path with the system's default application.
void open_with_system(const std::string& path) {
#if defined(_WIN32)
  // `start` is a cmd built-in; its first quoted argument is a window title.
  const std::vector<std::string> argv{"cmd", "/c", "start", "", path};
#elif defined(__APPLE__)
  const std::vector<std::string> argv{"open", path};
#else
  const std::vector<std::string> argv{"xdg-open", path};
#endif
  const common::process_result r = common::run_process(argv);
  if (!r.ok())
    std::fprintf(stderr, "find: cannot open %s: %s\n", path.c_str(), common::error_output(r).c_str());
}

} // namespace

void run_find(wish_app_host& s) {
  auto tool = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "Find"_key).get());
  auto state = std::make_shared<find_state>();

  // Locations: the command line, else the setting, else the home folder.
  auto& opts = state->index_options;
  opts.locations = s.app_args();
  if (opts.locations.empty())
    opts.locations = split_list(s.read_text_setting(find_settings::locations()));
  if (opts.locations.empty())
    opts.locations = {home_folder()};
  opts.excludes = split_list(s.read_text_setting(find_settings::excludes()));
  opts.exclude_hidden = s.read_bool_setting(find_settings::exclude_hidden());
  opts.one_filesystem = s.read_bool_setting(find_settings::one_filesystem());
  const bool auto_match_case = s.read_bool_setting(find_settings::auto_match_case());
  const bool auto_search_in_path = s.read_bool_setting(find_settings::auto_search_in_path());

  auto index_worker = std::make_shared<common::command_worker>(s, "$$FIND_TITLE");
  auto search_worker = std::make_shared<common::command_worker>(s, "$$FIND_TITLE");
  auto open_worker = std::make_shared<common::command_worker>(s, "$$FIND_TITLE");
  index_worker->start();
  search_worker->start();
  open_worker->start();

  tool->onEvent(
      "on_search"_key,
      [tool, state, search_worker, auto_match_case, auto_search_in_path](dynamic payload) {
        // Recorded here, not in the job: a queued job must already see that
        // a newer search exists.
        state->latest_generation = payload.as<int32_t>("generation"_key);
        payload["auto_match_case"_key] = auto_match_case;
        payload["auto_search_in_path"_key] = auto_search_in_path;
        search_worker->post([tool, state, payload = std::move(payload)] { run_search(tool, state, payload); });
      });

  index_worker->on(*tool, "on_update_database"_key, [tool, state](const dynamic&) {
    build_database(tool, state);
  });

  auto open_handler = [open_worker](bool folder) {
    return [open_worker, folder](dynamic payload) {
      const std::string paths = payload.as<std::string>("paths"_key);
      open_worker->post([paths, folder] {
        size_t begin = 0;
        while (begin < paths.size()) {
          size_t end = paths.find('\n', begin);
          if (end == std::string::npos)
            end = paths.size();
          const std::string path = paths.substr(begin, end - begin);
          if (!path.empty())
            open_with_system(folder ? fs::path(path).parent_path().string() : path);
          begin = end + 1;
        }
      });
    };
  };
  tool->onEvent("on_open"_key, open_handler(false));
  tool->onEvent("on_open_folder"_key, open_handler(true));

  tool->onEvent("closed"_key, [&s, state, index_worker, search_worker, open_worker](dynamic) {
    state->cancel_index = true;
    state->latest_generation = -1; // stops a running search
    index_worker->shutdown();
    search_worker->shutdown();
    open_worker->shutdown();
    s.signal_done();
  });

  index_worker->post([tool, state] { build_database(tool, state); });

  // on_session() blocks until signal_done() is called.
}

namespace {
struct find_app_registrar {
  find_app_registrar() {
    register_app({
        .name = "find",
        .organization = WISH_MODULE_BDG_DESKTOP_FIND_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DESKTOP_FIND_COLLECTION,
        .description = "Instant file search in the spirit of FSearch: indexes local folders and finds "
                       "files as you type",
        .icon = "res/icons/search.png",
        .params = {{"folders", "Folders to index (default: the Indexed folders setting, else the home folder)"}},
        .run = run_find,
    });
  }
};
const find_app_registrar find_app_registrar_instance;
} // namespace

} // namespace bdg::wish
