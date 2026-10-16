// MIT License © 2026 Binary Dice Games
/**
 * @file MediaPlayer.cs
 * @brief An audio/video player playing on the wish server (see
 *        `Client.OpenMedia` and docs/media.md). The server decodes and plays
 *        the file; this class only sends commands.
 */

using System.Runtime.InteropServices;

namespace Bdg.Wish;

/// <summary>A player's state (<c>WISH_MEDIA_STATE_*</c>).</summary>
public enum MediaState
{
    Stopped = 0,
    Playing = 1,
    Paused = 2,
    Ended = 3,
    Error = 4,
}

/// <summary>What a <see cref="MediaPlayer.Event"/> reports (<c>WISH_MEDIA_EVENT_*</c>).</summary>
public enum MediaEventKind
{
    StateChanged = 0,
    Ended = 1,
    Error = 2,
}

/// <summary>A snapshot of a player (<see cref="MediaPlayer.Status"/>).</summary>
public sealed record MediaStatus(
    MediaState State,
    double Position,
    double Duration,
    int Width,
    int Height,
    bool HasVideo,
    bool HasAudio,
    float Volume,
    bool Loop,
    bool AudioOutput);

/// <summary>
/// A media file opened on the server by <see cref="Client.OpenMedia"/>. Every method blocks until
/// the server answered and throws <see cref="WishException"/> once the session ended. Dispose it
/// (or call <see cref="Close"/>) to free it on the server.
/// </summary>
public sealed class MediaPlayer : IDisposable
{
    private nint _handle;
    private readonly Func<string> _lastError;
    private readonly object _lock = new();
    private NativeMediaEventFn? _native; // kept alive while registered

    internal MediaPlayer(nint handle, string name, Func<string> lastError)
    {
        _handle = handle;
        Name = name;
        _lastError = lastError;
    }

    /// <summary>The name widgets refer to.</summary>
    public string Name { get; }

    /// <summary>
    /// Raised for every event, on the client's event thread: (kind, new state, error message).
    /// </summary>
    public event Action<MediaEventKind, MediaState, string>? Event
    {
        add
        {
            lock (_lock)
            {
                _handlers += value;
                EnsureNativeCallback();
            }
        }
        remove
        {
            lock (_lock)
            {
                _handlers -= value;
            }
        }
    }

    private Action<MediaEventKind, MediaState, string>? _handlers;

    /// <summary>Starts or resumes; from the end, restarts from the beginning.</summary>
    public void Play() => Check(Native.wish_media_play(Handle), "play");

    /// <summary>Pauses at the current position.</summary>
    public void Pause() => Check(Native.wish_media_pause(Handle), "pause");

    /// <summary>Pauses and rewinds to the start.</summary>
    public void Stop() => Check(Native.wish_media_stop(Handle), "stop");

    /// <summary>Moves to <paramref name="seconds"/> (clamped to the file), keeping playing/paused.</summary>
    public void Seek(double seconds) => Check(Native.wish_media_seek(Handle, seconds), "seek");

    /// <summary>Sets the volume, 0 (mute) .. 1.</summary>
    public void SetVolume(float volume) => Check(Native.wish_media_set_volume(Handle, volume), "set_volume");

    /// <summary>Whether reaching the end restarts from the beginning.</summary>
    public void SetLoop(bool loop) => Check(Native.wish_media_set_loop(Handle, loop ? 1 : 0), "set_loop");

    /// <summary>The player's state, position and stream info.</summary>
    public MediaStatus Status()
    {
        Check(Native.wish_media_get_status(Handle, out NativeMediaStatus s), "status");
        return new MediaStatus(
            (MediaState)s.State, s.Position, s.Duration, s.Width, s.Height,
            s.HasVideo != 0, s.HasAudio != 0, s.Volume, s.Loop != 0, s.AudioOutput != 0);
    }

    /// <summary>Why the player is in <see cref="MediaState.Error"/> ("" otherwise).</summary>
    public string ErrorMessage()
    {
        Check(Native.wish_media_get_error(Handle, out nint text), "error_message");
        try
        {
            return text == nint.Zero ? "" : Marshal.PtrToStringUTF8(text) ?? "";
        }
        finally
        {
            WishInterop.FreeString(text);
        }
    }

    /// <summary>Stops and frees the player on the server; a no-op when closed.</summary>
    public void Close()
    {
        nint handle;
        NativeMediaEventFn? native;
        lock (_lock)
        {
            handle = _handle;
            native = _native;
            _handle = nint.Zero;
            _handlers = null;
        }
        if (handle == nint.Zero)
        {
            return;
        }
        // Outside the lock: this waits for a running callback, which takes it.
        Native.wish_media_close(handle); // no callback runs after this
        GC.KeepAlive(native);
        lock (_lock)
        {
            _native = null;
        }
    }

    /// <inheritdoc cref="Close"/>
    public void Dispose() => Close();

    private nint Handle =>
        _handle != nint.Zero ? _handle : throw new WishException(WishErrorCode.Null, $"media_player({Name})", "closed");

    private void Check(int rc, string what) => WishException.Check(rc, $"media_player.{what}", _lastError);

    private void EnsureNativeCallback()
    {
        if (_native != null)
        {
            return;
        }
        _native = (_, ev, state, message, _) =>
        {
            try
            {
                Action<MediaEventKind, MediaState, string>? handlers;
                lock (_lock)
                {
                    handlers = _handlers;
                }
                handlers?.Invoke((MediaEventKind)ev, (MediaState)state, Marshal.PtrToStringUTF8(message) ?? "");
            }
            catch
            {
                // C ABI boundary: cannot propagate.
            }
        };
        Check(Native.wish_media_on_event(Handle, Marshal.GetFunctionPointerForDelegate(_native), nint.Zero), "on_event");
    }
}
