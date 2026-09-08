// MIT License © 2026 Binary Dice Games
/// @file test_mi_parser.cpp
/// @brief Unit tests for the GDB/LLDB MI output-record parser
///        (modules/bdg/dev/dbg/client/mi_parser.{hpp,cpp}), which
///        posix_debug_backend relies on to interpret every line the child
///        debugger emits. Pure parser -- no subprocess -- so this runs on
///        every platform, including Windows where posix_debug_backend is
///        #if-d out.
#include "modules/bdg/dev/dbg/client/mi_parser.hpp"

#include <gtest/gtest.h>

namespace bdg::wish::dbg {
namespace {

TEST(MiParserTest, BlankAndBannerLinesAreIgnored) {
  EXPECT_FALSE(parse_mi_line("").has_value());
  EXPECT_FALSE(parse_mi_line("\n").has_value());
  EXPECT_FALSE(parse_mi_line("GNU gdb (Ubuntu) 12.1").has_value());
}

TEST(MiParserTest, PromptRecord) {
  auto rec = parse_mi_line("(gdb) ");
  ASSERT_TRUE(rec.has_value());
  EXPECT_EQ(rec->type, mi_record::kind::prompt);
}

TEST(MiParserTest, SimpleResultDone) {
  auto rec = parse_mi_line("^done");
  ASSERT_TRUE(rec.has_value());
  EXPECT_EQ(rec->type, mi_record::kind::result);
  EXPECT_EQ(rec->klass, "done");
  EXPECT_FALSE(rec->is_error());
}

TEST(MiParserTest, ResultWithLeadingToken) {
  auto rec = parse_mi_line("0042^done,value=\"7\"");
  ASSERT_TRUE(rec.has_value());
  EXPECT_EQ(rec->token, "0042");
  EXPECT_EQ(rec->klass, "done");
  EXPECT_EQ(rec->get("value"), "7");
}

TEST(MiParserTest, RunningAndErrorClasses) {
  auto run = parse_mi_line("^running");
  ASSERT_TRUE(run.has_value());
  EXPECT_EQ(run->klass, "running");
  EXPECT_FALSE(run->is_error());

  auto err = parse_mi_line("^error,msg=\"No symbol \\\"foo\\\" in current context.\"");
  ASSERT_TRUE(err.has_value());
  EXPECT_TRUE(err->is_error());
  EXPECT_EQ(err->get("msg"), "No symbol \"foo\" in current context.");
}

TEST(MiParserTest, CStringEscapes) {
  EXPECT_EQ(mi_unescape("a\\tb\\nc"), "a\tb\nc");
  EXPECT_EQ(mi_unescape("path \\\"x\\\" end"), "path \"x\" end");
  EXPECT_EQ(mi_unescape("back\\\\slash"), "back\\slash");
}

TEST(MiParserTest, ConsoleStreamRecord) {
  auto rec = parse_mi_line("~\"type = int\\n\"");
  ASSERT_TRUE(rec.has_value());
  EXPECT_EQ(rec->type, mi_record::kind::console_stream);
  EXPECT_EQ(rec->klass, "type = int\n");
}

TEST(MiParserTest, TargetAndLogStreamRecords) {
  auto tgt = parse_mi_line("@\"hello from debuggee\\n\"");
  ASSERT_TRUE(tgt.has_value());
  EXPECT_EQ(tgt->type, mi_record::kind::target_stream);
  EXPECT_EQ(tgt->klass, "hello from debuggee\n");

  auto log = parse_mi_line("&\"warning: bla\\n\"");
  ASSERT_TRUE(log.has_value());
  EXPECT_EQ(log->type, mi_record::kind::log_stream);
}

TEST(MiParserTest, StoppedWithNestedFrameTuple) {
  auto rec = parse_mi_line(
      "*stopped,reason=\"breakpoint-hit\",disp=\"keep\",bkptno=\"1\","
      "frame={addr=\"0x0000555555555149\",func=\"inner_function\",args=[{name=\"x\",value=\"3\"}],"
      "file=\"dbg_fixture.cpp\",fullname=\"/tmp/dbg_fixture.cpp\",line=\"24\"},"
      "thread-id=\"1\",stopped-threads=\"all\",core=\"2\"");
  ASSERT_TRUE(rec.has_value());
  EXPECT_EQ(rec->type, mi_record::kind::exec_async);
  EXPECT_EQ(rec->klass, "stopped");
  EXPECT_EQ(rec->get("reason"), "breakpoint-hit");
  EXPECT_EQ(rec->get("thread-id"), "1");

  const mi_value* frame = rec->find("frame");
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->type, mi_value::kind::tuple);
  EXPECT_EQ(frame->get("func"), "inner_function");
  EXPECT_EQ(frame->get("fullname"), "/tmp/dbg_fixture.cpp");
  EXPECT_EQ(frame->get("line"), "24");

