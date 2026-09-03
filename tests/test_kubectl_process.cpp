// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include "modules/bdg/dev/kubectl/client/kubectl_process.hpp"

using bdg::wish::kubectl::run_kubectl_cli;

namespace {

// run_kubectl_cli() is exercised with stub binaries (never `kubectl`) so
// these pass on any machine, with or without a cluster -- see the `binary`
// parameter's doc comment in kubectl_process.hpp.

TEST(KubectlProcessTest, CapturesStdoutFromAStubBinary) {
  auto r = run_kubectl_cli({"ns\tname"}, "printf");
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(r.stdout_text, "ns\tname");
}

TEST(KubectlProcessTest, ReportsNonZeroExitCode) {
  auto r = run_kubectl_cli({}, "false");
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.exit_code, 1);
}

TEST(KubectlProcessTest, MissingBinaryReportsSpawnFailure) {
  auto r = run_kubectl_cli({"version"}, "definitely-not-a-real-binary-xyzzy");
  EXPECT_EQ(r.exit_code, -1);
  EXPECT_FALSE(r.stderr_text.empty());
}

TEST(KubectlProcessTest, EmptyBinaryIsRejected) {
  auto r = run_kubectl_cli({"version"}, "");
  EXPECT_EQ(r.exit_code, -1);
}

TEST(KubectlProcessTest, JsonpathTemplateArgNeedsNoEscaping) {
  // No shell in the pipeline, so a template arg full of braces / quotes /
  // spaces is one argv entry passed through verbatim.
  auto r = run_kubectl_cli({"%s\n", "{range .items[*]}{.metadata.name}{\"\\n\"}{end}"}, "printf");
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.stdout_text, "{range .items[*]}{.metadata.name}{\"\\n\"}{end}\n");
}

} // namespace
