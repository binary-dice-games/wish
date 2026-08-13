// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Hashes field/class/event names into the {@code bison_hash} (32-bit
 * FNV-1a) identifiers the wire format and the C ABI key on. Wraps {@code
 * wish_key()} (identical to bison's {@code bison_key()}; re-exported by
 * {@code wish_client_c.h} so wish-only callers don't need bison's own
 * binding).
 *
 * <p>C++'s {@code "name"_key} is a {@code constexpr} evaluated at compile
 * time; Java has no equivalent, so every lookup calls across the ABI the
 * same way the Python and C# bindings do. Results are memoized in a small
 * bounded cache -- field/event names are drawn from a static,
 * schema-defined set reused across many calls, so this turns most lookups
 * into a map hit instead of a JNI round trip, while staying bounded so a
 * caller hashing high-cardinality strings can't grow it forever. Mirrors
 * bison's own {@code com.bdg.bison.Key}.
 */
public final class Key {
  static {
    NativeLibrary.ensureLoaded();
  }

  private static final int MAX_ENTRIES = 4096;

  // Access-order LinkedHashMap doubling as an LRU cache via removeEldestEntry.
  private static final Map<String, Integer> CACHE =
      new LinkedHashMap<String, Integer>(16, 0.75f, true) {
        @Override
        protected boolean removeEldestEntry(Map.Entry<String, Integer> eldest) {
          return size() > MAX_ENTRIES;
        }
      };

  /** Hashes {@code name}, matching the internal C++ {@code "name"_key} / {@code hash()}. */
  public static synchronized int of(String name) {
    if (name == null || name.isEmpty()) return 0;
    Integer cached = CACHE.get(name);
    if (cached != null) return cached;
    int hash = nativeKey(name);
    CACHE.put(name, hash);
    return hash;
  }

  private static native int nativeKey(String name);

  private Key() {}
}
