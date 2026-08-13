// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

/**
 * Callback invoked by {@link Client#run} once the connection is
 * established. Wraps {@code wish_session_fn} (see {@code
 * wish_client_c.h}).
 *
 * <p>Runs on the library's internal RMI worker thread. {@link Client#run}
 * blocks until this returns; call {@link Client#waitForQuit} inside it to
 * keep the session alive while {@link EventHandler}s update the UI, and end
 * it with {@link Client#quit} (typically from an event handler).
 */
public interface SessionCallback {
  void run(Client client);
}
