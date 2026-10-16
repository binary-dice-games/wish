// MIT License © 2026 Binary Dice Games
/// @file mpv.cpp
/// @brief Client-side runner for the mpv embedded app.
///
/// The Mpv form (server-side) owns the window and the sandbox library. This
/// runner is the client half:
///   - lists the client's local directories for the form's Local Machine
///     list (`on_local_navigate` -> `update_local_listing()`);
///   - uploads the chosen files into the sandbox (`on_upload_requested`), as
///     jobs on a `common::command_worker` with the shared progress dialog;
///   - opens and drives the media player through the client media API
///     (`wish_app_host::open_media()`, `media_player`): the server decodes
///     and plays the file, the client only sends commands;
///   - polls the player's status a few times a second for the form's seek
///     bar and labels (`update_status()`).
///
/// Media commands run on a second worker, so a long upload never delays a
/// Pause click.
#include "modules/bdg/desktop/mpv/client/mpv.hpp"

#include "modules/bdg/common/command_worker.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/media_player.hpp"
#include "src/client/wish_app_host.hpp"
#include "src/i18n/translations.hpp"

#include "src/bison/bison.hpp"
#include "src/bison/bison_sync.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace bdg::wish {

using namespace bison;

namespace {

namespace fs = std::filesystem;
using proxy_ptr = std::shared_ptr<rmi::proxy::dynamic>;

// The open player, shared by the media worker (opens/drives it) and the
// status thread (polls it).
using player_slot = bison::synchronized<std::optional<media_player>>;

std::vector<std::string> read_names(const dynamic& payload) {
  std::vector<std::string> names;
  if (auto* names_f = payload.findField<dynamic_ptr>("names"_key); names_f && *names_f) {
    (*names_f)->forEach([&](key_t, const field& f) {
      if (f.is<std::string>())
        names.push_back(f.as<std::string>());
    });
  }
  return names;
}

std::string read_local_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    throw std::runtime_error("cannot open " + path.string());
  std::string data(static_cast<size_t>(in.tellg()), '\0');
  in.seekg(0);
  if (!in.read(data.data(), static_cast<std::streamsize>(data.size())))
    throw std::runtime_error("cannot read " + path.string());
  return data;
}

std::string format_bytes(uintmax_t bytes) {
  static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << " " << kUnits[unit];
  return oss.str();
}

// Shows @p text in the form's status line without waiting for the reply:
// for the client's event thread, which must never block on an RMI reply.
void post_status(const proxy_ptr& form, const std::string& text, bool error) {
  dynamic patch;
  patch["status"_key] = text;
  patch["status_error"_key] = error;
  try {
    (void)form->set(std::move(patch));
  } catch (const std::exception&) {
  }
}

// Shows @p text (a translation key or make_message()) in the form's status line.
void set_status(const proxy_ptr& form, const std::string& text, bool error) {
  dynamic patch;
  patch["status"_key] = text;
  patch["status_error"_key] = error;
  try {
    form->set(std::move(patch)).get();
  } catch (const std::exception&) {
    // the form is gone (window closed)
  }
}

// Sends @p dir's entries (directories and files; the form keeps the media
// files) to update_local_listing().
void report_local_listing(const proxy_ptr& form, const fs::path& dir) {
  dynamic files;
  size_t i = 0;
  auto add = [&](const std::string& name, bool is_dir, const std::string& size) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = name;
    (*e)["type"_key] = is_dir ? std::string{"dir"} : std::string{"file"};
    (*e)["size"_key] = size;
    files[i++] = dynamic_ptr{e};
  };
  if (dir.has_parent_path() && dir != dir.root_path())
    add("..", true, {});
  std::error_code ec;
  for (auto& dirent : fs::directory_iterator{dir, fs::directory_options::skip_permission_denied, ec}) {
    const bool is_dir = dirent.is_directory(ec);
    const auto bytes = is_dir ? 0 : dirent.file_size(ec);
    add(dirent.path().filename().string(), is_dir, is_dir || ec ? std::string{} : format_bytes(bytes));
  }
  dynamic args;
  args["path"_key] = dir.string();
  args["files"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(files))};
  form->call("update_local_listing"_key, std::move(args)).get();
}

