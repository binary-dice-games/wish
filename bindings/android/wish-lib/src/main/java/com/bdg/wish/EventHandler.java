// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

/**
 * Callback registered with {@link Proxy#onEvent}, invoked when the server
 * pushes an event to this proxy (e.g. a button's {@code "clicked"} event).
 * Wraps {@code rmi_proxy_event_fn} (see {@code rmi_c.h}).
 *
 * <p>Fires on the library's internal RMI worker thread, not the thread that
 * registered the handler -- update UI state accordingly (e.g. post back to
 * the main thread). {@code params} is <em>borrowed</em>: valid only for the
 * duration of this call and must not be closed.
 */
public interface EventHandler {
  void onEvent(Dynamic params);
}
