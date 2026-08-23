// MIT License © 2025 Binary Dice Games
/// @file pix.cpp
/// @brief Client-side runner for the Pix embedded app.
///
/// The PixViewer form (server-side) owns and renders the UI, but has no
/// access to the client's local machine. This runner enumerates local
/// directories, decodes/resizes/re-encodes thumbnails and preview crops
/// (via stb_image/stb_image_resize2/stb_image_write), uploads results into
/// the session sandbox, and pushes them back through the form's RMI
/// methods -- all off the RMI dispatch thread (see the "async work"
/// comment below) so a large folder or a big image never blocks the UI.
#include "modules/bdg/desktop/pix/client/pix.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

// STB_IMAGE_STATIC/STB_IMAGE_WRITE_STATIC/STB_IMAGE_RESIZE_STATIC give this
// translation unit's stbi_*/stbir_* functions internal linkage. Without
// them, this TU's own IMPLEMENTATION would collide at final link time with
// wish_server's own separate stb_image/stb_image_write copies
// (src/web/web_renderer.cpp, src/sdl/sdl3_renderer.cpp) whenever a binary
// links both wish_server and this module's client code (e.g. wish-cli,
// wish-standalone) -- "multiple definition of `stbi_load'" and similar.
// wish-client/wish_client_dll never link wish_server at all, so this TU is
// the *only* source of these symbols there; STATIC linkage costs nothing
// and works identically in both cases.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

using namespace bison;

