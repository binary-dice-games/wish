// MIT License © 2026 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

import org.json.JSONException;
import org.json.JSONObject;

/** Metadata of one sidebar category, for {@link Client#describeSettingsCategory}. */
public final class SettingsCategory {
  private final String id;
  private String label = "";
  private String description = "";
  private String icon = "";
  private String color = "";
  private int order;

  public SettingsCategory(String id) {
    this.id = id;
  }

  public String id() { return id; }

  public SettingsCategory label(String v) { label = v; return this; }
  public SettingsCategory description(String v) { description = v; return this; }
  public SettingsCategory icon(String v) { icon = v; return this; }
  /** Badge color, {@code "#RRGGBB"}. */
  public SettingsCategory color(String v) { color = v; return this; }
  public SettingsCategory order(int v) { order = v; return this; }

  /** The JSON object {@code wish_settings_describe_category()} takes. */
  public String toJson() {
    try {
      return new JSONObject()
          .put("id", id)
          .put("label", label)
          .put("description", description)
          .put("icon", icon)
          .put("color", color)
          .put("order", order)
          .toString();
    } catch (JSONException e) {
      throw new WishException(-4 /* WISH_ERR_EXCEPTION */, "category " + id + ": " + e.getMessage());
    }
  }
}
