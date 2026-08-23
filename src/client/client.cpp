// MIT License © 2025 Binary Dice Games
/// @file client.cpp
/// @brief Implementation of wish::client.
#include <client/client.hpp>

#include <ui/ui_descriptor.hpp>

#include "src/bison/bison.hpp"

#include <cstdint>
#include <future>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace bdg::wish {

using namespace bison;

// ── client ────────────────────────────────────────────────────────────────────

void client::run(bison::dynamic connect_params) {
  connect(std::move(connect_params));
  try {
    on_session();
  } catch (...) {
    disconnect();
    throw;
  }
  disconnect();
}

void client::on_connect() {
  template_proxy_ = instantiate("wish"_key, "__WishTemplate"_key).get();
  fs_proxy_ = instantiate("wish"_key, "__WishFileSystem"_key).get();
  style_proxy_ = instantiate("wish"_key, "__WishStyle"_key).get();
  log_proxy_ = instantiate("wish"_key, "__WishLogger"_key).get();
  // Non-fatal: unlike the protocol handlers above (always present), a
  // server's active renderer may not implement automation at all (e.g. the
  // web renderer, which has its own separate browser-based mechanism) --
  // find_singleton_service() throws for "__WishAutomation" in that case
  // (see src/context/context.cpp), which must not break the rest of the
  // connection. automation_proxy_ simply stays unset.
  try {
    automation_proxy_ = instantiate("wish"_key, "__WishAutomation"_key).get();
  } catch (...) {
    automation_proxy_.reset();
  }
}

void client::on_disconnect() {
  template_proxy_.reset();
  fs_proxy_.reset();
  style_proxy_.reset();
  log_proxy_.reset();
  automation_proxy_.reset();
  if (on_disconnected_)
    on_disconnected_();
}

// ── Helper: build proxy_map from an indexed apply_descriptor result ───────────

static proxy_map proxies_from_result(bison::rmi::client& c, bison::dynamic& result) {
  proxy_map pm;
  result.forEach([&pm, &c](key_t /*k*/, const field& v) {
    if (!v.is<dynamic_ptr>())
      return;
    const auto& ptr = v.as<dynamic_ptr>();
    if (!ptr)
      return;
    const auto* nf = ptr->findField("name"_key);
    const auto* idf = ptr->findField("id"_key);
    if (!nf || !idf || !nf->is<std::string>() || !idf->is<key_t>())
      return;
    pm.emplace(nf->as<std::string>(), c.make_proxy(idf->as<key_t>()));
  });
  return pm;
}

// ── Wish helpers ──────────────────────────────────────────────────────────────

std::future<void> client::register_template(bison::key_t name, bison::dynamic descriptor) {
  return std::async(std::launch::async, [this, name, d = std::move(descriptor)]() mutable {
    dynamic args;
    args["name"_key] = name;
    args["descriptor"_key] = dynamic_ptr{std::move(d)};
    template_proxy_->call("register"_key, std::move(args)).get();
  });
}

std::future<void> client::register_template_from_json(bison::key_t name, const std::string& json) {
  return register_template(name, import_descriptor_json(json));
}

std::future<void> client::register_template_from_yaml(bison::key_t name, const std::string& yaml) {
  return register_template(name, import_descriptor_yaml(yaml));
}

std::future<proxy_map> client::instantiate_template(bison::key_t name) {
  return std::async(std::launch::async, [this, name]() -> proxy_map {
    dynamic args;
    args["name"_key] = name;
    auto result = template_proxy_->call("instantiate"_key, std::move(args)).get();
    return proxies_from_result(*this, result);
  });
}

void client::upload_stream_sync(const std::string& name, std::istream& data, std::size_t chunk_size) {
  std::vector<char> buf(chunk_size);
  bool first = true;
  for (;;) {
    data.read(buf.data(), static_cast<std::streamsize>(chunk_size));
    auto n = static_cast<std::size_t>(data.gcount());
    // A read that fills the whole buffer doesn't set eofbit until the
    // stream actually tries to read past the end -- peek() forces that
    // check so a chunk landing exactly on the stream's end is still
    // reported correctly (and so an empty stream still sends one chunk).
    bool eof = data.eof() || data.peek() == std::char_traits<char>::eof();

    dynamic args;
    args["name"_key] = name;
    args["data"_key] = std::string(buf.data(), n);
    args["first"_key] = first;
    args["eof"_key] = eof;
    fs_proxy_->call("upload_chunk"_key, std::move(args)).get();

    first = false;
    if (eof)
      break;
  }
}