namespace {

namespace fs = std::filesystem;

// Thumbnails are generated at this max square size and uploaded as PNG;
// PixViewer's own grid cells (see server/pix.cpp's kThumbPx) display them
// scaled to fit -- the two constants don't need to match exactly.
constexpr int kThumbMax = 96;
// Preview viewport PixViewer's preview_table clips/scrolls within (must
// match server/pix.cpp's kPixLayout preview_table outer_width/outer_height).
constexpr float kPreviewViewportW = 600.0f;
constexpr float kPreviewViewportH = 440.0f;
constexpr float kMinZoomPercent = 5.0f;
constexpr float kMaxZoomPercent = 800.0f;
// Full-resolution images can be tens of MB each and (unlike thumbnails,
// which stay cheap and per-directory forever) are cached globally across
// every directory browsed this session -- capped at this many most-
// recently-selected entries so the sandbox doesn't grow without bound as
// the user browses many folders. See touch_full_cache().
constexpr size_t kMaxCachedFullImages = 20;

const std::vector<std::string>& supported_extensions() {
  static const std::vector<std::string> kExts{".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif"};
  return kExts;
}

bool is_image_file(const fs::path& path) {
  auto ext = path.extension().string();
  for (auto& c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const auto& exts = supported_extensions();
  return std::find(exts.begin(), exts.end(), ext) != exts.end();
}

std::string format_name(const std::string& ext_lower) {
  if (ext_lower == ".jpg" || ext_lower == ".jpeg")
    return "JPEG";
  if (ext_lower.size() > 1)
    return [&] {
      std::string up = ext_lower.substr(1);
      for (auto& c : up)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      return up;
    }();
  return "Unknown";
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

std::string format_modified(const fs::file_time_type& ftime) {
  auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
  std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &tt);
#else
  localtime_r(&tt, &tm_buf);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M");
  return oss.str();
}

// A short, stable-per-path identifier used to namespace this directory's
// uploads in the sandbox (pix_cache/<dir_key>/...), so browsing two
// different local folders that happen to share a filename never collides.
std::string dir_key_for(const fs::path& dir) {
  std::hash<std::string> hasher;
  std::ostringstream oss;
  oss << std::hex << (hasher(dir.string()) & 0xffffffffu);
  return oss.str();
}

std::string sandbox_thumb_path(const std::string& dir_key, const std::string& name) {
  return "pix_cache/" + dir_key + "/thumbs/" + name + ".png";
}

// The full-resolution image cache is global (shared across every directory
// browsed this session, unlike thumbnails above), so its key is derived
// from the image's own full local path rather than a per-directory key --
// two different directories' "photo.png" must not collide, and revisiting
// the exact same local file (even after browsing elsewhere) must land on
// the same cache slot so it counts as a cache hit, not a fresh entry. The
// extension rides along in the key itself so it doubles as the cache
// entry's actual sandbox filename (see sandbox_full_cache_path()) without
// a separate lookup.
std::string full_cache_key_for(const fs::path& local_path) {
  std::hash<std::string> hasher;
  std::ostringstream oss;
  oss << std::hex << (hasher(local_path.string()) & 0xffffffffu) << local_path.extension().string();
  return oss.str();
}

std::string sandbox_full_cache_path(const std::string& cache_key) {
  return "pix_cache/full_cache/" + cache_key;
}

// Invokes an RMI method on `pix`, retrying a couple of times on failure --
// see tree.cpp's call_with_retry() for the rationale (a method
// call placed right after a burst of upload_file traffic occasionally
// races a concurrent session reader).
dynamic call_with_retry(
    const std::shared_ptr<rmi::proxy::dynamic>& pix, key_t method, dynamic args, int attempts = 3) {
  for (int attempt = 1;; ++attempt) {
    try {
      return pix->call(method, args.clone()).get();
    } catch (const std::exception&) {
      if (attempt >= attempts)
        throw;
      std::this_thread::sleep_for(std::chrono::milliseconds{50 * attempt});
    }
  }
}

// Returns the sandbox path's mtime (Unix seconds), or 0 if it doesn't
// exist / the stat call fails.
int64_t stat_sandbox_mtime(const std::shared_ptr<rmi::proxy::dynamic>& pix, const std::string& sandbox_path) {
  dynamic paths;
  paths[size_t{0}] = sandbox_path;
  dynamic args;
  args["paths"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(paths))};
  try {
    auto results = pix->call("stat_files"_key, std::move(args)).get();
    auto* first = results[size_t{0}].get<dynamic_ptr>();
    if (!first || !*first)
      return 0;
    auto& entry = **first;
    if (!entry.as<bool>("exists"_key))
      return 0;
    return entry.as<int32_t>("mtime"_key);
  } catch (const std::exception&) {
    return 0;
  }
}

int64_t local_mtime_unix(const fs::path& path) {
  std::error_code ec;
  auto ftime = fs::last_write_time(path, ec);
  if (ec)
    return 0;
  auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
  return static_cast<int64_t>(std::chrono::system_clock::to_time_t(sctp));
}

// PNG-encodes `pixels` (RGBA, w*h*4 bytes) into a std::string.
void write_png_cb(void* ctx, void* data, int size) {
  auto* out = static_cast<std::string*>(ctx);
  out->append(static_cast<const char*>(data), static_cast<size_t>(size));
}

std::string encode_png(const unsigned char* rgba, int w, int h) {
  std::string out;
  stbi_write_png_to_func(write_png_cb, &out, w, h, 4, rgba, w * 4);
  return out;
}

// Computes the largest w x h that fits within max_w x max_h while
// preserving aspect ratio (never upscaling beyond the source).
void fit_dims(int src_w, int src_h, int max_w, int max_h, int& out_w, int& out_h) {
  if (src_w <= 0 || src_h <= 0) {
    out_w = out_h = 1;
    return;
  }
  double scale = std::min({1.0, static_cast<double>(max_w) / src_w, static_cast<double>(max_h) / src_h});
  out_w = std::max(1, static_cast<int>(src_w * scale));
  out_h = std::max(1, static_cast<int>(src_h * scale));
}

// Per-directory-listing entry.
struct image_entry {
  std::string name;
  fs::path local_path;
};

// The currently-selected image's identity/geometry -- read from background
// worker threads (generate_preview_and_info(), push_zoom_update()) as well
// as written from the on_image_selected/on_view_control event handlers, so
// it lives behind bison::synchronized<T> (see CLAUDE.md's concurrency
// guidance) rather than as plain fields.
struct selection_state {
  std::string name;
  fs::path local_path;
  std::string full_sandbox_path;
  int natural_w{0};
  int natural_h{0};
  float zoom_percent{100.0f};
};

// All mutable state for the currently-browsed directory / selected image.
// cur_dir/dir_key/images are touched only from RMI event-handler callbacks
// (delivered one at a time, never concurrently with each other); selection
// is shared with background worker threads and synchronized accordingly.
struct pix_state {
  std::shared_ptr<rmi::proxy::dynamic> pix;
  fs::path cur_dir;
  std::string dir_key;
  std::vector<image_entry> images;

  bison::synchronized<selection_state> selection;

  // Bumped on every directory browse / selection change; a background
  // worker captures the value at spawn time and checks it before pushing
  // a result, so a slow thumbnail/preview job for a since-abandoned
  // selection or directory is silently dropped instead of clobbering
  // newer state.
  std::shared_ptr<std::atomic<uint64_t>> generation = std::make_shared<std::atomic<uint64_t>>(0);

