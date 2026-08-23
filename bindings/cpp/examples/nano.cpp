// MIT License © 2025 Binary Dice Games
//
// Nano example using the header-only C++ wish client binding.
//
// Port of bindings/python/examples/nano_example.py (itself a port of
// modules/bdg/desktop/nano/client/nano.cpp): the nano form (server-side) never
// touches this process's local filesystem -- it only edits files already
// sitting in its session sandbox. This program is the bridge: it reacts to
// the form's high-level events by driving its own local files through
// client::upload_file() / download_file().
//
// Usage: nano_cpp [--transport=tcp|pipe|term] [--host=HOST] [--port=PORT]
//                     [--name=PATH] [--theme=dark|light|classic] [--verbose] [file]

#include <wish_cpp/wish.hpp>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace wish = bdg::wish::binding;
using namespace bdg::wish::binding;  // for the "_key" literal operator
namespace fs = std::filesystem;

namespace {

std::string read_local_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_local_file(const fs::path& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      default: out += c; break;
    }
  }
  return out;
}

// Builds the `files` array expected by FileDialog from a directory listing.
// Mirrors list_directory() in modules/bdg/desktop/nano/client/nano.cpp: the C ABI
// has no bison_set_object_at(), so the array is built by round-tripping
// through JSON (bison_from_json() only accepts a top-level object, so the
// array is wrapped in one and the "items" field is projected back out).
wish::value files_array(const fs::path& directory) {
  std::ostringstream json;
  json << R"({"items":[)";
  bool first = true;
  auto add_entry = [&](const std::string& name, const char* type) {
    if (!first) json << ",";
    first = false;
    json << R"({"name":")" << json_escape(name) << R"(","type":")" << type << R"("})";
  };
  if (directory.has_parent_path() && directory.parent_path() != directory) add_entry("..", "dir");
  std::error_code ec;
  std::vector<fs::directory_entry> entries;
  for (auto& entry : fs::directory_iterator(directory, ec)) entries.push_back(entry);
  std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.path().filename() < b.path().filename(); });
  for (auto& entry : entries) add_entry(entry.path().filename().string(), entry.is_directory() ? "dir" : "file");
  json << "]}";
  return *wish::value::parse_json(json.str()).get_object("items"_key);
}

// Tracks the sandbox name <-> local path mapping for files this client has
// uploaded, picking a sandbox name that does not collide with one already in
// use (e.g. two different directories each containing a "notes.txt").
class sandbox_files {
 public:
  std::string reserve_name(const fs::path& local_path) {
    std::string candidate = local_path.filename().string();
    int suffix = 1;
    while (local_path_by_sandbox_name_.count(candidate)) {
      candidate = local_path.stem().string() + "_" + std::to_string(suffix++) + local_path.extension().string();
    }
    return candidate;
  }

  std::map<std::string, fs::path> local_path_by_sandbox_name_;
};

// Uploads a local file's current contents into the sandbox under a fresh
// name and registers it as a new nano tab. Shared by the FileDialog-driven
// Open/New flows and by opening a file passed on the command line.
void upload_and_open(wish::client& client, wish::proxy& nano, sandbox_files& files, const fs::path& local_path) {
  std::string data = read_local_file(local_path);
  std::string sandbox_name = files.reserve_name(local_path);
  client.upload_file(sandbox_name, data);
  files.local_path_by_sandbox_name_[sandbox_name] = local_path;
  wish::value params;
  params["path"_key] = sandbox_name;
  params["title"_key] = local_path.filename().string();
  nano.call("open_file"_key, params);
}

// Shared by "Open" and "New": shows a FileDialog populated from a local
// directory listing, then uploads the chosen file and registers it via
// open_file(). `create_if_missing` is set for "New", where the chosen path
// need not already exist locally.
void browse_and_open(
    wish::client& client,
    wish::proxy& nano,
    sandbox_files& files,
    const std::string& title,
    const std::string& confirm_label,
    bool create_if_missing) {
  auto cur_dir = std::make_shared<fs::path>(fs::current_path());

  auto make_init = [&](const fs::path& directory) {
    wish::value init;
    init["title"_key] = title;
    init["confirm_label"_key] = confirm_label;
    init["path"_key] = directory.string();
    init["files"_key] = files_array(directory);
    return init;
  };

  auto dlg = std::make_shared<wish::proxy>(client.instantiate("FileDialog"_key, "wish"_key));
  dlg->set(make_init(*cur_dir));

  dlg->on_event("on_navigate"_key, [dlg, cur_dir, make_init](wish::value payload) {
    std::string name = payload.get_string("name"_key).value_or("");
    std::string kind = payload.get_string("type"_key).value_or("");
    if (kind == "path")
      *cur_dir = fs::path(name);
    else
      *cur_dir = (name == "..") ? cur_dir->parent_path() : (*cur_dir / name);
    dlg->set(make_init(*cur_dir));
  });

  dlg->on_event(
      "on_open"_key,
      [&client, &nano, &files, cur_dir, create_if_missing](wish::value payload) {
        std::string name = payload.get_string("path"_key).value_or("");
        fs::path local_path(name);
        if (!local_path.is_absolute()) local_path = *cur_dir / local_path;

        // "New": the user typed/picked a path that may not exist yet -- create
        // it empty. If it already exists (e.g. they picked an existing file by
        // mistake), leave its content alone rather than truncating it.
        if (create_if_missing && !fs::exists(local_path)) write_local_file(local_path, "");

        upload_and_open(client, nano, files, local_path);
      });

  dlg->on_event("on_cancel"_key, [](wish::value) {});
}

