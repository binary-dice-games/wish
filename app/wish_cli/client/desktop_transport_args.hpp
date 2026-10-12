// MIT License © 2026 Binary Dice Games
/**
 * @file desktop_transport_args.hpp
 * @brief `wish client --transport=desktop`: rewrites a command line so it
 *        connects to the local desktop's fixed endpoint over a pipe.
 */
#pragma once

#include <array>
#include <string>
#include <vector>

namespace bdg::wish {

/**
 * @brief Inserts `--transport=pipe` and `--name=<endpoint>` into @p argv,
 *        just before the first literal `--` (or at the end if there is
 *        none), so they are still parsed as flags.
 *
 * bison's `client_app::run()` re-parses the original command line and only
 * knows tcp/pipe/tls/term. gflags keeps the last value given for a flag, so
 * the inserted arguments win over any earlier `--transport=desktop`,
 * `--transport desktop` or `--name`, and over a `WISH_TRANSPORT=desktop`
 * default. Arguments after `--` (the app's own) are left untouched.
 *
 * @param argv     Argument pointers; modified in place. argv[0] stays first.
 * @param storage  Receives the two inserted strings; must outlive @p argv's
 *                 use, and must not be modified meanwhile.
 * @param endpoint The desktop endpoint name (see desktop_endpoint()).
 */
inline void insert_desktop_transport_args(
    std::vector<char*>& argv,
    std::array<std::string, 2>& storage,
    const std::string& endpoint) {
  storage[0] = "--transport=pipe";
  storage[1] = "--name=" + endpoint;
  auto pos = argv.begin() + (argv.empty() ? 0 : 1);
  for (; pos != argv.end(); ++pos) {
    if (std::string(*pos) == "--")
      break;
  }
  argv.insert(pos, {storage[0].data(), storage[1].data()});
}

} // namespace bdg::wish