  // Global full-image cache LRU (see touch_full_cache()): full_cache_lru
  // holds cache keys, most-recently-used at the front; full_cache_index
  // maps a key to its list position for O(1) touch/evict. Like
  // cur_dir/dir_key/images above, touched only from RMI event-handler
  // callbacks (delivered one at a time, never concurrently), so it needs
  // no synchronization of its own.
  std::list<std::string> full_cache_lru;
  std::unordered_map<std::string, std::list<std::string>::iterator> full_cache_index;
};

// Marks `cache_key` as the most-recently-used entry in `state`'s global
// full-image cache. If `cache_key` is already tracked, just moves it to the
// front (a cache hit -- its sandbox file may still need a staleness check
// against the local file, which the caller handles separately). Otherwise
// inserts it as a new entry, evicting the least-recently-used entry first
// (deleting its sandbox file via the server's own "delete_file" RMI method
// -- see server/pix.cpp's do_delete_file()) if the cache is already at
// kMaxCachedFullImages. Must only be called from the RMI event-handler
// thread (see pix_state::full_cache_lru's doc comment) -- issues a blocking
// RMI call directly, same as browse_to()'s calls below.
void touch_full_cache(const std::shared_ptr<pix_state>& state, const std::string& cache_key) {
  auto it = state->full_cache_index.find(cache_key);
  if (it != state->full_cache_index.end()) {
    state->full_cache_lru.erase(it->second);
    state->full_cache_lru.push_front(cache_key);
    it->second = state->full_cache_lru.begin();
    return;
  }

  if (state->full_cache_lru.size() >= kMaxCachedFullImages) {
    const std::string& oldest_key = state->full_cache_lru.back();
    dynamic args;
    args["path"_key] = sandbox_full_cache_path(oldest_key);
    try {
      call_with_retry(state->pix, "delete_file"_key, std::move(args));
    } catch (const std::exception&) {
      // Best-effort: worst case the evicted entry's file lingers in the
      // sandbox; the LRU bookkeeping below still proceeds so this session
      // doesn't keep retrying the same failing delete on every eviction.
    }
    state->full_cache_index.erase(oldest_key);
    state->full_cache_lru.pop_back();
  }

  state->full_cache_lru.push_front(cache_key);
  state->full_cache_index[cache_key] = state->full_cache_lru.begin();
}

// Lists `dir`'s image files (see is_image_file()), sorted by name.
std::vector<image_entry> list_images(const fs::path& dir) {
  std::vector<image_entry> out;
  std::error_code ec;
  for (auto& entry : fs::directory_iterator{dir, ec}) {
    if (!entry.is_regular_file(ec))
      continue;
    if (!is_image_file(entry.path()))
      continue;
    out.push_back({entry.path().filename().string(), entry.path()});
  }
  std::sort(out.begin(), out.end(), [](const image_entry& a, const image_entry& b) { return a.name < b.name; });
  return out;
}

// Builds the `files` dynamic FileDialog expects from a directory listing
// (dirs and files both shown, so the user can navigate) -- mirrors
// nano.cpp's list_directory().
dynamic list_directory_for_dialog(const fs::path& dir) {
  dynamic files;
  size_t i = 0;
  if (dir.has_parent_path() && dir != dir.root_path()) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = std::string{".."};
    (*e)["type"_key] = std::string{"dir"};
    files[i++] = dynamic_ptr{e};
  }
  std::error_code ec;
  for (auto& entry : fs::directory_iterator{dir, ec}) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = entry.path().filename().string();
    (*e)["type"_key] = entry.is_directory(ec) ? std::string{"dir"} : std::string{"file"};
    files[i++] = dynamic_ptr{e};
  }
  return files;
}

// ── Thumbnail generation ─────────────────────────────────────────────────────

