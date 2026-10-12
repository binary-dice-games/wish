// MIT License © 2026 Binary Dice Games
//
// Exercises insert_desktop_transport_args()
// (app/wish_cli/client/desktop_transport_args.hpp), behind
// `wish client --transport=desktop`: after the rewrite, a gflags parse of
// the command line must end with a plain pipe transport to the endpoint,
// whichever way "desktop" was given, and must leave the app's own
// arguments after `--` alone.
#include "app/wish_cli/client/desktop_transport_args.hpp"

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

DEFINE_string(transport, "term", "test flag");
DEFINE_string(name, "", "test flag");

namespace wish = bdg::wish;

namespace {

const std::string kEndpoint = "@wish-desktop-test";

struct parsed {
  std::string transport;
  std::string name;
  std::vector<std::string> rest; // argv[1..] left after parsing
};

// Rewrites @p args as wish_client_app::run() does, then parses them.
parsed rewrite_and_parse(std::vector<std::string> args) {
  FLAGS_transport = "term";
  FLAGS_name = "";
  std::vector<char*> argv;
  for (auto& a : args)
    argv.push_back(a.data());
  std::array<std::string, 2> storage;
  wish::insert_desktop_transport_args(argv, storage, kEndpoint);

  int argc = static_cast<int>(argv.size());
  char** argp = argv.data();
  gflags::ParseCommandLineFlags(&argc, &argp, true);
  parsed p{FLAGS_transport, FLAGS_name, {}};
  for (int i = 1; i < argc; ++i)
    p.rest.emplace_back(argp[i]);
  return p;
}

} // namespace

TEST(DesktopTransportArgs, EqualsFormBecomesPipeToEndpoint) {
  auto p = rewrite_and_parse({"wish", "--name=ignored", "--transport=desktop"});
  EXPECT_EQ(p.transport, "pipe");
  EXPECT_EQ(p.name, kEndpoint);
}

TEST(DesktopTransportArgs, TwoTokenFormBecomesPipeToEndpoint) {
  auto p = rewrite_and_parse({"wish", "--transport", "desktop", "--name", "ignored"});
  EXPECT_EQ(p.transport, "pipe");
  EXPECT_EQ(p.name, kEndpoint);
}

TEST(DesktopTransportArgs, EnvironmentDefaultFormGetsTheArguments) {
  // WISH_TRANSPORT=desktop arrives as a flag default, not an argument.
  auto p = rewrite_and_parse({"wish"});
  EXPECT_EQ(p.transport, "pipe");
  EXPECT_EQ(p.name, kEndpoint);
}

TEST(DesktopTransportArgs, AppArgumentsAfterDoubleDashAreUntouched) {
  auto p = rewrite_and_parse({"wish", "--transport=desktop", "--", "-f", "--transport=x"});
  EXPECT_EQ(p.transport, "pipe");
  EXPECT_EQ(p.name, kEndpoint);
  EXPECT_EQ(p.rest, (std::vector<std::string>{"-f", "--transport=x"}));
}
