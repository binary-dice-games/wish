// MIT License © 2026 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

/**
 * Events of a {@link MediaPlayer}. Wraps {@code wish_media_event_fn} (see
 * {@code wish_client_c.h}). Fires on the client's event thread; must not call
 * {@link MediaPlayer#setListener} or {@link MediaPlayer#close} on the player.
 */
public interface MediaListener {
  /**
   * @param event   one of the {@code MediaPlayer.EVENT_*} constants
   * @param state   the player's new {@code MediaPlayer.STATE_*}
   * @param message error text for {@link MediaPlayer#EVENT_ERROR}, else ""
   */
  void onEvent(int event, int state, String message);
}