  const mi_value* args = frame->find("args");
  ASSERT_NE(args, nullptr);
  EXPECT_EQ(args->type, mi_value::kind::list);
  ASSERT_EQ(args->values.size(), 1u);
  EXPECT_EQ(args->values[0].get("name"), "x");
  EXPECT_EQ(args->values[0].get("value"), "3");
}

TEST(MiParserTest, BreakInsertResult) {
  auto rec = parse_mi_line(
      "^done,bkpt={number=\"2\",type=\"breakpoint\",disp=\"keep\",enabled=\"y\","
      "addr=\"0x000055555555514e\",func=\"inner_function\",file=\"dbg_fixture.cpp\","
      "fullname=\"/tmp/dbg_fixture.cpp\",line=\"24\",thread-groups=[\"i1\"],times=\"0\"}");
  ASSERT_TRUE(rec.has_value());
  const mi_value* bkpt = rec->find("bkpt");
  ASSERT_NE(bkpt, nullptr);
  EXPECT_EQ(bkpt->get("number"), "2");
  EXPECT_EQ(bkpt->get("line"), "24");
  const mi_value* groups = bkpt->find("thread-groups");
  ASSERT_NE(groups, nullptr);
  ASSERT_EQ(groups->values.size(), 1u);
  EXPECT_EQ(groups->values[0].str, "i1");
}

TEST(MiParserTest, StackListFramesResultStyleList) {
  auto rec = parse_mi_line(
      "^done,stack=[frame={level=\"0\",func=\"inner_function\",file=\"a.cpp\",fullname=\"/x/a.cpp\",line=\"24\"},"
      "frame={level=\"1\",func=\"outer_function\",file=\"a.cpp\",fullname=\"/x/a.cpp\",line=\"29\"},"
      "frame={level=\"2\",func=\"main\",file=\"a.cpp\",fullname=\"/x/a.cpp\",line=\"41\"}]");
  ASSERT_TRUE(rec.has_value());
  const mi_value* stack = rec->find("stack");
  ASSERT_NE(stack, nullptr);
  EXPECT_EQ(stack->type, mi_value::kind::list);
  ASSERT_EQ(stack->items.size(), 3u);
  EXPECT_EQ(stack->items[0].first, "frame");
  EXPECT_EQ(stack->items[0].second.get("func"), "inner_function");
  EXPECT_EQ(stack->items[2].second.get("func"), "main");
  EXPECT_EQ(stack->items[2].second.get("line"), "41");
}

TEST(MiParserTest, ThreadInfoResult) {
  auto rec = parse_mi_line(
      "^done,threads=[{id=\"1\",target-id=\"process 1234\",frame={level=\"0\",func=\"main\",line=\"41\"},"
      "state=\"stopped\"}],current-thread-id=\"1\"");
  ASSERT_TRUE(rec.has_value());
  const mi_value* threads = rec->find("threads");
  ASSERT_NE(threads, nullptr);
  ASSERT_EQ(threads->values.size(), 1u);
  EXPECT_EQ(threads->values[0].get("id"), "1");
  EXPECT_EQ(threads->values[0].get("state"), "stopped");
  const mi_value* frame = threads->values[0].find("frame");
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->get("func"), "main");
  EXPECT_EQ(rec->get("current-thread-id"), "1");
}

TEST(MiParserTest, NotifyAsyncRecord) {
  auto rec = parse_mi_line("=thread-created,id=\"2\",group-id=\"i1\"");
  ASSERT_TRUE(rec.has_value());
  EXPECT_EQ(rec->type, mi_record::kind::notify_async);
  EXPECT_EQ(rec->klass, "thread-created");
  EXPECT_EQ(rec->get("id"), "2");
}

TEST(MiParserTest, EmptyTupleAndList) {
  auto rec = parse_mi_line("^done,a={},b=[]");
  ASSERT_TRUE(rec.has_value());
  const mi_value* a = rec->find("a");
  const mi_value* b = rec->find("b");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a->type, mi_value::kind::tuple);
  EXPECT_TRUE(a->items.empty());
  EXPECT_EQ(b->type, mi_value::kind::list);
  EXPECT_TRUE(b->values.empty());
}

} // namespace
} // namespace bdg::wish::dbg