void client::download_stream_sync(const std::string& name, std::ostream& out, std::size_t chunk_size) {
  int32_t offset = 0;
  for (;;) {
    dynamic args;
    args["name"_key] = name;
    args["offset"_key] = offset;
    args["max_size"_key] = static_cast<int32_t>(chunk_size);
    auto result = fs_proxy_->call("download_chunk"_key, std::move(args)).get();

    auto chunk = result.as<std::string>("data"_key);
    bool eof = result.as<bool>("eof"_key);
    if (!chunk.empty()) {
      out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
      offset += static_cast<int32_t>(chunk.size());
    }
    if (eof)
      break;
  }
}

std::future<void> client::upload_file(const std::string& name, std::istream& data, std::size_t chunk_size) {
  return std::async(
      std::launch::async, [this, name, &data, chunk_size]() { upload_stream_sync(name, data, chunk_size); });
}

std::future<void> client::download_file(const std::string& name, std::ostream& out, std::size_t chunk_size) {
  return std::async(
      std::launch::async, [this, name, &out, chunk_size]() { download_stream_sync(name, out, chunk_size); });
}

void client::upload_stream_sync(
    const std::string& name,
    std::istream& data,
    std::size_t chunk_size,
    std::uint64_t total,
    const transfer_progress_callback& on_progress) {
  std::vector<char> buf(chunk_size);
  bool first = true;
  std::uint64_t sent = 0;
  for (;;) {
    data.read(buf.data(), static_cast<std::streamsize>(chunk_size));
    auto n = static_cast<std::size_t>(data.gcount());
    bool eof = data.eof() || data.peek() == std::char_traits<char>::eof();

    dynamic args;
    args["name"_key] = name;
    args["data"_key] = std::string(buf.data(), n);
    args["first"_key] = first;
    args["eof"_key] = eof;
    fs_proxy_->call("upload_chunk"_key, std::move(args)).get();

    sent += n;
    if (on_progress)
      on_progress(sent, total);

    first = false;
    if (eof)
      break;
  }
}

void client::download_stream_sync(
    const std::string& name, std::ostream& out, std::size_t chunk_size, const transfer_progress_callback& on_progress) {
  int32_t offset = 0;
  for (;;) {
    dynamic args;
    args["name"_key] = name;
    args["offset"_key] = offset;
    args["max_size"_key] = static_cast<int32_t>(chunk_size);
    auto result = fs_proxy_->call("download_chunk"_key, std::move(args)).get();

    auto chunk = result.as<std::string>("data"_key);
    bool eof = result.as<bool>("eof"_key);
    auto total = static_cast<std::uint64_t>(result.as<int32_t>("total"_key));
    if (!chunk.empty()) {
      out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
      offset += static_cast<int32_t>(chunk.size());
    }
    if (on_progress)
      on_progress(static_cast<std::uint64_t>(offset), total);
    if (eof)
      break;
  }
}

std::future<void>
client::upload_file(const std::string& name, const std::string& data, transfer_progress_callback on_progress) {
  if (!on_progress) {
    return std::async(std::launch::async, [this, name, data]() {
      dynamic args;
      args["name"_key] = name;
      args["data"_key] = data;
      fs_proxy_->call("upload"_key, std::move(args)).get();
    });
  }
  return std::async(std::launch::async, [this, name, data, on_progress]() {
    std::istringstream in(data);
    upload_stream_sync(name, in, kDefaultFileChunkSize, data.size(), on_progress);
  });
}

std::future<std::string> client::download_file(const std::string& name, transfer_progress_callback on_progress) {
  if (!on_progress) {
    return std::async(std::launch::async, [this, name]() -> std::string {
      dynamic args;
      args["name"_key] = name;
      auto result = fs_proxy_->call("download"_key, std::move(args)).get();
      return result.as<std::string>("result"_key);
    });
  }
  return std::async(std::launch::async, [this, name, on_progress]() -> std::string {
    std::ostringstream out;
    download_stream_sync(name, out, kDefaultFileChunkSize, on_progress);
    return out.str();
  });
}

std::future<void>
client::upload_package(const std::string& dest_path, std::istream& package_stream, std::size_t chunk_size) {
  return std::async(std::launch::async, [this, dest_path, &package_stream, chunk_size]() {
    std::string staging_name = dest_path + ".wishpkg.zip";
    upload_stream_sync(staging_name, package_stream, chunk_size);

    dynamic args;
    args["zip_name"_key] = staging_name;
    args["dest"_key] = dest_path;
    fs_proxy_->call("unpack"_key, std::move(args)).get();
  });
}

std::future<std::vector<std::string>> client::list_files(const std::string& path) {
  return std::async(std::launch::async, [this, path]() -> std::vector<std::string> {
    dynamic args;
    if (!path.empty())
      args["path"_key] = path;
    auto result = fs_proxy_->call("list"_key, std::move(args)).get();
    std::vector<std::string> names;
    result.forEach([&names](key_t, const field& f) {
      if (f.is<std::string>())
        names.push_back(f.as<std::string>());
    });
    return names;
  });
}

