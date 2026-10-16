// MIT License © 2026 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

/**
 * An audio/video file playing on the wish server, opened by
 * {@link Client#openMedia}. The server decodes and plays it (see
 * docs/media.md); this class only sends commands. Show its video with a
 * {@code Video} widget whose {@code player} is {@link #name()}.
 *
 * <p>Every method blocks until the server answered and throws
 * {@link WishException} once the session ended. {@link #close()} it to free it
 * on the server.
 */
public final class MediaPlayer implements AutoCloseable {
  public static final int STATE_STOPPED = 0;
  public static final int STATE_PLAYING = 1;
  public static final int STATE_PAUSED = 2;
  public static final int STATE_ENDED = 3;
  public static final int STATE_ERROR = 4;

  public static final int EVENT_STATE = 0;
  public static final int EVENT_ENDED = 1;
  public static final int EVENT_ERROR = 2;

  /** A snapshot of a player ({@link #status()}). */
  public static final class Status {
    public final int state;
    public final double position;
    public final double duration;
    public final float volume;
    public final int width;
    public final int height;
    public final boolean hasVideo;
    public final boolean hasAudio;
    public final boolean loop;
    /** False when the server has no sound card (silent). */
    public final boolean audioOutput;

    Status(double[] v) {
      position = v[0];
      duration = v[1];
      volume = (float) v[2];
      state = (int) v[3];
      width = (int) v[4];
      height = (int) v[5];
      hasVideo = v[6] != 0;
      hasAudio = v[7] != 0;
      loop = v[8] != 0;
      audioOutput = v[9] != 0;
    }
  }

  private final Client client;
  private final String name;
  private long handle;
  private long listenerCtx; // native GlobalRef holder, 0 when none

  MediaPlayer(Client client, long handle, String name) {
    this.client = client;
    this.handle = handle;
    this.name = name;
  }

  /** The name widgets refer to. */
  public String name() { return name; }

  /** Starts or resumes; from the end, restarts from the beginning. */
  public void play() { check(nativePlay(require())); }

  /** Pauses at the current position. */
  public void pause() { check(nativePause(require())); }

  /** Pauses and rewinds to the start. */
  public void stop() { check(nativeStop(require())); }

  /** Moves to {@code seconds} (clamped to the file), keeping playing/paused. */
  public void seek(double seconds) { check(nativeSeek(require(), seconds)); }

  /** Sets the volume, 0 (mute) .. 1. */
  public void setVolume(float volume) { check(nativeSetVolume(require(), volume)); }

  /** Whether reaching the end restarts from the beginning. */
  public void setLoop(boolean loop) { check(nativeSetLoop(require(), loop)); }

  /** The player's state, position and stream info. */
  public Status status() {
    double[] v = new double[10];
    check(nativeStatus(require(), v));
    return new Status(v);
  }

  /** Why the player is in {@link #STATE_ERROR} ("" otherwise). */
  public String errorMessage() {
    String text = nativeErrorMessage(require());
    if (text == null) throw new WishException(-4, client.lastError());
    return text;
  }

  /** Receives every event in {@code listener} (replacing an earlier one; null stops them). */
  public synchronized void setListener(MediaListener listener) {
    long ctx = nativeSetListener(require(), listener);
    if (ctx < 0) check((int) ctx);
    if (listenerCtx != 0) nativeFreeListener(listenerCtx); // no longer referenced natively
    listenerCtx = ctx;
  }

  /** Stops and frees the player on the server; a no-op when closed. */
  @Override
  public synchronized void close() {
    if (handle == 0) return;
    nativeClose(handle); // no callback runs after this
    handle = 0;
    if (listenerCtx != 0) nativeFreeListener(listenerCtx);
    listenerCtx = 0;
  }

  private long require() {
    if (handle == 0) throw new WishException(-1, "media player closed");
    return handle;
  }

  private void check(int rc) {
    if (rc != 0) throw new WishException(rc, client.lastError());
  }

  private static native int nativePlay(long media);
  private static native int nativePause(long media);
  private static native int nativeStop(long media);
  private static native int nativeSeek(long media, double seconds);
  private static native int nativeSetVolume(long media, float volume);
  private static native int nativeSetLoop(long media, boolean loop);
  private static native int nativeStatus(long media, double[] out);
  private static native String nativeErrorMessage(long media);
  private static native long nativeSetListener(long media, MediaListener listener);
  private static native void nativeFreeListener(long ctx);
  private static native void nativeClose(long media);
}
