// MIT License © 2026 Binary Dice Games
/// @file curl_response_parser.cpp
/// @brief Implementation of parse_curl_output().
#include "curl_response_parser.hpp"

#include <sstream>

namespace bdg::wish::curl {

// See DESIGN.md "Command construction & response parsing": `-i` prepends
// the (final, post-redirect) status line + headers to stdout ahead of the
// body; a `-w` format string appends one trailer line with status/timing/
// size after the body. No temp files, one captured stream.

parsed_response parse_curl_output(const std::string& stdout_text) {
  parsed_response pr;

  // Strip the -w sentinel trailer FIRST, from the raw, unsplit text -- not
  // from whatever the header/body boundary scan below leaves over. The
  // trailer's own leading "\n" (in the -w format string) plus a body that
  // already ends in its own trailing newline (routine for a JSON API
  // response) together produce a *second* blank-line boundary right
  // before "__WISH_CURL_META__"; scanning for boundaries before removing
  // the trailer misreads that as the real header/body split and leaves
  // the body empty. Stripping the trailer up front means the boundary
  // scan below only ever sees genuine header/body blank lines.
  static const std::string kSentinel = "__WISH_CURL_META__";
  size_t meta_pos = stdout_text.rfind(kSentinel);
  std::string head_and_body = meta_pos != std::string::npos ? stdout_text.substr(0, meta_pos) : stdout_text;
  // Trim exactly the one "\n" (or "\r\n") the -w format's own leading
  // newline put immediately before the sentinel -- not a real body
  // character, and not to be confused with a trailing newline the body
  // itself may separately end with.
  if (!head_and_body.empty() && head_and_body.back() == '\n')
    head_and_body.pop_back();
  if (!head_and_body.empty() && head_and_body.back() == '\r')
    head_and_body.pop_back();

  if (meta_pos != std::string::npos) {
    std::string meta = stdout_text.substr(meta_pos + kSentinel.size());
    std::vector<std::string> fields;
    std::string cur;
    for (char c : meta) {
      if (c == '\t') {
        fields.push_back(cur);
        cur.clear();
      } else if (c != '\n' && c != '\r') {
        cur += c;
      }
    }
    fields.push_back(cur);
    auto get = [&](size_t idx) -> std::string { return idx < fields.size() ? fields[idx] : std::string{}; };
    try {
      pr.status_code = std::stoi(get(1));
    } catch (const std::exception&) {
      pr.status_code = 0;
    }
    try {
      pr.time_ms = std::stof(get(2)) * 1000.0f;
    } catch (const std::exception&) {
      pr.time_ms = 0.0f;
    }
    try {
      pr.size_bytes = std::stof(get(3));
    } catch (const std::exception&) {
      pr.size_bytes = 0.0f;
    }
  }

  // Collect every blank-line boundary within the (now sentinel-free)
  // headers+body text -- `-L` prints one header block per redirect hop,
  // all concatenated ahead of the final body -- so we can pick out just
  // the *last* hop's status line + headers.
  std::vector<size_t> starts;
  std::vector<size_t> ends;
  size_t search_from = 0;
  for (;;) {
    size_t p_crlf = head_and_body.find("\r\n\r\n", search_from);
    size_t p_lf = head_and_body.find("\n\n", search_from);
    size_t p = std::string::npos;
    size_t len = 0;
    if (p_crlf != std::string::npos && (p_lf == std::string::npos || p_crlf <= p_lf)) {
      p = p_crlf;
      len = 4;
    } else if (p_lf != std::string::npos) {
      p = p_lf;
      len = 2;
    }
    if (p == std::string::npos)
      break;
    starts.push_back(p);
    ends.push_back(p + len);
    search_from = p + len;
  }

  if (!ends.empty()) {
    size_t header_start = ends.size() >= 2 ? ends[ends.size() - 2] : 0;
    pr.header_block = head_and_body.substr(header_start, starts.back() - header_start);
    pr.body = head_and_body.substr(ends.back());
  } else {
    // No blank-line boundary found at all (shouldn't normally happen for
    // a real HTTP response) -- treat the whole thing as body, no headers.
    pr.body = head_and_body;
  }

  {
    std::istringstream iss(pr.header_block);
    std::string line;
    bool first = true;
    while (std::getline(iss, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.empty())
        continue;
      if (first) {
        first = false;
        size_t sp1 = line.find(' ');
        if (sp1 != std::string::npos) {
          size_t sp2 = line.find(' ', sp1 + 1);
          pr.status_text = sp2 != std::string::npos ? line.substr(sp2 + 1) : std::string{};
        }
        continue;
      }
      size_t colon = line.find(':');
      if (colon == std::string::npos)
        continue;
      std::string key = line.substr(0, colon);
      size_t vstart = colon + 1;
      while (vstart < line.size() && line[vstart] == ' ')
        ++vstart;
      pr.headers.push_back({key, line.substr(vstart), true});
    }
  }
  return pr;
}

} // namespace bdg::wish::curl