// Decodes `local_path`, resizes to fit kThumbMax x kThumbMax, PNG-encodes,
// and uploads to `thumb_path`. Runs on a background thread (see
// run_pix()'s callers) -- never call this on the RMI dispatch thread.
void generate_and_upload_thumbnail(
    wish_app_host& s, const std::shared_ptr<rmi::proxy::dynamic>& pix, const fs::path& local_path,
    const std::string& name, const std::string& thumb_path) {
  int w = 0, h = 0, channels = 0;
  unsigned char* pixels = stbi_load(local_path.string().c_str(), &w, &h, &channels, 4);
  if (!pixels)
    return;

  int tw, th;
  fit_dims(w, h, kThumbMax, kThumbMax, tw, th);
  unsigned char* thumb =
      stbir_resize_uint8_linear(pixels, w, h, 0, nullptr, tw, th, 0, STBIR_RGBA);
  stbi_image_free(pixels);
  if (!thumb)
    return;

  std::string png = encode_png(thumb, tw, th);
  free(thumb);

  try {
    s.upload_file(thumb_path, png).get();
    dynamic args;
    args["name"_key] = name;
    args["thumb_path"_key] = thumb_path;
    // The generated thumbnail's own pixel size (already aspect-preserving
    // via fit_dims() above) -- PixViewer fits it into its fixed-square grid
    // cell without stretching it back out to a square. See
    // server/pix.cpp's do_set_thumbnail().
    args["width"_key] = static_cast<int32_t>(tw);
    args["height"_key] = static_cast<int32_t>(th);
    call_with_retry(pix, "set_thumbnail"_key, std::move(args));
  } catch (const std::exception&) {
    // Best-effort: the cell just keeps showing the generic placeholder.
  }
}

// ── Preview / info generation ────────────────────────────────────────────────

