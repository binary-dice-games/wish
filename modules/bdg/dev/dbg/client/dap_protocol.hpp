// MIT License © 2026 Binary Dice Games
/// @file dap_protocol.hpp
/// @brief Wire framing + message helpers for the Debug Adapter Protocol
///        (DAP), the JSON request/response/event protocol spoken by
///        Microsoft's `debugpy` (and every VS Code debug adapter).
///
/// `python_debug_backend` (the Python `debug_backend` implementation) drives
/// a `debugpy` debug server / adapter over a socket and reads its DAP
/// stream: each message is an HTTP-style `Content-Length: N\r\n\r\n` header
/// followed by exactly `N` bytes of a UTF-8 JSON object
/// (`type` ∈ {`request`, `response`, `event`}), the same envelope
/// `mi_parser.hpp` handles for GDB/MI.
///
/// This file is the pure, platform-agnostic, I/O-free half of that backend:
/// `dap_message_reader` turns a raw byte stream into whole JSON messages and
/// `dap_frame()` serialises one message back onto the wire, so both compile
/// and are unit-tested (`tests/test_dap_protocol.cpp`) on every platform.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace bdg::wish::dbg {

using dap_json = nlohmann::json;

/// @brief Incremental reassembler for the DAP `Content-Length` framing.
///
/// Feed it whatever bytes arrive from the socket (in any chunking); pull
/// complete messages out one at a time with `next()` / `next_raw()`. Holds
/// an internal buffer across `feed()` calls, so a header or body split
/// across two reads is handled transparently.
class dap_message_reader {
 public:
  /// @brief Appends freshly-read socket bytes to the internal buffer.
  void feed(std::string_view bytes);

  /// @brief Extracts the next complete message body as raw JSON text, or
  ///        `std::nullopt` if no full message is buffered yet.
  ///
  /// A malformed header (missing `Content-Length`, non-numeric value) is
  /// skipped past rather than returned, matching `parse_mi_line`'s
  /// tolerance of stray non-record lines.
  std::optional<std::string> next_raw();

  /// @brief `next_raw()` parsed into a `dap_json`. A body that isn't valid
  ///        JSON is discarded (returns the following message instead, or
  ///        `std::nullopt`).
  std::optional<dap_json> next();

  /// @brief Bytes buffered but not yet consumed — for tests / diagnostics.
  size_t buffered() const {
    return buf_.size() - pos_;
  }

 private:
  std::string buf_;
  size_t pos_{0}; ///< Consumed-prefix offset; `buf_` is compacted lazily.
};

/// @brief Serialises @p message to the DAP wire framing
///        (`Content-Length: N\r\n\r\n<json>`).
std::string dap_frame(const dap_json& message);

/// @brief Builds a DAP request object:
///        `{ "seq": <seq>, "type": "request", "command": <command>,
///           "arguments": <arguments> }` (the `arguments` key is omitted
///        when @p arguments is null).
dap_json dap_make_request(int seq, std::string_view command, dap_json arguments = dap_json(nullptr));

} // namespace bdg::wish::dbg
