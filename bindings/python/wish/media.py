# MIT License © 2026 Binary Dice Games
"""Audio/video players playing on the wish server (see docs/media.md).

The server decodes the file -- which must already be in the session sandbox,
e.g. uploaded with :meth:`wish.Client.upload_file` -- drives its own sound
card and shows video frames in ``Video`` widgets (``"player": name``) or
``Image`` widgets (``"src": "media://" + name``). This module only sends
commands; it needs no media library.

Example::

    client.upload_file("intro.mp4", open("intro.mp4", "rb").read())
    player = client.open_media("intro", "intro.mp4", autoplay=True)
    player.on_event(lambda event, state, message: print(event, state))
"""

import ctypes
import threading
from dataclasses import dataclass
from typing import Callable, Optional

from . import _native as _n

__all__ = ["MediaPlayer", "MediaStatus", "MEDIA_STATES", "MEDIA_EVENTS"]

#: ``wish_media_status.state`` values, by name.
MEDIA_STATES = ("stopped", "playing", "paused", "ended", "error")

#: Event names passed to :meth:`MediaPlayer.on_event` handlers.
MEDIA_EVENTS = ("state_changed", "ended", "error")


@dataclass
class MediaStatus:
    """A snapshot of a player (:meth:`MediaPlayer.status`)."""

    state: str
    position: float
    duration: float
    width: int
    height: int
    has_video: bool
    has_audio: bool
    volume: float
    loop: bool
    audio_output: bool  #: False when the server has no sound card (silent).


class MediaPlayer:
    """A media file opened on the server by :meth:`wish.Client.open_media`.

    Every method blocks until the server answered and raises
    :class:`wish.WishError` once the player was closed or the session ended.
    Use it as a context manager, or call :meth:`close`, to free it on the
    server.
    """

    def __init__(self, lib, handle, name: str, last_error: Callable[[], str]):
        self._lib = lib
        self._handle = handle
        self._name = name
        self._last_error = last_error
        self._callback = None  # keeps the ctypes callback alive
        self._lock = threading.Lock()

    @property
    def name(self) -> str:
        """The name widgets refer to."""
        return self._name

    def _call(self, fn, context: str, *args) -> None:
        from .client import _check

        if not self._handle:
            raise RuntimeError(f"{context}: media player closed")
        _check(fn(self._handle, *args), context, self._last_error)

    def play(self) -> None:
        """Start or resume; from the end, restart from the beginning."""
        self._call(self._lib.wish_media_play, "MediaPlayer.play")

    def pause(self) -> None:
        """Pause at the current position."""
        self._call(self._lib.wish_media_pause, "MediaPlayer.pause")

    def stop(self) -> None:
        """Pause and rewind to the start."""
        self._call(self._lib.wish_media_stop, "MediaPlayer.stop")

    def seek(self, seconds: float) -> None:
        """Move to ``seconds`` (clamped to the file), keeping playing/paused."""
        self._call(self._lib.wish_media_seek, "MediaPlayer.seek", float(seconds))

    def set_volume(self, volume: float) -> None:
        """Set the volume, 0 (mute) .. 1."""
        self._call(self._lib.wish_media_set_volume, "MediaPlayer.set_volume", float(volume))

    def set_loop(self, loop: bool) -> None:
        """Whether reaching the end restarts from the beginning."""
        self._call(self._lib.wish_media_set_loop, "MediaPlayer.set_loop", 1 if loop else 0)

    def status(self) -> MediaStatus:
        """The player's state, position and stream info."""
        s = _n.MediaStatus()
        self._call(self._lib.wish_media_get_status, "MediaPlayer.status", ctypes.byref(s))
        return MediaStatus(
            state=MEDIA_STATES[s.state] if 0 <= s.state < len(MEDIA_STATES) else "error",
            position=s.position,
            duration=s.duration,
            width=s.width,
            height=s.height,
            has_video=bool(s.has_video),
            has_audio=bool(s.has_audio),
            volume=s.volume,
            loop=bool(s.loop),
            audio_output=bool(s.audio_output),
        )

    def error_message(self) -> str:
        """Why the player is in the ``"error"`` state ("" otherwise)."""
        out = ctypes.c_char_p()
        self._call(self._lib.wish_media_get_error, "MediaPlayer.error_message", ctypes.byref(out))
        try:
            return out.value.decode("utf-8") if out.value else ""
        finally:
            self._lib.bison_free_string(out)

    def on_event(self, handler: Optional[Callable[[str, str, str], None]]) -> None:
        """Call ``handler(event, state, message)`` for every event (replacing
        an earlier handler; ``None`` stops them). ``event`` is one of
        :data:`MEDIA_EVENTS`, ``state`` one of :data:`MEDIA_STATES`,
        ``message`` the error text. Runs on the client's event thread;
        exceptions it raises are ignored."""
        if handler is None:
            cb = _n.MediaEventFn()
        else:

            def trampoline(_media, event, state, message, _ud):
                try:
                    handler(
                        MEDIA_EVENTS[event] if 0 <= event < len(MEDIA_EVENTS) else "state_changed",
                        MEDIA_STATES[state] if 0 <= state < len(MEDIA_STATES) else "error",
                        message.decode("utf-8") if message else "",
                    )
                except Exception:  # never let an exception cross the C ABI
                    pass

            cb = _n.MediaEventFn(trampoline)
        with self._lock:
            self._call(self._lib.wish_media_on_event, "MediaPlayer.on_event", cb, None)
            self._callback = cb

    def close(self) -> None:
        """Stop and free the player on the server; a no-op when closed."""
        with self._lock:
            if self._handle:
                self._lib.wish_media_close(self._handle)  # no callback runs after this
                self._handle = None
                self._callback = None

    def __enter__(self) -> "MediaPlayer":
        return self

    def __exit__(self, *exc) -> None:
        self.close()