// Uploads the full-resolution image if the sandbox copy is missing or
// older than the local file, then reports metadata + the initial
// (fit-to-viewport) preview. Runs on a background thread.
void generate_preview_and_info(
    wish_app_host& s, const std::shared_ptr<pix_state>& state, const std::shared_ptr<rmi::proxy::dynamic>& pix,
    std::string name, fs::path local_path, std::string full_path, uint64_t gen) {
  auto abandoned = [&] { return state->generation->load() != gen; };

  int w = 0, h = 0, comp = 0;
  stbi_info(local_path.string().c_str(), &w, &h, &comp);

  auto ext = local_path.extension().string();
  for (auto& c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  std::error_code ec;
  auto file_size = fs::file_size(local_path, ec);
  auto mtime = fs::last_write_time(local_path, ec);

  if (abandoned())
    return;

  dynamic info;
  info["filename"_key] = name;
  if (w > 0 && h > 0) {
    std::ostringstream res;
    res << w << " x " << h;
    info["resolution"_key] = res.str();
  }
  info["format"_key] = format_name(ext);
  if (!ec)
    info["size"_key] = format_bytes(file_size);
  if (!ec)
    info["modified"_key] = format_modified(mtime);
  call_with_retry(pix, "set_info"_key, std::move(info));

  // Timestamp-checked full-image upload: skip if the sandbox copy is
  // already at least as new as the local file.
  int64_t sandbox_mtime = stat_sandbox_mtime(pix, full_path);
  int64_t local_mtime = local_mtime_unix(local_path);
  if (sandbox_mtime == 0 || sandbox_mtime < local_mtime) {
    std::ifstream in(local_path, std::ios::binary);
    std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
    if (abandoned())
      return;
    try {
      s.upload_file(full_path, data).get();
    } catch (const std::exception&) {
      return;
    }
  }

  if (abandoned())
    return;

  float zoom = 100.0f;
  if (w > 0 && h > 0) {
    double fit = std::min(
        1.0, std::min(
                 static_cast<double>(kPreviewViewportW) / w, static_cast<double>(kPreviewViewportH) / h));
    zoom = static_cast<float>(std::min(100.0, fit * 100.0));
  }
  if (abandoned())
    return;
  {
    auto sel = state->selection.wlock();
    if (sel->name != name)
      return; // Superseded by a newer selection while we were computing.
    sel->natural_w = w;
    sel->natural_h = h;
    sel->zoom_percent = zoom;
  }

  dynamic preview;
  preview["loading"_key] = false;
  preview["src"_key] = full_path;
  preview["width"_key] = static_cast<int32_t>(w * zoom / 100.0f);
  preview["height"_key] = static_cast<int32_t>(h * zoom / 100.0f);
  preview["zoom_percent"_key] = zoom;
  call_with_retry(pix, "set_preview"_key, std::move(preview));
}

// Applies the current selection's zoom_percent to its natural size and
// pushes an updated set_preview -- never re-uploads, just changes the
// displayed size (see server/pix.hpp's class comment on why panning past
// that size is the preview Table's own native scrolling).
void push_zoom_update(const std::shared_ptr<pix_state>& state) {
  dynamic preview;
  {
    auto sel = state->selection.rlock();
    if (sel->name.empty() || sel->natural_w <= 0)
      return;
    preview["loading"_key] = false;
    preview["src"_key] = sel->full_sandbox_path;
    preview["width"_key] = static_cast<int32_t>(sel->natural_w * sel->zoom_percent / 100.0f);
    preview["height"_key] = static_cast<int32_t>(sel->natural_h * sel->zoom_percent / 100.0f);
    preview["zoom_percent"_key] = sel->zoom_percent;
  }
  call_with_retry(state->pix, "set_preview"_key, std::move(preview));
}

// ── Directory browsing ───────────────────────────────────────────────────────

// Lists `dir`'s images, rebuilds the grid, and kicks off async thumbnail
// generation for any whose sandbox copy is missing/stale. Runs on the RMI
// dispatch (event-handler) thread; all actual decode/resize/upload work is
// handed off to background threads so this returns quickly.
void browse_to(wish_app_host& s, const std::shared_ptr<pix_state>& state, const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    dynamic status;
    status["message"_key] = std::string{"Not a directory: "} + dir.string();
    call_with_retry(state->pix, "set_status"_key, std::move(status));
    return;
  }

  state->generation->fetch_add(1);
  state->cur_dir = dir;
  state->dir_key = dir_key_for(dir);
  state->images = list_images(dir);
  *state->selection.wlock() = selection_state{};

  dynamic path_patch;
  path_patch["path"_key] = dir.string();
  state->pix->set(std::move(path_patch)).get();

  dynamic images_arg;
  {
    dynamic list;
    size_t i = 0;
    for (auto& img : state->images) {
      auto e = std::make_shared<dynamic>();
      (*e)["name"_key] = img.name;
      list[i++] = dynamic_ptr{e};
    }
    images_arg["images"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(list))};
  }
  call_with_retry(state->pix, "set_images"_key, std::move(images_arg));

  dynamic status;
  std::ostringstream msg;
  msg << state->images.size() << " image(s).";
  status["message"_key] = msg.str();
  call_with_retry(state->pix, "set_status"_key, std::move(status));

  // Batch-stat every thumbnail path in one round trip (see
  // PixViewer::do_stat_files's doc comment) rather than one call per
  // image, then only spawn a worker for entries that are missing/stale.
  dynamic paths;
  {
    size_t i = 0;
    for (auto& img : state->images)
      paths[i++] = sandbox_thumb_path(state->dir_key, img.name);
  }
  dynamic stat_args;
  stat_args["paths"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(paths))};

  dynamic stat_results;
  try {
    stat_results = state->pix->call("stat_files"_key, std::move(stat_args)).get();
  } catch (const std::exception&) {
    // Fall through -- every thumbnail will be (re)generated below.
  }

  for (size_t i = 0; i < state->images.size(); ++i) {
    auto& img = state->images[i];
    std::string thumb_path = sandbox_thumb_path(state->dir_key, img.name);

    bool up_to_date = false;
    if (auto* f = stat_results[i].get<dynamic_ptr>(); f && *f) {
      auto& entry = **f;
      if (entry.as<bool>("exists"_key) && entry.as<int32_t>("mtime"_key) >= local_mtime_unix(img.local_path))
        up_to_date = true;
    }

    if (up_to_date) {
      dynamic args;
      args["name"_key] = img.name;
      args["thumb_path"_key] = thumb_path;
      call_with_retry(state->pix, "set_thumbnail"_key, std::move(args));
      continue;
    }

    auto pix = state->pix;
    auto local_path = img.local_path;
    auto name = img.name;
    std::thread([&s, pix, local_path, name, thumb_path]() {
      generate_and_upload_thumbnail(s, pix, local_path, name, thumb_path);
    }).detach();
  }
}

