// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

/**
 * Thrown when a {@code wish_client_c.h} call fails. {@link #code} is the
 * raw {@code wish_error} value (see {@code include/wish_client_c.h}), e.g.
 * {@code WISH_ERR_NOT_FOUND} (-2) or {@code WISH_ERR_AMBIGUOUS} (-5).
 */
public final class WishException extends RuntimeException {
  public final int code;

  public WishException(int code, String message) {
    super(message);
    this.code = code;
  }
}
