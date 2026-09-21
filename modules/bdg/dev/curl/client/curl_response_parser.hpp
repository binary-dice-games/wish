// MIT License © 2026 Binary Dice Games
/// @file curl_response_parser.hpp
/// @brief Pure, dependency-free parsing of a `curl -i ... -w ...`
///        invocation's captured stdout into status/headers/body.
///
/// Deliberately has zero bison/RMI/framework dependencies (just
/// <string>/<vector>/<cstdint>) so it can be unit-tested directly --
/// see tests/test_curl_response_parser.cpp -- without pulling in the rest
/// of curl_source's proxy/session machinery. This split exists because a
/// real bug in parse_curl_output() (see its own doc comment, and
/// DESIGN.md "Command construction & response parsing") was found only by
/// driving the real module end-to-end against a live HTTP endpoint via
/// the automation module; the function had no unit test of its own at
/// that point. It does now.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bdg::wish::curl {

/// @brief One Params/Headers/Form-field/Environment-variable/response-
/// header row.
struct kv_entry {
  std::string key;
  std::string value;
  bool enabled{true};
};

/// @brief The result of parsing one finished `curl` invocation's stdout.
struct parsed_response {
  std::string header_block;
  std::string body;
  std::vector<kv_entry> headers;
  int32_t status_code{0};
  std::string status_text;
  float time_ms{0.0f};
  float size_bytes{0.0f};
};

/// @brief Parses the captured stdout of a
/// `curl -sS -i [-L] -X <method> ... -w
/// '\n__WISH_CURL_META__\t%{http_code}\t%{time_total}\t%{size_download}\n'
/// <url>` invocation (see curl_source.cpp's send_request()) into the
/// final (post-redirect) status code/text, timing, size, response
/// headers, and the raw response body.
///
/// Strips the `-w` sentinel trailer from the raw text FIRST, before doing
/// any header/body boundary detection -- see the implementation's comment
/// for why scanning for boundaries first silently drops the body whenever
/// it already ends in its own trailing newline (routine for a JSON API
/// response).
parsed_response parse_curl_output(const std::string& stdout_text);

} // namespace bdg::wish::curl
