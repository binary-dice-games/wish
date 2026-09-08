// MIT License © 2026 Binary Dice Games
/// @file dap_protocol.cpp
/// @brief Implementation of the pure DAP framing helpers. No I/O, no OS
///        calls — see the header.
#include "dap_protocol.hpp"

#include <cctype>
#include <cstdlib>

namespace bdg::wish::dbg {

namespace {

constexpr std::string_view kHeaderSep = "\r\n\r\n";
constexpr std::string_view kContentLength = "content-length:";

/// @brief ASCII-lowercased copy — DAP header names are case-insensitive.
std::string ascii_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

/// @brief Parses the `Content-Length` value out of a raw header block.
/// @return The byte count, or `std::nullopt` if the header is absent /
///         malformed.
std::optional<size_t> content_length_of(std::string_view header_block) {
  size_t line_start = 0;
  while (line_start < header_block.size()) {
    size_t eol = header_block.find("\r\n", line_start);
    std::string_view line = header_block.substr(
        line_start, eol == std::string_view::npos ? std::string_view::npos : eol - line_start);
    std::string lowered = ascii_lower(line);
    if (lowered.rfind(kContentLength, 0) == 0) {
      std::string_view value = line.substr(kContentLength.size());
      size_t b = value.find_first_not_of(" \t");
      if (b == std::string_view::npos)
        return std::nullopt;
      value.remove_prefix(b);
      char* end = nullptr;
      long n = std::strtol(std::string(value).c_str(), &end, 10);
      if (end == std::string(value).c_str() || n < 0)
        return std::nullopt;
      return static_cast<size_t>(n);
    }
    if (eol == std::string_view::npos)
      break;
    line_start = eol + 2;
  }
  return std::nullopt;
}

} // namespace

void dap_message_reader::feed(std::string_view bytes) {
  // Compact the already-consumed prefix before appending so the buffer
  // doesn't grow without bound across a long session.
  if (pos_ > 0) {
    buf_.erase(0, pos_);
    pos_ = 0;
  }
  buf_.append(bytes);
}

std::optional<std::string> dap_message_reader::next_raw() {
  for (;;) {
    std::string_view view(buf_);
    view.remove_prefix(pos_);

    size_t sep = view.find(kHeaderSep);
    if (sep == std::string_view::npos)
      return std::nullopt; // Header not fully arrived.

    std::string_view header_block = view.substr(0, sep);
    size_t body_start = sep + kHeaderSep.size();

    std::optional<size_t> len = content_length_of(header_block);
    if (!len) {
      // Unusable header — skip it and keep scanning from just past it.
      pos_ += body_start;
      continue;
    }

    if (view.size() - body_start < *len)
      return std::nullopt; // Body not fully arrived yet.

    std::string body(view.substr(body_start, *len));
    pos_ += body_start + *len;
    return body;
  }
}

std::optional<dap_json> dap_message_reader::next() {
  for (;;) {
    std::optional<std::string> raw = next_raw();
    if (!raw)
      return std::nullopt;
    dap_json parsed = dap_json::parse(*raw, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_discarded())
      return parsed;
    // Malformed JSON body: drop it, try the next framed message.
  }
}

std::string dap_frame(const dap_json& message) {
  std::string body = message.dump();
  return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

dap_json dap_make_request(int seq, std::string_view command, dap_json arguments) {
  dap_json req = {
      {"seq", seq},
      {"type", "request"},
      {"command", std::string(command)},
  };
  if (!arguments.is_null())
    req["arguments"] = std::move(arguments);
  return req;
}

} // namespace bdg::wish::dbg