void run_nano(wish::client& client, const std::optional<fs::path>& startup_path) {
  auto nano = client.instantiate("Nano"_key, "wish"_key);
  sandbox_files files;

  // "Open" clicked: the server has no view of the client's local files, so
  // it asks us to present our own picker.
  nano.on_event("on_request_open"_key, [&](wish::value) {
    browse_and_open(client, nano, files, "Open File", "Open", /*create_if_missing=*/false);
  });

  // "New" clicked: same handshake, but the chosen path need not exist yet.
  nano.on_event("on_request_new"_key, [&](wish::value) {
    browse_and_open(client, nano, files, "New File", "New", /*create_if_missing=*/true);
  });

  // A tab (or the whole window) closed: download this file one last time and
  // forget our local bookkeeping for it.
  nano.on_event("on_file_closed"_key, [&](wish::value payload) {
    std::string path = payload.get_string("path"_key).value_or("");
    auto it = files.local_path_by_sandbox_name_.find(path);
    if (it == files.local_path_by_sandbox_name_.end()) return;
    write_local_file(it->second, client.download_file(path));
    files.local_path_by_sandbox_name_.erase(it);
  });

  // Ctrl+S inside a tab, or the "Save" button clicked for the active tab:
  // download that one file, keep it open.
  nano.on_event("on_file_saved"_key, [&](wish::value payload) {
    std::string path = payload.get_string("path"_key).value_or("");
    auto it = files.local_path_by_sandbox_name_.find(path);
    if (it != files.local_path_by_sandbox_name_.end()) write_local_file(it->second, client.download_file(path));
  });

  // The window's title-bar X was clicked while one or more tabs still had
  // unsaved changes: the server held the close open and is asking whether
  // to save them first. Confirm via the built-in MessageBox form, then
  // report the answer back so the server can actually finish closing.
  // "Cancel" simply never calls confirm_close() -- the window was never
  // actually removed from top_level_objects while waiting on this prompt,
  // so leaving it unanswered leaves it open exactly as it was. The
  // MessageBox proxy is kept alive by capturing it in its own on_event
  // handler -- same idiom as `dlg` in browse_and_open() above.
  nano.on_event("on_confirm_close"_key, [&](wish::value payload) {
    size_t count = 0;
    if (auto paths = payload.get_object("paths"_key)) count = paths->size();

    wish::value params;
    params["title"_key] = std::string{"Unsaved Changes"};
    params["message"_key] = count == 1 ? "1 file has unsaved changes. Save it before closing?"
                                        : std::to_string(count) + " files have unsaved changes. Save them before closing?";
    params["icon"_key] = std::string{"question"};
    params["buttons"_key] = std::string{"yes_no_cancel"};
    auto mb = std::make_shared<wish::proxy>(client.instantiate("MessageBox"_key, "wish"_key, params));
    mb->on_event("on_result"_key, [&nano, mb](wish::value result) {
      auto button = result.get_string("button"_key).value_or("");
      if (button == "cancel") return;
      wish::value args;
      args["save"_key] = button == "yes";
      nano.call("confirm_close"_key, args);
    });
  });

  nano.on_event("closed"_key, [&client](wish::value) { client.quit(); });

  // A file to open at startup may be passed on the command line.
  if (startup_path) {
    if (fs::exists(*startup_path))
      upload_and_open(client, nano, files, *startup_path);
    else
      std::cerr << "[nano] no such file: " << startup_path->string() << "\n";
  }

  client.wait();
}

wish::client* g_client_for_signal = nullptr;
void handle_sigint(int) {
  if (g_client_for_signal) g_client_for_signal->quit();
}

struct cli_args {
  std::string transport = "tcp";
  std::string host = "127.0.0.1";
  uint16_t port = 7070;
  std::string name;
  std::string theme = "wish";
  bool verbose = false;
  std::optional<fs::path> file;
};

cli_args parse_args(int argc, char* argv[]) {
  cli_args args;
  auto value_of = [&](const std::string& arg, const std::string& flag) -> std::optional<std::string> {
    std::string prefix = flag + "=";
    if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    return std::nullopt;
  };
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (auto v = value_of(arg, "--transport")) args.transport = *v;
    else if (auto v = value_of(arg, "--host")) args.host = *v;
    else if (auto v = value_of(arg, "--port")) args.port = static_cast<uint16_t>(std::stoi(*v));
    else if (auto v = value_of(arg, "--name")) args.name = *v;
    else if (auto v = value_of(arg, "--theme")) args.theme = *v;
    else if (arg == "--verbose") args.verbose = true;
    else if (arg.rfind("--", 0) != 0) args.file = fs::absolute(fs::path(arg));
  }
  return args;
}

}  // namespace

int main(int argc, char* argv[]) {
  cli_args args = parse_args(argc, argv);

  wish::client client = [&] {
    if (args.transport == "tcp") {
      std::cout << "[Client] connecting to " << args.host << ":" << args.port << " ...\n";
      return wish::client::tcp(args.host, args.port);
    }
    if (args.transport == "pipe") {
      std::cout << "[Client] connecting to pipe " << args.name << " ...\n";
      return wish::client::pipe(args.name);
    }
    std::cout << "[Client] connecting via inherited stdio (--transport=term) ...\n";
    return wish::client::term();
  }();

  g_client_for_signal = &client;
  std::signal(SIGINT, handle_sigint);

  try {
    client.run([&](wish::client& c) {
      c.set_style_preset(args.theme);
      run_nano(c, args.file);
    });
  } catch (const std::exception& e) {
    std::cerr << "[Client] error: " << e.what() << "\n";
    return EXIT_FAILURE;
  }

  std::cout << "[Client] done.\n";
  return EXIT_SUCCESS;
}
