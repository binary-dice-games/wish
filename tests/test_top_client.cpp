// MIT License © 2026 Binary Dice Games
/// @file test_top_client.cpp
/// @brief Tests top's client-side sampler (process_info_source) against the
///        real machine: rates, and a listening socket opened by the test.
#include "modules/bdg/desktop/top/client/process_info.hpp"

#include <gtest/gtest.h>
#include <uv.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace bdg::wish;

namespace {

int current_pid() {
  return static_cast<int>(uv_os_getpid());
}

} // namespace

// ── Helpers ──────────────────────────────────────────────────────────────────

TEST(TopCounterRate, DividesDeltaByElapsedTime) {
  EXPECT_DOUBLE_EQ(counter_rate(1000, 3000, 2.0), 1000.0);
}

TEST(TopCounterRate, BackwardsCounterOrNoElapsedTimeIsZero) {
  EXPECT_DOUBLE_EQ(counter_rate(3000, 1000, 1.0), 0.0);
  EXPECT_DOUBLE_EQ(counter_rate(1000, 3000, 0.0), 0.0);
}

TEST(TopSortPorts, OrdersByPortThenProtocolThenAddress) {
  std::vector<open_port> ports{
      {"UDP", "0.0.0.0", 53, 0, ""},
      {"TCP6", "::", 22, 0, ""},
      {"TCP", "127.0.0.1", 53, 0, ""},
      {"TCP", "0.0.0.0", 22, 0, ""},
  };
  sort_ports(ports);
  ASSERT_EQ(ports.size(), 4u);
  EXPECT_EQ(ports[0].protocol + ports[0].address, "TCP0.0.0.0");
  EXPECT_EQ(ports[1].protocol, "TCP6");
  EXPECT_EQ(ports[2].protocol + ports[2].address, "TCP127.0.0.1");
  EXPECT_EQ(ports[3].protocol, "UDP");
}

// ── Live sampling ────────────────────────────────────────────────────────────

TEST(TopSampler, FirstSampleReportsZeroRates) {
  process_info_source source;
  auto snap = source.sample();
  EXPECT_EQ(snap.system.net_rx_bytes_per_sec, 0.0);
  EXPECT_EQ(snap.system.net_tx_bytes_per_sec, 0.0);
  EXPECT_EQ(snap.system.disk_read_bytes_per_sec, 0.0);
  EXPECT_EQ(snap.system.disk_write_bytes_per_sec, 0.0);
  EXPECT_FALSE(snap.processes.empty());
}

TEST(TopSampler, RatesAreNonNegativeAndPortsSorted) {
  process_info_source source;
  source.sample();
  auto snap = source.sample();
  EXPECT_GE(snap.system.net_rx_bytes_per_sec, 0.0);
  EXPECT_GE(snap.system.net_tx_bytes_per_sec, 0.0);
  EXPECT_GE(snap.system.disk_read_bytes_per_sec, 0.0);
  EXPECT_GE(snap.system.disk_write_bytes_per_sec, 0.0);
  auto sorted = snap.ports;
  sort_ports(sorted);
  ASSERT_EQ(sorted.size(), snap.ports.size());
  for (size_t i = 0; i < sorted.size(); ++i)
    EXPECT_EQ(sorted[i].port, snap.ports[i].port);
}

// A TCP socket this test puts into the listening state shows up, attributed
// to this process.
TEST(TopSampler, ReportsOwnListeningTcpSocket) {
  uv_loop_t loop;
  ASSERT_EQ(uv_loop_init(&loop), 0);
  uv_tcp_t server;
  ASSERT_EQ(uv_tcp_init(&loop, &server), 0);
  sockaddr_in bind_addr{};
  ASSERT_EQ(uv_ip4_addr("127.0.0.1", 0, &bind_addr), 0);
  ASSERT_EQ(uv_tcp_bind(&server, reinterpret_cast<const sockaddr*>(&bind_addr), 0), 0);
  ASSERT_EQ(uv_listen(reinterpret_cast<uv_stream_t*>(&server), 1, [](uv_stream_t*, int) {}), 0);
  sockaddr_in bound{};
  int len = sizeof(bound);
  ASSERT_EQ(uv_tcp_getsockname(&server, reinterpret_cast<sockaddr*>(&bound), &len), 0);
  const int port = ntohs(bound.sin_port);

  process_info_source source;
  auto snap = source.sample();
  auto it = std::find_if(
      snap.ports.begin(), snap.ports.end(), [&](const open_port& p) { return p.protocol == "TCP" && p.port == port; });
  ASSERT_NE(it, snap.ports.end()) << "port " << port << " not reported";
  EXPECT_EQ(it->address, "127.0.0.1");
  EXPECT_EQ(it->pid, current_pid());
  EXPECT_FALSE(it->process.empty());

  uv_close(reinterpret_cast<uv_handle_t*>(&server), nullptr);
  uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);
}
