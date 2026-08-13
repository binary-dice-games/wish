// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

/**
 * Thrown when a {@code bison_c.h} call fails. {@link #code} is the raw
 * {@code bison_error} value (see {@code extern/bison/include/bison_c.h}),
 * e.g. {@code BISON_ERR_TYPE} (-2) or {@code BISON_ERR_NOT_FOUND} (-3).
 */
public final class BisonException extends RuntimeException {
  public final int code;

  public BisonException(int code, String message) {
    super(message);
    this.code = code;
  }
}
