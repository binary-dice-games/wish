// MIT License © 2025 Binary Dice Games
/// @file editor.cpp
/// @brief Client-side runner for the Editor embedded app.
///
/// The Editor form (server-side) never touches the client's local
/// filesystem -- it only edits a file already sitting in its session
/// sandbox (same rule as Notepad, see modules/bdg/desktop/notepad/client/
/// notepad.cpp). This runner is the bridge: it uploads the local JSON file
/// once at startup, then owns a background poll loop (mirroring Process
/// Explorer's sampling thread, see modules/bdg/desktop/process_explorer/
/// client/process_explorer.cpp) that watches the local file for changes
/// made outside the tool and re-uploads it under a fresh sandbox name each
/// time -- required because TextEditor's renderer only reloads its buffer
/// when `file_path` itself changes, not when the file underneath an
/// unchanged path is overwritten.
///
/// The JSON file to edit is passed after `--` on the command line, e.g.
/// `wish client --run=editor -- path/to/ui.json` (see
/// `wish_app_host::app_args()`).
#include "modules/bdg/dev/editor/client/editor.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"
#include "src/bison/bison_sync.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace bdg::wish {

using namespace bison;

namespace {

namespace fs = std::filesystem;

std::string read_local_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
}

void write_local_file(const fs::path& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

// Tracks the sandbox filename currently backing the source TextEditor and
// the content we ourselves last pushed there, so the change-watcher thread
// can tell "the file changed because we wrote it back" apart from a
// genuine external edit and avoid re-uploading in a feedback loop.
struct sandbox_source_state {
  std::string sandbox_name;
  std::string last_uploaded_content;
  int counter{0};
};

using sync_state = bison::synchronized<sandbox_source_state>;

// Upload the local file's current contents under a fresh sandbox name and
// point the Editor's set_source at it. A fresh name is required every call
// -- see the file-level comment above.
void push_local_file(
    wish_app_host& s,
    const std::shared_ptr<rmi::proxy::dynamic>& ed,
    const std::shared_ptr<sync_state>& state,
    const fs::path& local_path) {
  auto data = read_local_file(local_path);
  std::string name;
  {
    auto lock = state->wlock();
    name = "source_" + std::to_string(lock->counter++) + ".json";
    lock->sandbox_name = name;
    lock->last_uploaded_content = data;
  }
  s.upload_file(name, data).get();

  dynamic args;
  args["path"_key] = name;
  args["display_path"_key] = local_path.string();
  ed->call("set_source"_key, std::move(args)).get();
}

} // namespace

void run_editor(wish_app_host& s) {
  namespace fs = std::filesystem;

  if (s.app_args().empty()) {
    std::cerr << "[editor] usage: wish client --run=editor -- path/to/ui.json\n";
    s.signal_done();
    return;
  }

  fs::path local_path = fs::absolute(s.app_args()[0]);
  if (!fs::exists(local_path))
    write_local_file(local_path, "");

  auto ed = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "Editor"_key).get());
  auto state = std::make_shared<sync_state>();
  auto stop = std::make_shared<std::atomic<bool>>(false);

  push_local_file(s, ed, state, local_path);

  // Ctrl+S inside the source editor, or a confirmed "Save & Close": download
  // the sandbox file, persist it back to the original local path, and tell
  // the server the save completed (mirrors Notepad's on_file_saved; the
  // mark_saved() call back is what lets the server clear its "unsaved
  // changes" state and finish a pending close).
  ed->onEvent("on_source_saved"_key, [&s, ed, state, local_path](dynamic) {
    std::string name;
    if (auto lock = state->rlock(); lock)
      name = lock->sandbox_name;
    if (name.empty())
      return;
    try {
      auto data = s.download_file(name).get();
      write_local_file(local_path, data);
      {
        auto lock = state->wlock();
        lock->last_uploaded_content = data;
      }
      ed->call("mark_saved"_key, dynamic{}).get();
    } catch (const std::exception&) {
      // Session ending; nothing to persist.
    }
  });

  ed->onEvent("closed"_key, [&s, stop](dynamic) {
    stop->store(true, std::memory_order_relaxed);
    s.signal_done();
  });

  // `ed`, `state`, and `stop` stay alive via the shared_ptrs captured above
  // and below -- no separate keep_alive() call needed, mirroring notepad's
  // `notepad` proxy.
  std::thread([&s, ed, state, stop, local_path] {
    using namespace std::chrono_literals;
    std::error_code ec;
    auto last_write = fs::exists(local_path, ec) ? fs::last_write_time(local_path, ec) : fs::file_time_type{};

    while (!stop->load(std::memory_order_relaxed)) {
      for (int i = 0; i < 5 && !stop->load(std::memory_order_relaxed); ++i)
        std::this_thread::sleep_for(100ms);
      if (stop->load(std::memory_order_relaxed))
        break;

      if (!fs::exists(local_path, ec) || ec)
        continue;
      auto write_time = fs::last_write_time(local_path, ec);
      if (ec || write_time == last_write)
        continue;
      last_write = write_time;

      auto data = read_local_file(local_path);
      if (auto lock = state->rlock(); lock && data == lock->last_uploaded_content)
        continue; // our own write-back landing back on disk; not an external edit

      if (stop->load(std::memory_order_relaxed))
        break;
      try {
        push_local_file(s, ed, state, local_path);
      } catch (const std::exception&) {
        // Session torn down mid-poll; stop.
        break;
      }
    }
  }).detach();

  // on_session() blocks until signal_done() is called.
}

namespace {
struct editor_app_registrar {
  editor_app_registrar() {
    register_app({
        .name = "editor",
        .organization = WISH_MODULE_BDG_DEV_EDITOR_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DEV_EDITOR_COLLECTION,
        .description = "Live JSON UI mock editor -- edit, preview, and watch events in real time",
        .params = {{"file", "Path to the JSON UI file to edit"}},
        .run = run_editor,
    });
  }
};
const editor_app_registrar editor_app_registrar_instance;
} // namespace

} // namespace bdg::wish
