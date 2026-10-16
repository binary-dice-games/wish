// MIT License © 2026 Binary Dice Games
/// @file mpv.cpp
/// @brief Implementation of the mpv media player form.
#include "mpv.hpp"

#include "modules/bdg/common/server/list_panel.hpp"
#include "modules/bdg/common/server/ui_helpers.hpp"

#include <context/context.hpp>
#include <context/file_service.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace bdg::wish {

using namespace bison;
using common::make_payload;
using common::wish_id_of;

namespace {

namespace fs = std::filesystem;

// Sandbox folder uploads go to.
constexpr const char* kMediaDir = "media";

// How long update_status() leaves the seek bar alone after the user moved it.
constexpr std::chrono::milliseconds kSeekQuiet{1200};

// The window: a Splitter with the file lists on the left (the local machine
// above the sandbox library) and the player on the right. The Video and both tables take "height": -1, so their
// VerticalLayout gives them whatever is left after the other rows.
// "unavailable" is shown instead of the player on a server without media
// support. Rows of both tables are built at runtime (fill_local(),
// fill_library()).
constexpr const char* kLayout = R"json({
  "type": "Window",
  "title": "$$MPV_TITLE",
  "width": 1040, "height": 640,
  "closable": true, "flags": "NoScrollbar|NoScrollWithMouse",
  "children": {
    "split": {
      "type": "Splitter", "thickness": 6, "min_pane_size": 220,
      "children": {
        "files": {
          "type": "VerticalLayout", "spacing": 4, "width": 400,
          "children": {
            "local_heading": { "type": "SeparatorText", "label": "$$MPV_LOCAL_MACHINE" },
            "path_row": {
              "type": "HorizontalLayout", "spacing": 4,
              "children": {
                "up": { "type": "Button", "label": "", "icon": "res/icons/arrow_up.png", "tooltip": "$$MPV_UP" },
                "path": { "type": "InputText", "hint": "$$MPV_LOCAL_PATH", "value": "", "flags": "EnterReturnsTrue", "width": -1 }
              }
            },
            "local_table": {
              "type": "Table", "id": "##mpv_local", "columns": 2, "headers": true,
              "flags": "Resizable|RowBg|Borders|ScrollY", "height": 230, "outer_height": 230, "auto_scroll": false,
              "children": {
                "col_name": { "type": "TableColumn", "label": "$$MPV_NAME" },
                "col_size": { "type": "TableColumn", "label": "$$MPV_SIZE", "flags": "WidthFixed", "init_width": 80 }
              }
            },
            "local_actions": {
              "type": "HorizontalLayout", "spacing": 6,
              "children": {
                "upload": { "type": "Button", "label": "$$MPV_UPLOAD", "icon": "res/icons/upload.png", "tooltip": "$$MPV_UPLOAD_TOOLTIP" },
                "upload_play": { "type": "Button", "label": "$$MPV_UPLOAD_PLAY", "icon": "res/icons/play.png", "tooltip": "$$MPV_UPLOAD_PLAY_TOOLTIP" },
                "selected": { "type": "Label", "text": "" }
              }
            },
            "library_heading": { "type": "SeparatorText", "label": "$$MPV_LIBRARY" },
            "library_table": {
              "type": "Table", "id": "##mpv_library", "columns": 2, "headers": true,
              "flags": "Resizable|RowBg|Borders|ScrollY", "height": -1, "auto_scroll": false,
              "children": {
                "col_name": { "type": "TableColumn", "label": "$$MPV_NAME" },
                "col_size": { "type": "TableColumn", "label": "$$MPV_SIZE", "flags": "WidthFixed", "init_width": 80 }
              }
            },
            "library_actions": {
              "type": "HorizontalLayout", "spacing": 6,
              "children": {
                "play": { "type": "Button", "label": "$$MPV_PLAY_SELECTED", "icon": "res/icons/play.png" },
                "remove": { "type": "Button", "label": "$$MPV_REMOVE", "icon": "res/icons/delete.png" },
                "refresh": { "type": "Button", "label": "", "icon": "res/icons/refresh.png", "tooltip": "$$MPV_REFRESH", "shortcut": "F5" },
                "count": { "type": "Label", "text": "" }
              }
            }
          }
        },
        "player": {
          "type": "VerticalLayout", "spacing": 6,
          "children": {
            "now_playing": { "type": "Label", "text": "$$MPV_NOTHING_OPEN" },
            "unavailable": { "type": "Label", "text": "$$MPV_UNAVAILABLE", "visible": false, "wrap": true },
            "video": { "type": "Video", "player": "", "width": -1, "height": -1 },
            "seek": { "type": "SliderFloat", "label": "##seek", "value": 0.0, "min": 0.0, "max": 1.0, "format": "", "width": -1 },
            "controls": {
              "type": "HorizontalLayout", "spacing": 12,
              "children": {
                "play": { "type": "Button", "label": "$$MPV_PLAY", "icon": "res/icons/play.png", "width": 96 },
                "stop": { "type": "Button", "label": "$$MPV_STOP", "icon": "res/icons/stop.png" },
                "time": { "type": "Label", "text": "0:00 / 0:00" },
                "loop": { "type": "Checkbox", "label": "$$MPV_LOOP", "value": false },
                "volume_label": { "type": "Label", "text": "$$MPV_VOLUME" },
                "volume": { "type": "SliderFloat", "label": "##volume", "value": 1.0, "min": 0.0, "max": 1.0, "format": "%.2f", "width": 140 }
              }
            },
            "status": { "type": "Label", "text": "$$MPV_READY" }
          }
        }
      }
    }
  }
})json";