// Shows a FileDialog for picking a folder: navigating updates the dialog's
// listing, and confirming (Open button or double-clicking a row) treats
// whatever directory is currently navigated to as the chosen folder --
// mirrors nano.cpp's browse_and_open(), adapted for folder rather than
// file selection.
void browse_for_folder(wish_app_host& s, const std::shared_ptr<pix_state>& state) {
  auto cur_dir = std::make_shared<fs::path>(state->cur_dir.empty() ? fs::current_path() : state->cur_dir);
  auto dlg = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "FileDialog"_key).get());

  dynamic init;
  init["title"_key] = std::string{"Select Image Folder"};
  init["confirm_label"_key] = std::string{"Select Folder"};
  init["path"_key] = cur_dir->string();
  init["files"_key] = dynamic_ptr{std::make_shared<dynamic>(list_directory_for_dialog(*cur_dir))};
  dlg->set(std::move(init)).get();

  dlg->onEvent("on_navigate"_key, [dlg, cur_dir](dynamic payload) mutable {
    auto name = payload.as<std::string>("name"_key);
    auto type = payload.as<std::string>("type"_key);
    if (type == "path")
      *cur_dir = fs::path(name);
    else if (type == "dir")
      *cur_dir = (name == "..") ? cur_dir->parent_path() : (*cur_dir / name);
    else
      return; // type == "file": not a valid folder choice, ignore.
    dynamic next;
    next["path"_key] = cur_dir->string();
    next["files"_key] = dynamic_ptr{std::make_shared<dynamic>(list_directory_for_dialog(*cur_dir))};
    dlg->set(std::move(next));
  });

  dlg->onEvent("on_open"_key, [&s, state, cur_dir](dynamic) { browse_to(s, state, *cur_dir); });
  // on_cancel: the dialog already removed itself from context.ui_objects;
  // the capture just keeps dlg alive until one of its events fires.
  dlg->onEvent("on_cancel"_key, [dlg](dynamic) {});
}

} // namespace

void run_pix(wish_app_host& s) {
  auto state = std::make_shared<pix_state>();
  state->pix = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "PixViewer"_key).get());
  auto pix = state->pix;

  pix->onEvent("closed"_key, [&s](dynamic) { s.signal_done(); });

  pix->onEvent("on_browse_clicked"_key, [&s, state](dynamic) { browse_for_folder(s, state); });

  pix->onEvent("on_path_submitted"_key, [&s, state](dynamic payload) {
    browse_to(s, state, fs::path(payload.as<std::string>("path"_key)));
  });

  pix->onEvent("on_image_selected"_key, [&s, state](dynamic payload) {
    auto name = payload.as<std::string>("name"_key);
    auto it = std::find_if(
        state->images.begin(), state->images.end(), [&](const image_entry& e) { return e.name == name; });
    if (it == state->images.end())
      return;

    uint64_t gen = state->generation->fetch_add(1) + 1;
    std::string cache_key = full_cache_key_for(it->local_path);
    std::string full_path = sandbox_full_cache_path(cache_key);
    // Touch/evict before spawning the background upload -- must happen here
    // on the event-handler thread, not inside generate_preview_and_info()
    // (a background thread), since full_cache_lru/full_cache_index are only
    // safe to touch single-threaded (see pix_state's doc comment).
    touch_full_cache(state, cache_key);
    {
      auto sel = state->selection.wlock();
      sel->name = name;
      sel->local_path = it->local_path;
      sel->full_sandbox_path = full_path;
      sel->natural_w = sel->natural_h = 0;
    }

    dynamic loading;
    loading["loading"_key] = true;
    call_with_retry(state->pix, "set_preview"_key, std::move(loading));

    auto local_path = it->local_path;
    std::thread([&s, state, pix = state->pix, name, local_path, full_path, gen]() {
      generate_preview_and_info(s, state, pix, name, local_path, full_path, gen);
    }).detach();
  });

  pix->onEvent("on_view_control"_key, [state](dynamic payload) {
    auto action = payload.as<std::string>("action"_key);
    {
      auto sel = state->selection.wlock();
      if (sel->natural_w <= 0)
        return;
      if (action == "zoom_in")
        sel->zoom_percent = std::min(kMaxZoomPercent, sel->zoom_percent * 1.25f);
      else if (action == "zoom_out")
        sel->zoom_percent = std::max(kMinZoomPercent, sel->zoom_percent / 1.25f);
      else if (action == "zoom_100")
        sel->zoom_percent = 100.0f;
      else if (action == "zoom_fit") {
        double fit = std::min(
            1.0, std::min(
                     static_cast<double>(kPreviewViewportW) / sel->natural_w,
                     static_cast<double>(kPreviewViewportH) / sel->natural_h));
        sel->zoom_percent = static_cast<float>(std::min(100.0, fit * 100.0));
      }
    }
    push_zoom_update(state);
  });

  // on_session() blocks until signal_done() is called.
}

namespace {
struct pix_app_registrar {
  pix_app_registrar() {
    register_app({
        .name = "pix",
        .organization = WISH_MODULE_BDG_DESKTOP_PIX_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DESKTOP_PIX_COLLECTION,
        .description = "Local image folder viewer: thumbnail grid + zoomable/pannable preview",
        .params = {},
        .run = run_pix,
    });
  }
};
const pix_app_registrar pix_app_registrar_instance;
} // namespace

} // namespace bdg::wish
