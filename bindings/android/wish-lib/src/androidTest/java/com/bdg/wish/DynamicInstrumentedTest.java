// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import org.junit.Test;
import org.junit.runner.RunWith;

/**
 * Runs the binding against the real {@code wish_client}/{@code wish_jni}
 * {@code .so}s on-device -- {@code ./gradlew connectedAndroidTest} (see
 * docs/examples.md) is this Android platform's equivalent of the
 * {@code ctest}/{@code pytest}/{@code dotnet test} suites the other
 * bindings run.
 *
 * <p>Only {@link Dynamic}/{@link Key} are covered here -- unlike bison's
 * own instrumented test, there is no in-process {@code rmi::standalone}
 * equivalent exposed by {@code wish_client_c.h} (a wish client always
 * connects to a real, separately-running {@code wish server}), so
 * {@link Client}/{@link Proxy} are exercised manually against a live
 * server instead (see the WishExample app, docs/examples.md).
 */
@RunWith(AndroidJUnit4.class)
public class DynamicInstrumentedTest {

  @Test
  public void scalarFieldsRoundTrip() {
    try (Dynamic obj = new Dynamic("Player")) {
      obj.setInt("hp", 100);
      obj.setString("name", "hero");
      obj.setFloat("speed", 3.5f);
      obj.setBool("alive", true);

      assertEquals(100, obj.getInt("hp"));
      assertEquals("hero", obj.getString("name"));
      assertEquals(3.5f, obj.getFloat("speed"), 0.0f);
      assertTrue(obj.getBool("alive"));
    }
  }

  @Test
  public void serializationRoundTrips() {
    try (Dynamic obj = new Dynamic("Player")) {
      obj.setInt("hp", 42);
      byte[] bytes = obj.serialize();
      assertTrue(bytes.length > 0);
      try (Dynamic restored = Dynamic.deserialize(bytes)) {
        assertEquals(42, restored.getInt("hp"));
      }
      assertTrue(obj.toJson().contains("hp"));
    }
  }

  @Test
  public void vectorFieldsRoundTrip() {
    try (Dynamic obj = new Dynamic()) {
      obj.setVectorInt("tags", new int[] {1, 2, 3});
      assertArrayEquals(new int[] {1, 2, 3}, obj.getVectorInt("tags"));

      obj.setVectorBytes("payload", new byte[] {9, 8, 7});
      assertArrayEquals(new byte[] {9, 8, 7}, obj.getVectorBytes("payload"));
    }
  }

  @Test
  public void objectFieldsRoundTrip() {
    try (Dynamic outer = new Dynamic(); Dynamic inner = new Dynamic("Point")) {
      inner.setInt("x", 1);
      inner.setInt("y", 2);
      outer.setObject("origin", inner);
      try (Dynamic readBack = outer.getObject("origin")) {
        assertEquals(1, readBack.getInt("x"));
        assertEquals(2, readBack.getInt("y"));
      }
    }
  }

  @Test
  public void typeMismatchThrowsBisonException() {
    try (Dynamic obj = new Dynamic()) {
      obj.setString("name", "hero");
      try {
        obj.getInt("name");
        fail("expected BisonException");
      } catch (BisonException e) {
        assertEquals(-2 /* BISON_ERR_TYPE */, e.code);
      }
    }
  }

  @Test
  public void keyHashIsStableAndCached() {
    int a = Key.of("clicked");
    int b = Key.of("clicked");
    assertEquals(a, b);
    assertEquals(0, Key.of(""));
  }
}
