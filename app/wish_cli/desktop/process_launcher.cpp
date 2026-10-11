// MIT License © 2026 Binary Dice Games
/**
 * @file process_launcher.cpp
 * @brief process_launcher implementation on a private libuv loop.
 */
#include "app/wish_cli/desktop/process_launcher.hpp"

#include "src/bison/bison_sync.hpp"

#include <uv.h>

#include <csignal>
#include <deque>
#include <future>
#include <map>
#include <thread>

namespace bdg::wish {

namespace {

/// A spawn handed to the loop thread, answered through @p result.
struct pending_spawn {
  launch_request request;
  std::promise<std::pair<std::optional<int>, std::string>> result;
};

struct queue_state {
  std::deque<pending_spawn> spawns;
  bool terminate{false};
  bool stop{false};
};

} // namespace

struct process_launcher::impl {
  uv_loop_t loop{};
  uv_async_t wake{};
  std::thread thread;
  bison::synchronized<queue_state> queue;
  /// pid -> handle; only touched on the loop thread.
  std::map<int, uv_process_t*> children;
  /// Snapshot of `children`'s pids for running(), from any thread.
  bison::synchronized<std::vector<int>> pids;

  impl() {
    uv_loop_init(&loop);
    loop.data = this;
    wake.data = this;
    uv_async_init(&loop, &wake, [](uv_async_t* a) { static_cast<impl*>(a->data)->drain(); });
    thread = std::thread([this] { uv_run(&loop, UV_RUN_DEFAULT); });
  }

  ~impl() {
    queue.wlock()->stop = true;
    uv_async_send(&wake);
    thread.join();
    uv_loop_close(&loop);
  }

  void publish_pids() {
    std::vector<int> v;
    v.reserve(children.size());
    for (const auto& [pid, h] : children)
      v.push_back(pid);
    *pids.wlock() = std::move(v);
  }

  static void on_exit(uv_process_t* proc, int64_t, int) {
    auto* self = static_cast<impl*>(proc->loop->data);
    self->children.erase(proc->pid);
    self->publish_pids();
    uv_close(reinterpret_cast<uv_handle_t*>(proc), [](uv_handle_t* h) { delete reinterpret_cast<uv_process_t*>(h); });
  }

  void kill_all() {
    for (const auto& [pid, proc] : children)
      uv_process_kill(proc, SIGTERM);
  }

  /// Loop thread: runs every queued spawn/terminate/stop request.
  void drain() {
    queue_state work;
    {
      auto q = queue.wlock();
      work.spawns = std::move(q->spawns);
      q->spawns.clear();
      work.terminate = std::exchange(q->terminate, false);
      work.stop = q->stop;
    }
    for (auto& s : work.spawns)
      s.result.set_value(spawn(s.request));
    if (work.terminate || work.stop)
      kill_all();
    if (work.stop) {
      // Children still running keep going until the SIGTERM above takes
      // effect; their handles are closed without waiting for their exit.
      for (const auto& [pid, proc] : children)
        uv_close(
            reinterpret_cast<uv_handle_t*>(proc), [](uv_handle_t* h) { delete reinterpret_cast<uv_process_t*>(h); });
      children.clear();
      publish_pids();
      uv_close(reinterpret_cast<uv_handle_t*>(&wake), nullptr);
    }
  }

  std::pair<std::optional<int>, std::string> spawn(const launch_request& req) {
    if (req.argv.empty() || req.argv[0].empty())
      return {std::nullopt, "no program to run"};

    std::vector<std::string> args = req.argv;
    std::vector<char*> c_args;
    for (auto& a : args)
      c_args.push_back(a.data());
    c_args.push_back(nullptr);

    // Environment: this process's own, with the request's variables set.
    std::map<std::string, std::string> env_map;
    uv_env_item_t* items = nullptr;
    int count = 0;
    if (uv_os_environ(&items, &count) == 0) {
      for (int i = 0; i < count; ++i)
        env_map[items[i].name] = items[i].value;
      uv_os_free_environ(items, count);
    }
    for (const auto& [k, v] : req.env)
      env_map[k] = v;
    std::vector<std::string> env_strings;
    env_strings.reserve(env_map.size());
    for (const auto& [k, v] : env_map)
      env_strings.push_back(k + "=" + v);
    std::vector<char*> c_env;
    for (auto& e : env_strings)
      c_env.push_back(e.data());
    c_env.push_back(nullptr);

    // stdout/stderr go to the log file, never to this process's terminal.
    uv_file log_fd = -1;
    if (!req.log_path.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(req.log_path.parent_path(), ec);
      uv_fs_t open_req;
      log_fd = uv_fs_open(
          nullptr,
          &open_req,
          req.log_path.string().c_str(),
          UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC,
          0644,
          nullptr);
      uv_fs_req_cleanup(&open_req);
    }
    uv_stdio_container_t stdio[3];
    stdio[0].flags = UV_IGNORE;
    for (int i = 1; i <= 2; ++i) {
      if (log_fd >= 0) {
        stdio[i].flags = UV_INHERIT_FD;
        stdio[i].data.fd = log_fd;
      } else {
        stdio[i].flags = UV_IGNORE;
      }
    }

    uv_process_options_t options{};
    options.exit_cb = &impl::on_exit;
    options.file = args[0].c_str();
    options.args = c_args.data();
    options.env = c_env.data();
    options.stdio_count = 3;
    options.stdio = stdio;
    options.flags = UV_PROCESS_WINDOWS_HIDE;

    auto* proc = new uv_process_t;
    int rc = uv_spawn(&loop, proc, &options);
    if (log_fd >= 0) {
      uv_fs_t close_req;
      uv_fs_close(nullptr, &close_req, log_fd, nullptr);
      uv_fs_req_cleanup(&close_req);
    }
    if (rc != 0) {
      // A handle uv_spawn() failed on is still registered with the loop and
      // must be closed, not just freed (see modules/bdg/common/process.cpp).
      uv_close(reinterpret_cast<uv_handle_t*>(proc), [](uv_handle_t* h) { delete reinterpret_cast<uv_process_t*>(h); });
      return {std::nullopt, uv_strerror(rc)};
    }
    children[proc->pid] = proc;
    publish_pids();
    return {proc->pid, {}};
  }
};

process_launcher::process_launcher() : impl_(std::make_unique<impl>()) {}

process_launcher::~process_launcher() = default;

std::optional<int> process_launcher::launch(const launch_request& request, std::string& error) {
  std::future<std::pair<std::optional<int>, std::string>> result;
  {
    auto q = impl_->queue.wlock();
    if (q->stop) {
      error = "launcher stopped";
      return std::nullopt;
    }
    q->spawns.push_back(pending_spawn{request, {}});
    result = q->spawns.back().result.get_future();
  }
  uv_async_send(&impl_->wake);
  auto [pid, err] = result.get();
  error = err;
  return pid;
}

void process_launcher::terminate_all() {
  impl_->queue.wlock()->terminate = true;
  uv_async_send(&impl_->wake);
}

std::vector<int> process_launcher::running() const {
  return *impl_->pids.rlock();
}

std::filesystem::path process_launcher::self_exe_path() {
  char buf[4096];
  size_t size = sizeof(buf);
  if (uv_exepath(buf, &size) != 0)
    return {};
  return std::filesystem::path(std::string(buf, size));
}

} // namespace bdg::wish