std::string read_size_label(uintmax_t bytes) {
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

std::string base_name(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

// ── Static helpers ───────────────────────────────────────────────────────────

bool mpv::is_media_name(const std::string& name) {
  static const std::set<std::string> kExtensions{"mp4", "m4v", "mkv", "webm", "mov", "avi", "mpg", "mpeg", "ogv",
      "wmv", "flv", "ts", "mp3", "wav", "flac", "ogg", "oga", "opus", "m4a", "aac", "wma"};
  const auto dot = name.find_last_of('.');
  if (dot == std::string::npos || dot + 1 == name.size())
    return false;
  return kExtensions.count(common::lower(name.substr(dot + 1))) != 0;
}

std::vector<std::pair<std::string, uintmax_t>> mpv::scan_library(const fs::path& root) {
  std::vector<std::pair<std::string, uintmax_t>> out;
  std::error_code ec;
  fs::recursive_directory_iterator it{root, fs::directory_options::skip_permission_denied, ec}, end;
  for (; !ec && it != end; it.increment(ec)) {
    const auto rel = fs::relative(it->path(), root, ec);
    if (ec)
      break;
    const std::string first = rel.begin() != rel.end() ? rel.begin()->string() : std::string{};
    if (it->is_directory(ec)) {
      // The session's built-in resources and tools' private folders are
      // not the user's media; deep trees are not worth walking.
      if ((it.depth() == 0 && (first == "res" || first == "private" || first == "url_cache")) || it.depth() >= 4)
        it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec) || !is_media_name(it->path().filename().string()))
      continue;
    out.emplace_back(rel.generic_string(), it->file_size(ec));
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string mpv::format_time(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0)
    seconds = 0;
  const auto total = static_cast<long>(seconds);
  char buf[32];
  if (total >= 3600)
    std::snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", total / 3600, (total / 60) % 60, total % 60);
  else
    std::snprintf(buf, sizeof(buf), "%ld:%02ld", total / 60, total % 60);
  return buf;
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

mpv::mpv(dynamic&& base) : tool_form(std::move(base)) {
  i18n_prefix_ = "bdg/desktop/mpv";
}

mpv::~mpv() = default;

void mpv::on_init() {
  internal_root_key_ = next_available_key("__mpv_");
  // Unique per form, so two players in one session don't share a texture.
  player_name_ = "bdg.desktop.mpv." + internal_root_key_;
  (*this)["player"_key] = player_name_;

  build_window(internal_root_key_, kLayout, window_id_, [&](ui_tree& tree) {
    const std::string L = "split.files.";
    const std::string B = "split.files.";
    const std::string P = "split.player.";
    tree.with(L + "path_row.up", [&](const auto& e) { up_id_ = wish_id_of(e); });
    tree.with(L + "path_row.path", [&](const auto& e) {
      local_path_ = e;
      local_path_id_ = wish_id_of(e);
    });
    tree.with(L + "local_table", [&](const auto& e) {
      local_table_ = e;
      local_table_id_ = wish_id_of(e);
    });
    tree.with(L + "local_actions.upload", [&](const auto& e) { upload_id_ = wish_id_of(e); });
    tree.with(L + "local_actions.upload_play", [&](const auto& e) { upload_play_id_ = wish_id_of(e); });
    tree.with(L + "local_actions.selected", [&](const auto& e) { local_selected_ = e; });
    tree.with(B + "library_table", [&](const auto& e) {
      library_table_ = e;
      library_table_id_ = wish_id_of(e);
    });
    tree.with(B + "library_actions.play", [&](const auto& e) { lib_play_id_ = wish_id_of(e); });
    tree.with(B + "library_actions.remove", [&](const auto& e) { lib_remove_id_ = wish_id_of(e); });
    tree.with(B + "library_actions.refresh", [&](const auto& e) { lib_refresh_id_ = wish_id_of(e); });
    tree.with(B + "library_actions.count", [&](const auto& e) { library_count_ = e; });
    tree.with(P + "now_playing", [&](const auto& e) { now_playing_ = e; });
    tree.with(P + "unavailable", [&](const auto& e) { unavailable_ = e; });
    tree.with(P + "video", [&](const auto& e) {
      video_ = e;
      video_id_ = wish_id_of(e);
    });
    tree.with(P + "seek", [&](const auto& e) {
      seek_ = e;
      seek_id_ = wish_id_of(e);
    });
    tree.with(P + "controls.play", [&](const auto& e) {
      play_btn_ = e;
      play_id_ = wish_id_of(e);
    });
    tree.with(P + "controls.stop", [&](const auto& e) { stop_id_ = wish_id_of(e); });
    tree.with(P + "controls.time", [&](const auto& e) { time_ = e; });
    tree.with(P + "controls.loop", [&](const auto& e) {
      loop_ = e;
      loop_id_ = wish_id_of(e);
    });
    tree.with(P + "controls.volume", [&](const auto& e) {
      volume_ = e;
      volume_id_ = wish_id_of(e);
    });
    tree.with(P + "status", [&](const auto& e) { status_ = e; });
  });
  if (video_)
    video_["player"_key] = player_name_;

  on_click(up_id_, [this] { emit("on_local_navigate"_key, make_payload("name"_key, std::string{".."}, "type"_key, std::string{"dir"})); });
  on_click(upload_id_, [this] { request_upload({local_selection_.begin(), local_selection_.end()}, false); });
  on_click(upload_play_id_, [this] { request_upload({local_selection_.begin(), local_selection_.end()}, true); });
  on_click(lib_play_id_, [this] { open_library(library_selected_); });
  on_click(lib_remove_id_, [this] { remove_selected(); });
  on_click(lib_refresh_id_, [this] { fill_library(); });
  on_click(play_id_, [this] { control(playing_ ? "pause" : "play"); });
  on_click(stop_id_, [this] { control("stop"); });
  on_click(video_id_, [this] { control(playing_ ? "pause" : "play"); });

  fill_library();
}

// ── Local machine list ───────────────────────────────────────────────────────

void mpv::clear_rows(const ui_element_ptr& table, std::vector<key_t>& ids) {
  if (!table)
    return;
  if (auto* children = table->findField<dynamic_ptr>("children"_key); children && *children) {
    // Keep the TableColumns (named keys); drop the numbered rows.
    std::vector<key_t> rows;
    (*children)->forEach([&](key_t k, const field& f) {
      if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>() && (*f.as<dynamic_ptr>())[dynamic::CLASS].is<key_t>() &&
          (*f.as<dynamic_ptr>())[dynamic::CLASS].as<key_t>() == "TableRow"_key)
        rows.push_back(k);
    });
    for (auto k : rows)
      (*children)->erase(k);
  }
  erase_objects(ids);
  ids.clear();
}

void mpv::fill_local() {
  clear_rows(local_table_, local_row_ids_);
  if (!local_table_)
    return;
  auto* children = local_table_->findField<dynamic_ptr>("children"_key);
  if (!children || !*children)
    return;
  size_t key = 0;
  for (const auto& e : local_entries_) {
    auto name = make_label(e.is_dir ? "[" + e.name + "]" : e.name);
    if (e.is_dir)
      common::set_text_color(name, common::kIdle);
    auto size = make_label(e.size);
    auto row = ui_element_ptr::create("wish"_key, "TableRow"_key);
    row["selected"_key] = local_selection_.count(e.name) != 0;
    assign_id(row);
    common::set_children_list(row, {name, size});
    local_row_ids_.insert(local_row_ids_.end(), {wish_id_of(name), wish_id_of(size), wish_id_of(row)});
    (**children)[key++] = dynamic_ptr{row};
  }
  local_table_->refresh_children_order();

  if (local_selected_) {
    const size_t n = local_selection_.size();
    local_selected_["text"_key] = n == 0 ? std::string{}
        : n == 1                         ? *local_selection_.begin()
                                         : trf("$$MPV_N_SELECTED", {std::to_string(n)});
  }
}

void mpv::select_local(int32_t index, bool ctrl, bool shift) {
  if (index < 0 || index >= static_cast<int32_t>(local_entries_.size()))
    return;
  const auto& e = local_entries_[static_cast<size_t>(index)];
  if (shift && local_anchor_ >= 0) {
    local_selection_.clear();
    for (int32_t i = std::min(local_anchor_, index); i <= std::max(local_anchor_, index); ++i)
      if (!local_entries_[static_cast<size_t>(i)].is_dir)
        local_selection_.insert(local_entries_[static_cast<size_t>(i)].name);
  } else if (ctrl) {
    if (!e.is_dir && !local_selection_.erase(e.name))
      local_selection_.insert(e.name);
    local_anchor_ = index;
  } else {
    local_selection_.clear();
    if (!e.is_dir)
      local_selection_.insert(e.name);
    local_anchor_ = index;
  }
  fill_local();
}

void mpv::activate_local(int32_t index) {
  if (index < 0 || index >= static_cast<int32_t>(local_entries_.size()))
    return;
  const auto& e = local_entries_[static_cast<size_t>(index)];
  if (e.is_dir)
    emit("on_local_navigate"_key, make_payload("name"_key, e.name, "type"_key, std::string{"dir"}));
  else
    request_upload({e.name}, true); // double-click a file: upload and play it
}

void mpv::request_upload(const std::vector<std::string>& names, bool play) {
  if (names.empty()) {
    set_status("$$MPV_SELECT_LOCAL", false);
    return;
  }
  dynamic list;
  size_t i = 0;
  for (const auto& n : names)
    list[i++] = n;
  dynamic req;
  req["names"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(list))};
  req["local_path"_key] = local_dir_;
  req["dest"_key] = std::string{kMediaDir};
  req["play"_key] = play && media_available_;
  req["player"_key] = player_name_;
  req["loop"_key] = loop_ ? loop_->get_as<bool>("value"_key, false) : false;
  req["volume"_key] = volume_ ? volume_->get_as<float>("value"_key, 1.0f) : 1.0f;
  if (play && media_available_) {
    playing_path_ = std::string{kMediaDir} + "/" + names.front();
    if (now_playing_)
      now_playing_["text"_key] = trf("$$MPV_OPENING", {names.front()});
  }
  emit("on_upload_requested"_key, std::move(req));
}

dynamic mpv::do_update_local_listing(const dynamic& args) {
  local_dir_ = common::str_of(args, "path"_key);
  local_entries_.clear();
  std::vector<local_entry> dirs, files;
  common::for_each_entry(args, "files"_key, [&](const dynamic& f) {
    local_entry e{common::str_of(f, "name"_key), common::str_of(f, "type"_key) == "dir", common::str_of(f, "size"_key)};
    if (e.is_dir)
      dirs.push_back(std::move(e));
    else if (is_media_name(e.name))
      files.push_back(std::move(e));
  });
  auto by_name = [](const local_entry& a, const local_entry& b) {
    if (a.name == "..")
      return b.name != "..";
    if (b.name == "..")
      return false;
    return common::lower(a.name) < common::lower(b.name);
  };
  std::sort(dirs.begin(), dirs.end(), by_name);
  std::sort(files.begin(), files.end(), by_name);
  local_entries_ = std::move(dirs);
  local_entries_.insert(local_entries_.end(), files.begin(), files.end());
  local_selection_.clear();
  local_anchor_ = -1;
  if (local_path_)
    local_path_["value"_key] = local_dir_;
  fill_local();
  return {};
}

// ── Library ──────────────────────────────────────────────────────────────────

void mpv::fill_library() {
  // Callable from RMI methods (dispatch) and from on_event() (not).
  run_in_dispatch([this] { library_ = scan_library(sess().resource_dir); });
  clear_rows(library_table_, library_row_ids_);
  if (library_selected_ >= static_cast<int32_t>(library_.size()))
    library_selected_ = -1;
  if (!library_table_)
    return;
  auto* children = library_table_->findField<dynamic_ptr>("children"_key);
  if (!children || !*children)
    return;
  size_t key = 0;
  for (size_t i = 0; i < library_.size(); ++i) {
    const auto& [path, bytes] = library_[i];
    const bool now = path == playing_path_;
    auto name = make_label(now ? "> " + path : path);
    if (now)
      common::set_text_color(name, common::kOk);
    auto size = make_label(read_size_label(bytes));
    auto row = ui_element_ptr::create("wish"_key, "TableRow"_key);
    row["selected"_key] = static_cast<int32_t>(i) == library_selected_;
    assign_id(row);
    common::set_children_list(row, {name, size});
    library_row_ids_.insert(library_row_ids_.end(), {wish_id_of(name), wish_id_of(size), wish_id_of(row)});
    (**children)[key++] = dynamic_ptr{row};
  }
  library_table_->refresh_children_order();
  if (library_count_)
    library_count_["text"_key] = library_.empty() ? tr("$$MPV_LIBRARY_EMPTY")
        : library_.size() == 1                    ? tr("$$MPV_ONE_FILE")
                                                  : trf("$$MPV_N_FILES", {std::to_string(library_.size())});
}

void mpv::open_library(int32_t index) {
  if (index < 0 || index >= static_cast<int32_t>(library_.size())) {
    set_status("$$MPV_SELECT_LIBRARY", false);
    return;
  }
  if (!media_available_) {
    set_status("$$MPV_UNAVAILABLE", false);
    return;
  }
  const std::string& path = library_[static_cast<size_t>(index)].first;
  playing_path_ = path;
  dynamic req;
  req["path"_key] = path;
  req["player"_key] = player_name_;
  req["loop"_key] = loop_ ? loop_->get_as<bool>("value"_key, false) : false;
  req["volume"_key] = volume_ ? volume_->get_as<float>("value"_key, 1.0f) : 1.0f;
  emit("on_open_requested"_key, std::move(req));
  if (now_playing_)
    now_playing_["text"_key] = trf("$$MPV_OPENING", {base_name(path)});
  fill_library();
}

void mpv::remove_selected() {
  if (library_selected_ < 0 || library_selected_ >= static_cast<int32_t>(library_.size())) {
    set_status("$$MPV_SELECT_LIBRARY", false);
    return;
  }
  const std::string path = library_[static_cast<size_t>(library_selected_)].first;
  show_confirm(trf("$$MPV_REMOVE_CONFIRM", {path}), [this, path] {
    if (path == playing_path_) {
      control("close");
      playing_path_.clear();
      playing_ = false;
      show_player_controls(false);
      if (now_playing_)
        now_playing_["text"_key] = tr("$$MPV_NOTHING_OPEN");
    }
    std::error_code ec;
    run_in_dispatch([&] {
      const auto& s = sess();
      const auto full = file_service::resolve_path(path, s.resource_dir, s.allow_absolute_paths);
      if (full.empty() || !fs::remove(full, ec))
        ec = ec ? ec : std::make_error_code(std::errc::no_such_file_or_directory);
    });
    set_status(ec ? trf("$$MPV_REMOVE_FAILED", {path, ec.message()}) : trf("$$MPV_REMOVED", {path}), !ec);
    library_selected_ = -1;
    fill_library();
  });
}

dynamic mpv::do_refresh_library(const dynamic&) {
  fill_library();
  return {};
}

// ── Player ───────────────────────────────────────────────────────────────────

void mpv::control(const std::string& action, float value) {
  if (playing_path_.empty() && action != "close") {
    set_status("$$MPV_NOTHING_OPEN", false);
    return;
  }
  emit("on_control"_key, make_payload("action"_key, action, "value"_key, value));
}

void mpv::show_player_controls(bool playing) {
  // One toggle button, not separate Play/Pause buttons swapped with
  // "visible": a hidden first child of a HorizontalLayout breaks the row.
  if (!play_btn_)
    return;
  play_btn_["label"_key] = tr(playing ? "$$MPV_PAUSE" : "$$MPV_PLAY");
  common::set_icon(play_btn_, playing ? "pause" : "play");
}

dynamic mpv::do_update_status(const dynamic& args) {
  const int32_t state = args.get_as<int32_t>("state"_key, 0);
  const float position = args.get_as<float>("position"_key, 0.0f);
  const float duration = args.get_as<float>("duration"_key, 0.0f);
  playing_ = state == 1;
  show_player_controls(playing_);
  if (seek_ && std::chrono::steady_clock::now() >= seek_quiet_until_) {
    seek_["max"_key] = duration > 0.0f ? duration : 1.0f;
    seek_["value"_key] = position;
  }
  if (time_)
    time_["text"_key] = format_time(position) + " / " + format_time(duration);
  if (now_playing_ && !playing_path_.empty()) {
    static const char* kStates[] = {"$$MPV_STOPPED", "$$MPV_PLAYING", "$$MPV_PAUSED", "$$MPV_ENDED", "$$MPV_ERROR"};
    const std::string state_text = tr(kStates[std::clamp(state, 0, 4)]);
    std::string line = base_name(playing_path_) + "  -  " + state_text;
    if (args.get_as<bool>("has_audio"_key, false) && !args.get_as<bool>("audio_output"_key, true))
      line += "  " + tr("$$MPV_NO_SOUND_CARD");
    if (!args.get_as<bool>("has_video"_key, false))
      line += "  " + tr("$$MPV_AUDIO_ONLY");
    now_playing_["text"_key] = line;
  }
  return {};
}

dynamic mpv::do_media_unavailable(const dynamic&) {
  media_available_ = false;
  if (unavailable_)
    unavailable_["visible"_key] = true;
  set_status("$$MPV_UNAVAILABLE", false);
  return {};
}

dynamic mpv::on_set(const dynamic& patch) {
  // The client reports transfer and player results through "status".
  if (auto* v = patch.findField<std::string>("status"_key))
    set_status(*v, !patch.get_as<bool>("status_error"_key, false));
  return patch;
}

void mpv::set_status(const std::string& text, bool ok) {
  common::set_status_text(status_, tr(text), ok);
}

// ── Events ───────────────────────────────────────────────────────────────────

void mpv::on_event(key_t id, key_t event, const dynamic& payload) {
  if (id == window_id_ && event == "closed"_key) {
    emit("closed"_key);
    remove_internal_objects();
    return;
  }
  if (event == "clicked"_key && dispatch_click(id))
    return;

  if (id == local_table_id_ || id == library_table_id_) {
    const int32_t index = payload.get_as<int32_t>("index"_key, -1);
    const bool activated = event == "row_activated"_key;
    if (event != "row_selected"_key && !activated)
      return;
    if (id == local_table_id_) {
      if (activated)
        activate_local(index);
      else
        select_local(index, payload.get_as<bool>("ctrl"_key, false), payload.get_as<bool>("shift"_key, false));
    } else {
      library_selected_ = index;
      if (activated)
        open_library(index);
      else
        fill_library();
    }
    return;
  }

  if (event != "changed"_key)
    return;
  if (id == local_path_id_) {
    emit("on_local_navigate"_key,
        make_payload("name"_key, common::str_of(payload, "value"_key), "type"_key, std::string{"path"}));
  } else if (id == seek_id_) {
    seek_quiet_until_ = std::chrono::steady_clock::now() + kSeekQuiet;
    control("seek", payload.get_as<float>("value"_key, 0.0f));
  } else if (id == volume_id_) {
    control("volume", payload.get_as<float>("value"_key, 1.0f));
  } else if (id == loop_id_) {
    control("loop", payload.get_as<bool>("value"_key, false) ? 1.0f : 0.0f);
  }
}

// ── Registration ─────────────────────────────────────────────────────────────

void register_mpv() {
  auto proto = dynamic_ptr{"Mpv"_key, {}};
  proto->addField(
      "player"_key,
      field{
          std::string{""},
          attr<DisplayName>("Player"),
          attr<Description>("Name of the media player this window shows (set by the form; the client "
                            "opens files under this name, see on_open_requested)."),
          attr<Category>("Data")});
  proto->addField(
      "status"_key,
      field{
          std::string{"$$MPV_READY"},
          attr<DisplayName>("Status"),
          attr<Description>("Latest status message, set by the client to report transfers and player "
                            "errors (translation keys are translated)."),
          attr<Category>("Data")});
  proto->addField(
      "status_error"_key,
      field{
          false,
          attr<DisplayName>("Status Is Error"),
          attr<Description>("Set together with status: shows it in red."),
          attr<Category>("Data")});
  proto->addMethod("update_local_listing"_key, method{[](dynamic& self, const dynamic& a) -> dynamic {
                     return static_cast<mpv&>(self).do_update_local_listing(a);
                   }});
  proto->addMethod("update_status"_key, method{[](dynamic& self, const dynamic& a) -> dynamic {
                     return static_cast<mpv&>(self).do_update_status(a);
                   }});
  proto->addMethod("media_unavailable"_key, method{[](dynamic& self, const dynamic& a) -> dynamic {
                     return static_cast<mpv&>(self).do_media_unavailable(a);
                   }});
  proto->addMethod("refresh_library"_key, method{[](dynamic& self, const dynamic& a) -> dynamic {
                     return static_cast<mpv&>(self).do_refresh_library(a);
                   }});
  proto->addMethod("__setter"_key, method{[](dynamic& s, const dynamic& p) -> dynamic {
                     return static_cast<mpv&>(s).on_set(p);
                   }});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Mpv"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Media player: lists the client's local media files (on_local_navigate / "
                        "update_local_listing), uploads them to the sandbox (on_upload_requested), and "
                        "plays sandbox media through the server's media service (on_open_requested / "
                        "on_control, status via update_status). Emits 'closed' when the window closes."));
  dynamic::addClass("wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<mpv>("wish"_key, "Mpv"_key));
}

} // namespace bdg::wish
