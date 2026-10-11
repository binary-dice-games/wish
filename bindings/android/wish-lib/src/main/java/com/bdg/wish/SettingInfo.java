// MIT License © 2026 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

import java.util.ArrayList;
import java.util.List;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * Metadata of one setting, for {@link Client#describeSetting}: field {@code
 * color} of the user store entry {@code bdg.desktop.example} is named {@code
 * "bdg.desktop.example.color"}. See docs/settings.md.
 *
 * <pre>{@code
 * client.describeSetting(new SettingInfo("bdg.desktop.example.color", "color")
 *     .label("Color").category("example").defaultValue("#2F7BF5"));
 * }</pre>
 */
public final class SettingInfo {
  private final String name;
  private final String type;
  private String label = "";
  private String description = "";
  private String category = "";
  private String section = "";
  private Object defaultValue;
  private Double min;
  private Double max;
  private Double step;
  private final List<String[]> options = new ArrayList<>();
  private String icon = "";
  private int order;
  private boolean restart;

  /**
   * @param name Dotted name: the entry, a dot, then the field.
   * @param type bool, int, float, string, enum, color or path.
   */
  public SettingInfo(String name, String type) {
    this.name = name;
    this.type = type;
  }

  public String name() { return name; }

  public SettingInfo label(String v) { label = v; return this; }
  public SettingInfo description(String v) { description = v; return this; }
  /** Sidebar category id. */
  public SettingInfo category(String v) { category = v; return this; }
  /** Group title inside the category. */
  public SettingInfo section(String v) { section = v; return this; }
  /** A Boolean, Integer, Double or String; {@code null} for none. */
  public SettingInfo defaultValue(Object v) { defaultValue = v; return this; }
  public SettingInfo min(double v) { min = v; return this; }
  public SettingInfo max(double v) { max = v; return this; }
  public SettingInfo step(double v) { step = v; return this; }
  /** Adds a choice of an enum setting; an empty label shows the value. */
  public SettingInfo option(String value, String label) { options.add(new String[] {value, label}); return this; }
  public SettingInfo icon(String v) { icon = v; return this; }
  public SettingInfo order(int v) { order = v; return this; }
  /** True when a change takes effect only after a restart. */
  public SettingInfo restart(boolean v) { restart = v; return this; }

  /** The JSON object {@code wish_settings_describe()} takes. */
  public String toJson() {
    try {
      JSONObject j = new JSONObject();
      j.put("name", name);
      j.put("type", type);
      j.put("label", label);
      j.put("description", description);
      j.put("category", category);
      j.put("section", section);
      j.put("default", defaultValue == null ? JSONObject.NULL : defaultValue);
      if (min != null) j.put("min", min.doubleValue());
      if (max != null) j.put("max", max.doubleValue());
      if (step != null) j.put("step", step.doubleValue());
      if (!options.isEmpty()) {
        JSONArray arr = new JSONArray();
        for (String[] o : options) arr.put(new JSONObject().put("value", o[0]).put("label", o[1]));
        j.put("options", arr);
      }
      j.put("icon", icon);
      j.put("order", order);
      j.put("restart", restart);
      return j.toString();
    } catch (JSONException e) {
      throw new WishException(-4 /* WISH_ERR_EXCEPTION */, "setting " + name + ": " + e.getMessage());
    }
  }
}
