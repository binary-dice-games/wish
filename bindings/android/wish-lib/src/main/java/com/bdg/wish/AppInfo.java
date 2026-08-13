// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * One embedded app registered by an enabled optional module (see {@code
 * modules/README.md}). {@link #name} alone is what {@link Client#runApp}
 * takes; {@link #organization}/{@link #collection} are the module's
 * location in the {@code modules/<organization>/<collection>/<name>} tree
 * (empty if not populated).
 */
public final class AppInfo {
  public final String name;
  public final String organization;
  public final String collection;
  public final String description;
  public final List<AppParamInfo> params;

  public AppInfo(String name, String organization, String collection, String description, List<AppParamInfo> params) {
    this.name = name;
    this.organization = organization;
    this.collection = collection;
    this.description = description;
    this.params = params;
  }

  /** Parses the JSON array returned by {@code wish_list_apps()}. */
  static List<AppInfo> parseList(String json) {
    List<AppInfo> out = new ArrayList<>();
    JSONArray arr;
    try {
      arr = new JSONArray(json);
    } catch (org.json.JSONException e) {
      throw new WishException(-4 /* WISH_ERR_EXCEPTION */, "malformed list_apps JSON: " + e.getMessage());
    }
    for (int i = 0; i < arr.length(); i++) {
      JSONObject obj = arr.optJSONObject(i);
      if (obj == null) continue;
      List<AppParamInfo> params = new ArrayList<>();
      JSONArray paramsArr = obj.optJSONArray("params");
      if (paramsArr != null) {
        for (int j = 0; j < paramsArr.length(); j++) {
          JSONObject p = paramsArr.optJSONObject(j);
          if (p == null) continue;
          params.add(new AppParamInfo(p.optString("name", ""), p.optString("description", "")));
        }
      }
      out.add(new AppInfo(
          obj.optString("name", ""), obj.optString("organization", ""), obj.optString("collection", ""),
          obj.optString("description", ""), Collections.unmodifiableList(params)));
    }
    return Collections.unmodifiableList(out);
  }
}
