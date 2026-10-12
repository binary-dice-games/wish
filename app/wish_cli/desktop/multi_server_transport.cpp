// MIT License © 2026 Binary Dice Games
/**
 * @file multi_server_transport.cpp
 * @brief multi_server_transport implementation.
 */
#include "app/wish_cli/desktop/multi_server_transport.hpp"

namespace bdg::wish {

namespace transport = bison::rmi::transport;

namespace {
/// How long each accept thread blocks in its transport's accept() before
/// re-checking for stop().
constexpr std::chrono::milliseconds kPumpSlice{100};
} // namespace

multi_server_transport::multi_server_transport(transport::server_transport_iface& primary) : primary_(primary) {}

multi_server_transport::~multi_server_transport() {
  stop();
}

void multi_server_transport::add(std::unique_ptr<transport::server_transport_iface> transport) {
  extras_.push_back(std::move(transport));
}

void multi_server_transport::start(bison::dynamic params) {
  if (started_)
    return;

  std::vector<transport::server_transport_iface*> started;
  try {
    primary_.start(std::move(params));
    started.push_back(&primary_);
    for (auto& extra : extras_) {
      extra->start(bison::dynamic{});
      started.push_back(extra.get());
    }
  } catch (...) {
    for (auto* t : started)
      t->stop();
    throw;
  }

  started_ = true;
  stopping_.store(false);
  for (auto* t : started)
    pumps_.emplace_back([this, t] { pump(*t); });
}

void multi_server_transport::pump(transport::server_transport_iface& transport) {
  while (!stopping_.load()) {
    connection_ptr conn = transport.accept(kPumpSlice);
    if (!conn)
      continue;
    accepted_.withWLock([&](auto& q) { q.push_back(std::move(conn)); });
    accepted_.notify_one();
  }
}

std::unique_ptr<transport::server_connection_iface> multi_server_transport::accept(std::chrono::milliseconds timeout) {
  if (!accepted_.wait_for(timeout, [this](auto& q) { return !q.empty() || stopping_.load(); }))
    return nullptr;
  return accepted_.withWLock([](auto& q) -> connection_ptr {
    if (q.empty())
      return nullptr;
    connection_ptr conn = std::move(q.front());
    q.pop_front();
    return conn;
  });
}

void multi_server_transport::stop() {
  if (!started_ || stopping_.exchange(true))
    return;
  // Join the accept threads before stopping the transports: a transport's
  // stop() may free state its accept() is still using on another thread
  // (named_pipe_server_transport does). Each thread notices stopping_
  // within kPumpSlice.
  for (auto& p : pumps_)
    p.join();
  pumps_.clear();
  primary_.stop();
  for (auto& extra : extras_)
    extra->stop();
  accepted_.notify_all();
}

} // namespace bdg::wish
