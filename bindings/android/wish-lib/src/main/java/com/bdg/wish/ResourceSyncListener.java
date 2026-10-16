// MIT License © 2026 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

/**
 * Progress and completion callback of {@link Client#syncResourcePackage}.
 * Wraps {@code wish_resource_sync_fn} (see {@code wish_client_c.h}).
 *
 * <p>Fires on a background thread: zero or more {@link Client#SYNC_PROGRESS}
 * calls, then exactly one final call with {@link Client#SYNC_UPLOADED},
 * {@link Client#SYNC_CACHED} or {@link Client#SYNC_FAILED}.
 */
public interface ResourceSyncListener {
  /**
   * @param name        package name
   * @param state       one of the {@code Client.SYNC_*} constants
   * @param transferred bytes uploaded so far
   * @param total       size of the local zip
   * @param error       failure message for {@link Client#SYNC_FAILED}, else {@code null}
   */
  void onUpdate(String name, int state, long transferred, long total, String error);
}
