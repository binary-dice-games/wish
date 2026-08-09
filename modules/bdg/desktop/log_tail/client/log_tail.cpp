// MIT License © 2025 Binary Dice Games
/// @file log_tail.cpp
/// @brief Client-side runner for the Log Tail embedded app.
///
/// The LogTail form (server-side) never touches the filesystem -- it only
/// parses/renders whatever raw lines it is given. This runner owns actual
/// `tail`-style file reading (and, with `-f`, following) of files on the
/// *client's* local machine, one background thread per file, mirroring how
/// ProcessExplorer's reference client owns sampling while the server only
/// renders (see modules/bdg/desktop/process_explorer/client/process_explorer.cpp).
///
/// Command-line usage (after `--`, see `wish_app_host::app_args()`), modeled
/// on the Linux `tail` tool:
///
///   wish client --run=log_tail -- [-f] [-n N|+N] [-e REGEX] FILE [FILE...]
///
///   -f, --follow        Keep watching each file for appended lines.
///   -n, --lines N       Show the last N lines initially (default 10).
///   -n +N               Show lines starting from line N (from the start).
///   -q, -v              Accepted for command-line compatibility with real
///                       `tail`; every row already carries its own Source
///                       column, so there is no separate per-file header to
///                       suppress/force.
///   -e, --filter REGEX  Set the form's initial filter regex.
#include "modules/bdg/desktop/log_tail/client/log_tail.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bdg::wish {

using namespace bison;

namespace {

namespace fs = std::filesystem;

struct log_tail_options {
  bool follow = false;
  int32_t line_count = 10;
  bool lines_from_start = false; // true for "-n +N"; false for "-n N" (last N lines)
  std::string filter;
  std::vector<std::string> files;
};

void parse_n_value(const std::string& v, log_tail_options& opt) {
  std::string digits = v;
  if (!digits.empty() && digits[0] == '+') {
    opt.lines_from_start = true;
    digits = digits.substr(1);
  } else if (!digits.empty() && digits[0] == '-') {
    digits = digits.substr(1);
  }
  try {
    opt.line_count = std::stoi(digits);
  } catch (const std::exception&) {
    // Malformed -n value: ignore, keep the previous count.
  }
}

log_tail_options parse_args(const std::vector<std::string>& args) {
  log_tail_options opt;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "-f" || a == "--follow") {
      opt.follow = true;
    } else if (a == "-q" || a == "--quiet" || a == "--silent" || a == "-v" || a == "--verbose") {
      // See file doc comment: accepted, no visual effect.
    } else if (a == "-e" || a == "--filter") {
      if (i + 1 < args.size())
        opt.filter = args[++i];
    } else if (a.rfind("--filter=", 0) == 0) {
      opt.filter = a.substr(9);
    } else if (a == "-n" || a == "--lines") {
      if (i + 1 < args.size())
        parse_n_value(args[++i], opt);
    } else if (a.rfind("--lines=", 0) == 0) {
      parse_n_value(a.substr(8), opt);
    } else if (a.size() > 2 && a[0] == '-' && a[1] == 'n') {
      parse_n_value(a.substr(2), opt);
    } else {
      opt.files.push_back(a);
    }
  }
  return opt;
}

// Scans backward from EOF in fixed-size chunks to find the byte offset
// where the last `n` lines begin, without reading a potentially huge file
// fully into memory just to keep its tail. Returns 0 (start of file) if the
// file has `n` or fewer lines.
std::uintmax_t offset_of_last_n_lines(std::ifstream& in, int32_t n) {
  in.clear();
  in.seekg(0, std::ios::end);
  auto size = static_cast<std::uintmax_t>(in.tellg());
  if (n <= 0)
    return size;

  constexpr std::uintmax_t kChunk = 4096;
  std::uintmax_t pos = size;
  int32_t newline_count = 0;

  while (pos > 0) {
    std::uintmax_t read_size = std::min<std::uintmax_t>(kChunk, pos);
    pos -= read_size;
    in.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
    std::string chunk(read_size, '\0');
    in.read(chunk.data(), static_cast<std::streamsize>(read_size));

    for (auto it = chunk.rbegin(); it != chunk.rend(); ++it) {
      if (*it != '\n')
        continue;
      ++newline_count;
      if (newline_count > n) {
        auto forward_index = static_cast<std::uintmax_t>(std::distance(chunk.begin(), it.base()));
        return pos + forward_index;
      }
    }
  }
  return 0;
}

// Reads every complete (or trailing partial) line from `in` starting at
// `start_offset` through EOF; `out_offset` is left at the file size at read
// time -- the resume point for follow-mode polling.
std::vector<std::string> read_lines_from(std::ifstream& in, std::uintmax_t start_offset, std::uintmax_t& out_offset) {
  in.clear();
  in.seekg(static_cast<std::streamoff>(start_offset), std::ios::beg);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(std::move(line));
  }
  in.clear();
  in.seekg(0, std::ios::end);
  out_offset = static_cast<std::uintmax_t>(in.tellg());
  return lines;
}