std::future<void> client::set_style_preset(const std::string& name) {
  return std::async(std::launch::async, [this, name]() {
    dynamic args;
    args["name"_key] = name;
    // oneway=true: server applies the preset but sends no response.
    // This lets the call be made safely from within event callbacks
    // (which run on the RMI worker thread that would otherwise deadlock
    // waiting for a response that only it can deliver).
    style_proxy_->call("preset"_key, std::move(args), true).get();
  });
}

std::future<void> client::set_style(bison::dynamic params) {
  return std::async(std::launch::async, [this, p = std::move(params)]() mutable {
    // oneway=true: same reasoning as set_style_preset above.
    style_proxy_->call("set"_key, std::move(p), true).get();
  });
}

std::future<bison::dynamic> client::get_style() {
  return std::async(std::launch::async, [this]() -> dynamic { return style_proxy_->call("get"_key, dynamic{}).get(); });
}

std::future<void> client::log(const std::string& level, const std::string& msg) {
  return std::async(std::launch::async, [this, level, msg]() {
    dynamic args;
    args["level"_key] = level;
    args["msg"_key] = msg;
    // oneway=true: log messages are fire-and-forget to avoid blocking callers.
    log_proxy_->call("log"_key, std::move(args), true).get();
  });
}

std::future<void> client::log_debug(const std::string& msg) {
  return log("debug", msg);
}
std::future<void> client::log_info(const std::string& msg) {
  return log("info", msg);
}
std::future<void> client::log_warn(const std::string& msg) {
  return log("warn", msg);
}
std::future<void> client::log_error(const std::string& msg) {
  return log("error", msg);
}

// ── Automation helpers ──────────────────────────────────────────────────────

namespace {
// Shared guard for every automation helper below -- thrown inside the
// std::async lambda (not before it), so it surfaces through the returned
// future's .get() like any other RMI failure, rather than throwing
// synchronously from the helper call itself.
void require_automation(const std::optional<bison::rmi::proxy::dynamic>& proxy) {
  if (!proxy)
    throw std::logic_error(
        "wish::client: this server's active renderer does not support automation "
        "(see wish::client::automation_supported())");
}
} // namespace

std::future<std::string> client::get_automation_tree(const std::string& root) {
  return std::async(std::launch::async, [this, root]() -> std::string {
    require_automation(automation_proxy_);
    dynamic args;
    if (!root.empty())
      args["root"_key] = root;
    auto result = automation_proxy_->call("get_tree"_key, std::move(args)).get();
    return result.as<std::string>("json"_key);
  });
}

std::future<std::string> client::get_automation_logs() {
  return std::async(std::launch::async, [this]() -> std::string {
    require_automation(automation_proxy_);
    auto result = automation_proxy_->call("get_logs"_key, dynamic{}).get();
    return result.as<std::string>("json"_key);
  });
}

std::future<std::vector<uint8_t>> client::take_screenshot() {
  return std::async(std::launch::async, [this]() -> std::vector<uint8_t> {
    require_automation(automation_proxy_);
    auto result = automation_proxy_->call("screenshot"_key, dynamic{}).get();
    auto data = result.as<std::string>("data"_key);
    return std::vector<uint8_t>(data.begin(), data.end());
  });
}

std::future<void> client::inject_mouse_move(float x, float y) {
  return std::async(std::launch::async, [this, x, y]() {
    require_automation(automation_proxy_);
    dynamic args;
    args["x"_key] = x;
    args["y"_key] = y;
    // oneway=true: same reasoning as set_style_preset -- lets the call be
    // made safely from within event callbacks on the render thread, which
    // would otherwise deadlock waiting for its own response.
    automation_proxy_->call("mouse_move"_key, std::move(args), true).get();
  });
}

std::future<void> client::inject_mouse_button(int button, bool down) {
  return std::async(std::launch::async, [this, button, down]() {
    require_automation(automation_proxy_);
    dynamic args;
    args["button"_key] = static_cast<int32_t>(button);
    args["down"_key] = down;
    automation_proxy_->call("mouse_button"_key, std::move(args), true).get();
  });
}

std::future<void> client::inject_key(int keycode, bool down) {
  return std::async(std::launch::async, [this, keycode, down]() {
    require_automation(automation_proxy_);
    dynamic args;
    args["keycode"_key] = static_cast<int32_t>(keycode);
    args["down"_key] = down;
    automation_proxy_->call("key_event"_key, std::move(args), true).get();
  });
}

std::future<void> client::inject_text(const std::string& utf8) {
  return std::async(std::launch::async, [this, utf8]() {
    require_automation(automation_proxy_);
    dynamic args;
    args["utf8"_key] = utf8;
    automation_proxy_->call("text_input"_key, std::move(args), true).get();
  });
}

} // namespace bdg::wish
