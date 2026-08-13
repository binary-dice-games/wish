// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

/** One parameter accepted by an embedded app (see {@link Client#listApps}). */
public final class AppParamInfo {
  public final String name;
  public final String description;

  public AppParamInfo(String name, String description) {
    this.name = name;
    this.description = description;
  }
}
