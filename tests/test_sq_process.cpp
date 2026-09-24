// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include "modules/bdg/dev/sq/client/sq_process.hpp"

#include <string>

using bdg::wish::sq::run_sq_cli;

namespace {

// run_sq_cli() is exercised with stub binaries (never `sq`) so these pass on
// any machine, with or without sq installed -- see the `binary` parameter's
// doc comment in sq_process.hpp.

TEST(SqProcessTest, CapturesStdoutFromAStubBinary) {
  auto r = run_sq_cli({"hello\tworld"}, "printf");
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.stdout_text, "hello\tworld");
}

TEST(SqProcessTest, ReportsNonZeroExitCode) {
  auto r = run_sq_cli({}, "false");
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.exit_code, 1);
}

TEST(SqProcessTest, MissingBinaryReportsSpawnFailure) {
  auto r = run_sq_cli({"version"}, "definitely-not-a-real-binary-xyzzy");
  EXPECT_EQ(r.exit_code, -1);
  EXPECT_FALSE(r.stderr_text.empty());
}

TEST(SqProcessTest, ArgsWithSpacesAndQuotesNeedNoEscaping) {
  // (std::string, not two bare literals: {"a", "b"} would pick vector's
  // iterator-range constructor.)
  auto r = run_sq_cli({std::string{"%s"}, "select * from \"t\" where a = 'x y'"}, "printf");
  EXPECT_EQ(r.stdout_text, "select * from \"t\" where a = 'x y'");
}

TEST(SqProcessTest, StdinTextIsDeliveredToTheChild) {
  auto r = run_sq_cli({}, "cat", "s3cret\n");
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.stdout_text, "s3cret\n");
}

TEST(SqProcessTest, ChildThatIgnoresStdinDoesNotCrashUs) {
  // `true` exits without reading; the write must not raise SIGPIPE.
  auto r = run_sq_cli({}, "true", std::string(1 << 20, 'x'));
  EXPECT_TRUE(r.ok());
}

} // namespace