struct transfer_cancelled : std::runtime_error {
  transfer_cancelled() : std::runtime_error("cancelled") {}
};

// Opens sandbox file @p path as player @p name and plays it, replacing the
// open player. Media worker thread only.
void open_player(wish_app_host& s, const proxy_ptr& form, const std::shared_ptr<player_slot>& slot,
    const std::string& name, const std::string& path, bool loop, float volume) {
  {
    auto open = slot->wlock();
    if (*open) {
      try {
        (*open)->close().get();
      } catch (const std::exception&) {
      }
      open->reset();
    }
  }
  media_options options;
  options.autoplay = true;
  options.loop = loop;
  options.volume = volume;
  try {
    auto player = s.open_media(name, path, options).get();
    player.on_error([form](const std::string& message) { post_status(form, message, true); });
    *slot->wlock() = std::move(player);
    set_status(form, make_message("$$MPV_NOW_PLAYING", {fs::path(path).filename().string()}), false);
  } catch (const std::exception& e) {
    set_status(form, make_message("$$MPV_OPEN_FAILED", {path, e.what()}), true);
  }
}

} // namespace

void run_mpv(wish_app_host& s) {
  auto form = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "Mpv"_key).get());
  auto cur_dir =
      std::make_shared<fs::path>(s.app_args().empty() ? fs::current_path() : fs::path(s.app_args().front()));
  auto slot = std::make_shared<player_slot>();
  auto stop = std::make_shared<std::atomic<bool>>(false);

  // Transfers and local listings (with the progress dialog), and media
  // commands, on two workers: never inside an event handler (in standalone
  // mode that runs on the RMI dispatch thread holding the session lock).
  auto files = std::make_shared<common::command_worker>(s, "$$MPV_TITLE");
  auto media = std::make_shared<common::command_worker>(s, "$$MPV_TITLE");
  files->start();
  media->start();

  files->on(*form, "on_local_navigate"_key, [form, cur_dir](const dynamic& payload) {
    const auto name = payload.get_as<std::string>("name"_key, "");
    const auto type = payload.get_as<std::string>("type"_key, "");
    fs::path target = type == "path" ? fs::path(name) : (name == ".." ? cur_dir->parent_path() : (*cur_dir / name));
    std::error_code ec;
    if (!fs::is_directory(target, ec)) {
      set_status(form, make_message("$$MPV_NOT_A_FOLDER", {target.string()}), true);
      return;
    }
    *cur_dir = fs::absolute(target, ec);
    report_local_listing(form, *cur_dir);
  });

  files->on(*form, "on_upload_requested"_key, [&s, form, files, media, slot](const dynamic& payload) {
    const auto names = read_names(payload);
    const fs::path local = payload.get_as<std::string>("local_path"_key, "");
    const auto dest = payload.get_as<std::string>("dest"_key, "media");
    std::vector<std::string> failures;
    std::string first_uploaded;
    files->run_task("$$MPV_UPLOADING", [&](common::task_progress& progress) {
      for (size_t i = 0; i < names.size() && !progress.cancelled(); ++i) {
        const std::string remote = dest + "/" + names[i];
        progress.set_caption(make_message("$$MPV_UPLOADING_FILE", {names[i]}));
        try {
          auto data = read_local_file(local / names[i]);
          s.upload_file(remote, data, [&progress](std::uint64_t done, std::uint64_t total) {
             if (progress.cancelled())
               throw transfer_cancelled{};
             progress.report(done, total, format_bytes(done) + " / " + format_bytes(total));
           }).get();
          if (first_uploaded.empty())
            first_uploaded = remote;
        } catch (const transfer_cancelled&) {
        } catch (const std::exception& e) {
          failures.push_back(names[i] + ": " + e.what());
        }
      }
    });
    if (!failures.empty()) {
      std::string list;
      for (auto& f : failures)
        list += "\n" + f;
      files->fail(make_message("$$MPV_UPLOAD_FAILED", {list}), /*always_show=*/true);
    }
    try {
      form->call("refresh_library"_key, dynamic{}).get();
    } catch (const std::exception&) {
    }
    if (payload.get_as<bool>("play"_key, false) && !first_uploaded.empty()) {
      media->post([&s, form, slot, first_uploaded, name = payload.get_as<std::string>("player"_key, "mpv"),
                      loop = payload.get_as<bool>("loop"_key, false),
                      volume = payload.get_as<float>("volume"_key, 1.0f)] {
        open_player(s, form, slot, name, first_uploaded, loop, volume);
      });
    }
  });

  media->on(*form, "on_open_requested"_key, [&s, form, slot](const dynamic& payload) {
    open_player(s, form, slot, payload.get_as<std::string>("player"_key, "mpv"),
        payload.get_as<std::string>("path"_key, ""), payload.get_as<bool>("loop"_key, false),
        payload.get_as<float>("volume"_key, 1.0f));
  });

  media->on(*form, "on_control"_key, [form, slot](const dynamic& payload) {
    std::optional<media_player> player = *slot->rlock();
    if (!player)
      return;
    const auto action = payload.get_as<std::string>("action"_key, "");
    const float value = payload.get_as<float>("value"_key, 0.0f);
    try {
      if (action == "play")
        player->play().get();
      else if (action == "pause")
        player->pause().get();
      else if (action == "stop")
        player->stop().get();
      else if (action == "seek")
        player->seek(value).get();
      else if (action == "volume")
        player->set_volume(value).get();
      else if (action == "loop")
        player->set_loop(value != 0.0f).get();
      else if (action == "close") {
        player->close().get();
        slot->wlock()->reset();
      }
    } catch (const std::exception& e) {
      set_status(form, e.what(), true);
    }
  });

  // The server closes the session's players when the app's session ends.
  form->onEvent("closed"_key, [&s, files, media, stop](dynamic) {
    stop->store(true);
    files->shutdown();
    media->shutdown();
    s.signal_done();
  });

  // Status for the seek bar and labels, a few times a second. Holds only
  // the proxies (like top's sampling thread), never the host.
  std::thread([form, slot, stop] {
    using namespace std::chrono_literals;
    while (!stop->load()) {
      std::optional<media_player> player = *slot->rlock();
      if (player && player->valid()) {
        try {
          const auto st = player->status().get();
          dynamic args;
          args["state"_key] = static_cast<int32_t>(st.state);
          args["position"_key] = static_cast<float>(st.position);
          args["duration"_key] = static_cast<float>(st.duration);
          args["has_video"_key] = st.has_video;
          args["has_audio"_key] = st.has_audio;
          args["audio_output"_key] = st.audio_output;
          form->call("update_status"_key, std::move(args)).get();
        } catch (const std::exception&) {
          // A player closed meanwhile is fine; a gone form ends the loop.
          if (stop->load())
            break;
        }
      }
      std::this_thread::sleep_for(250ms);
    }
  }).detach();

  files->post([&s, form, cur_dir] {
    if (!s.has_media())
      form->call("media_unavailable"_key, dynamic{}).get();
    report_local_listing(form, *cur_dir);
  });

  // on_session() blocks until signal_done() is called.
}

namespace {
struct mpv_app_registrar {
  mpv_app_registrar() {
    register_app({
        .name = "mpv",
        .organization = WISH_MODULE_BDG_DESKTOP_MPV_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DESKTOP_MPV_COLLECTION,
        .description = "Media player: upload local video/audio files to the sandbox and play them on the server",
        .icon = "res/icons/audio.png",
        .params = {{"folder", "Local folder to list at startup (default: the current directory)"}},
        .run = run_mpv,
    });
  }
};
const mpv_app_registrar mpv_app_registrar_instance;
} // namespace

} // namespace bdg::wish