dynamic encode_lines(const std::vector<std::string>& lines, const std::string& source) {
  dynamic entries;
  size_t i = 0;
  for (auto& line : lines) {
    auto e = std::make_shared<dynamic>();
    (*e)["text"_key] = line;
    (*e)["source"_key] = source;
    entries[i++] = dynamic_ptr{e};
  }
  dynamic args;
  args["lines"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(entries))};
  return args;
}

void push_lines(const std::shared_ptr<rmi::proxy::dynamic>& proxy, const std::vector<std::string>& lines,
    const std::string& source) {
  if (lines.empty())
    return;
  try {
    proxy->call("push_lines"_key, encode_lines(lines, source)).get();
  } catch (const std::exception&) {
    // Form already torn down (window closed / session ending) -- the
    // caller's polling loop notices via `stop` on its next iteration.
  }
}

// One file's tail: an initial batch (the last `line_count` lines, or
// starting from line `line_count` if `lines_from_start`), then -- if
// `follow` -- polls for appended content until `stop` is set. A file that
// shrinks (truncation, or rotation that recreates it) is reopened and
// re-tailed from the start.
void tail_file(std::shared_ptr<rmi::proxy::dynamic> proxy, std::shared_ptr<std::atomic<bool>> stop, std::string path,
    log_tail_options opt) {
  std::string source = fs::path(path).filename().string();

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::cerr << "[log_tail] cannot open: " << path << '\n';
    return;
  }

  std::uintmax_t offset = 0;
  if (opt.lines_from_start) {
    std::uintmax_t end_offset = 0;
    auto all_lines = read_lines_from(in, 0, end_offset);
    size_t skip = opt.line_count > 1 ? static_cast<size_t>(opt.line_count - 1) : 0;
    std::vector<std::string> kept;
    for (size_t i = skip; i < all_lines.size(); ++i)
      kept.push_back(std::move(all_lines[i]));
    push_lines(proxy, kept, source);
    offset = end_offset;
  } else {
    auto start_offset = offset_of_last_n_lines(in, opt.line_count);
    auto lines = read_lines_from(in, start_offset, offset);
    push_lines(proxy, lines, source);
  }

  if (!opt.follow)
    return;

  using namespace std::chrono_literals;
  while (!stop->load(std::memory_order_relaxed)) {
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    if (ec) {
      // Missing (e.g. rotated away) -- keep polling; it may reappear.
      std::this_thread::sleep_for(300ms);
      continue;
    }
    if (size < offset) {
      // Truncated or replaced with a new file at the same path.
      in.close();
      in.open(path, std::ios::binary);
      offset = 0;
    }
    if (size > offset) {
      std::uintmax_t new_offset = offset;
      auto lines = read_lines_from(in, offset, new_offset);
      offset = new_offset;
      push_lines(proxy, lines, source);
    }
    std::this_thread::sleep_for(300ms);
  }
}

} // namespace

void run_log_tail(wish_app_host& s) {
  auto proxy = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "LogTail"_key).get());
  auto stop = std::make_shared<std::atomic<bool>>(false);

  proxy->onEvent("closed"_key, [&s, stop](dynamic) {
    stop->store(true, std::memory_order_relaxed);
    s.signal_done();
  });

  auto opt = parse_args(s.app_args());

  if (opt.files.empty()) {
    std::cerr << "[log_tail] usage: wish client --run=log_tail -- [-f] [-n N|+N] [-e REGEX] FILE...\n";
  } else {
    std::string title = "Log Tail - ";
    for (size_t i = 0; i < opt.files.size(); ++i) {
      if (i)
        title += ", ";
      title += fs::path(opt.files[i]).filename().string();
    }
    dynamic title_args;
    title_args["title"_key] = title;
    proxy->set(std::move(title_args));
  }

  if (!opt.filter.empty()) {
    dynamic filter_args;
    filter_args["pattern"_key] = opt.filter;
    try {
      proxy->call("set_filter"_key, std::move(filter_args)).get();
    } catch (const std::exception&) {
    }
  }

  // `proxy`/`stop` stay alive via the shared_ptrs captured by the "closed"
  // handler above and by each tail_file thread below -- no separate
  // keep_alive() call needed, mirroring process_explorer's own
  // background-sampling thread.
  for (auto& file : opt.files)
    std::thread(tail_file, proxy, stop, file, opt).detach();
}

namespace {
struct log_tail_app_registrar {
  log_tail_app_registrar() {
    register_app({
        .name = "log_tail",
        .organization = WISH_MODULE_BDG_DESKTOP_LOG_TAIL_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DESKTOP_LOG_TAIL_COLLECTION,
        .description = "`tail`-like log viewer: colorized by severity, filterable by regex, "
                        "with a dedicated tab per [Tag] token seen in the stream",
        .params = {{"files", "One or more log file paths to tail (see --describe=log_tail for full usage)"}},
        .run = run_log_tail,
    });
  }
};
const log_tail_app_registrar log_tail_app_registrar_instance;
} // namespace

} // namespace bdg::wish
