// MIT License © 2026 Binary Dice Games
/// @file session_event_recorder.hpp
/// @brief Thread-safe capture of the events a wish session emits to its
///        client, for server-side form tests.
///
/// Why this exists: tests used to swap `last_session->emit_event` for a
/// `[&]` lambda capturing test-body locals (a `got` flag, a `cap` payload,
/// the previous `emit_event` as `prev`). The render loop copies
/// `emit_event` under the session lock and calls the copy *after*
/// releasing it, so an event delivered late -- e.g. from a click fired
/// earlier in the test, or during `TearDown()`'s `server::stop()` -- ran a
/// closure whose captures had already gone out of scope: an intermittent
/// SIGSEGV. The swap itself also raced the render loop's read, and the
/// polled `bool` flags were written from another thread.
///
/// Instead, `attach()` wraps `emit_event` exactly once, from the server's
/// `on_session_created()` hook (which runs under the session's write lock,
/// before any dispatch or render), and the wrapper owns the recorder via
/// `shared_ptr` -- nothing it touches can dangle. Tests never write
/// `emit_event`; they wait for or inspect recorded events.
///
/// Typical use in a fixture's server subclass:
/// @code
///   void on_session_created(wish::context& s) override {
///     last_session = &s;
///     session_event_recorder::attach(events, s);
///   }
///   std::shared_ptr<session_event_recorder> events = std::make_shared<session_event_recorder>();
/// @endcode
/// and in a test:
/// @code
///   auto since = events.mark();
///   fire(button, "clicked"_key);
///   auto payload = events.wait_for("refresh_requested"_key, since);
///   ASSERT_TRUE(payload);
/// @endcode
#pragma once

#include <context/context.hpp>

#include "src/bison/bison_object.hpp"
#include "src/bison/bison_sync.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

/// One event a session emitted to its client.
struct recorded_event {
  bdg::bison::key_t id; ///< `__wish_id` / object id the event was raised on.
  bdg::bison::key_t name; ///< Event name (e.g. `"clicked"_key`).
  bdg::bison::dynamic payload;
};

/**
 * @brief Records every event a session emits, in arrival order, and lets a
 *        test wait for one.
 *
 * All methods are thread-safe. Events are indexed from 0 in arrival order;
 * `mark()` returns the index the next event will get, so passing it as
 * `since` restricts a query to events emitted after the mark.
 */
class session_event_recorder {
 public:
  /// Predicate over a recorded event's payload.
  using payload_pred = std::function<bool(const bdg::bison::dynamic&)>;

  /// Default wait for an event that is expected to arrive.
  static constexpr std::chrono::milliseconds kDefaultTimeout{2000};

  /**
   * @brief Wrap @p s's `emit_event` so every event is recorded into
   *        @p recorder before being forwarded to the client.
   *
   * Call only from the server's `on_session_created()` hook: that is the one
   * point where `emit_event` is already set and no other thread can be
   * reading it.
   */
  static void attach(const std::shared_ptr<session_event_recorder>& recorder, bdg::wish::context& s) {
    s.emit_event = [recorder, prev = std::move(s.emit_event)](
                       bdg::bison::key_t id, bdg::bison::key_t name, bdg::bison::dynamic payload) {
      recorder->record(id, name, payload);
      if (prev)
        prev(id, name, std::move(payload));
    };
  }

  /// Append an event (payload is deep-copied) and wake any waiter.
  void record(bdg::bison::key_t id, bdg::bison::key_t name, const bdg::bison::dynamic& payload) {
    events_.wlock()->push_back({id, name, payload.clone()});
    events_.notify_all();
  }

  /// @return The index the next recorded event will get.
  size_t mark() const {
    return events_.rlock()->size();
  }

  /**
   * @brief Wait for an event named @p name, recorded at index >= @p since,
   *        whose payload satisfies @p pred (any payload when empty).
   * @return A copy of the first matching payload, or `std::nullopt` on
   *         timeout.
   */
  std::optional<bdg::bison::dynamic> wait_for(
      bdg::bison::key_t name,
      size_t since = 0,
      const payload_pred& pred = {},
      std::chrono::milliseconds timeout = kDefaultTimeout) {
    std::optional<bdg::bison::dynamic> found;
    events_.wait_for(timeout, [&](const std::vector<recorded_event>& evs) {
      found = find_in(evs, name, since, pred);
      return found.has_value();
    });
    return found;
  }

  /**
   * @brief Non-blocking check: has a matching event already been recorded?
   *
   * For "must NOT be emitted" assertions, first wait for something that is
   * emitted after it would have been (or use `wait_for()` with a short
   * timeout) -- this only inspects what has arrived so far.
   */
  bool saw(bdg::bison::key_t name, size_t since = 0, const payload_pred& pred = {}) const {
    return find_in(*events_.rlock(), name, since, pred).has_value();
  }

  /// @return A deep copy of every event recorded at index >= @p since.
  std::vector<recorded_event> snapshot(size_t since = 0) const {
    auto evs = events_.rlock();
    std::vector<recorded_event> out;
    for (size_t i = since; i < evs->size(); ++i)
      out.push_back({(*evs)[i].id, (*evs)[i].name, (*evs)[i].payload.clone()});
    return out;
  }

 private:
  static std::optional<bdg::bison::dynamic>
  find_in(const std::vector<recorded_event>& evs, bdg::bison::key_t name, size_t since, const payload_pred& pred) {
    for (size_t i = since; i < evs.size(); ++i)
      if (evs[i].name == name && (!pred || pred(evs[i].payload)))
        return evs[i].payload.clone();
    return std::nullopt;
  }

  bdg::bison::synchronized<std::vector<recorded_event>> events_;
};
