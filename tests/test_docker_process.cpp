// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include "modules/bdg/dev/docker/client/docker_process.hpp"

using bdg::wish::docker::run_docker_cli;

namespace {

// run_docker_cli() is exercised with stub binaries (never `docker`) so these
// pass on any machine, with or without a Docker daemon -- see the `binary`
// parameter's doc comment in docker_process.hpp.

TEST(DockerProcessTest, CapturesStdoutFromAStubBinary) {
  auto r = run_docker_cli({"hello\tworld"}, "printf");
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(r.stdout_text, "hello\tworld");
}

TEST(DockerProcessTest, ReportsNonZeroExitCode) {
  auto r = run_docker_cli({}, "false");
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.exit_code, 1);
}

TEST(DockerProcessTest, MissingBinaryReportsSpawnFailure) {
  auto r = run_docker_cli({"version"}, "definitely-not-a-real-binary-xyzzy");
  EXPECT_EQ(r.exit_code, -1);
  EXPECT_FALSE(r.stderr_text.empty());
}

TEST(DockerProcessTest, EmptyBinaryIsRejected) {
  auto r = run_docker_cli({"version"}, "");
  EXPECT_EQ(r.exit_code, -1);
}

TEST(DockerProcessTest, ArgsWithSpacesNeedNoEscaping) {
  // No shell in the pipeline, so a space-containing arg is one argv entry.
  auto r = run_docker_cli({"%s\n", "a b c"}, "printf");
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.stdout_text, "a b c\n");
}

} // namespace
