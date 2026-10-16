// MIT License © 2026 Binary Dice Games
//! Audio/video players playing on the wish server (see
//! [`crate::Client::open_media`] and docs/media.md).
//!
//! The server decodes the file -- which must already be in the session
//! sandbox -- and drives its own sound card; this module only sends
//! commands and links no media library.

use std::ffi::CStr;
use std::os::raw::{c_char, c_void};
use std::panic::AssertUnwindSafe;
use std::ptr;

use crate::client::{check, WishError};
use crate::sys;

/// A player's state (`WISH_MEDIA_STATE_*`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MediaState {
    Stopped = 0,
    Playing = 1,
    Paused = 2,
    Ended = 3,
    Error = 4,
}

impl MediaState {
    fn from_raw(v: i32) -> Self {
        match v {
            0 => MediaState::Stopped,
            1 => MediaState::Playing,
            2 => MediaState::Paused,
            3 => MediaState::Ended,
            _ => MediaState::Error,
        }
    }
}

/// Options for [`crate::Client::open_media`].
#[derive(Debug, Clone)]
pub struct MediaOptions {
    /// Start playing right away.
    pub autoplay: bool,
    /// Restart from the beginning at the end.
    pub loop_: bool,
    /// 0 (mute) .. 1.
    pub volume: f32,
}

impl Default for MediaOptions {
    fn default() -> Self {
        MediaOptions { autoplay: false, loop_: false, volume: 1.0 }
    }
}

/// A snapshot of a player ([`MediaPlayer::status`]).
#[derive(Debug, Clone, PartialEq)]
pub struct MediaStatus {
    pub state: MediaState,
    /// Seconds.
    pub position: f64,
    /// Seconds; 0 when the file doesn't say.
    pub duration: f64,
    pub width: i32,
    pub height: i32,
    pub has_video: bool,
    pub has_audio: bool,
    pub volume: f32,
    pub loop_: bool,
    /// False when the server has no sound card (silent).
    pub audio_output: bool,
}

/// What a [`MediaEvent`] reports (`WISH_MEDIA_EVENT_*`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MediaEventKind {
    StateChanged,
    Ended,
    Error,
}

/// One player event, passed to [`MediaPlayer::on_event`] handlers.
#[derive(Debug, Clone)]
pub struct MediaEvent {
    pub kind: MediaEventKind,
    /// The player's new state.
    pub state: MediaState,
    /// Error text for [`MediaEventKind::Error`], else empty.
    pub message: String,
}

type EventCtx = Box<dyn FnMut(&MediaEvent) + Send>;

/// A media file opened on the server. Dropping it (or [`MediaPlayer::close`])
/// stops and frees the player on the server.
pub struct MediaPlayer {
    handle: sys::wish_media_handle,
    client: sys::wish_client_handle,
    name: String,
    // Boxed twice so the C callback's userdata stays at a stable address.
    handler: Option<Box<EventCtx>>,
}

// The C ABI is thread-safe; the handle may move between threads.
unsafe impl Send for MediaPlayer {}

impl std::fmt::Debug for MediaPlayer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("MediaPlayer").field("name", &self.name).field("open", &!self.handle.is_null()).finish()
    }
}

impl MediaPlayer {
    pub(crate) fn new(handle: sys::wish_media_handle, client: sys::wish_client_handle, name: &str) -> Self {
        MediaPlayer { handle, client, name: name.to_string(), handler: None }
    }

    /// The name widgets refer to.
    pub fn name(&self) -> &str {
        &self.name
    }

    fn call(&self, rc: impl FnOnce(sys::wish_media_handle) -> sys::wish_error, what: &str) -> Result<(), WishError> {
        if self.handle.is_null() {
            return check(sys::WISH_ERR_NULL, what, ptr::null_mut());
        }
        check(rc(self.handle), what, self.client)
    }

    /// Starts or resumes; from the end, restarts from the beginning.
    pub fn play(&self) -> Result<(), WishError> {
        self.call(|h| unsafe { sys::wish_media_play(h) }, "media_player.play")
    }

    /// Pauses at the current position.
    pub fn pause(&self) -> Result<(), WishError> {
        self.call(|h| unsafe { sys::wish_media_pause(h) }, "media_player.pause")
    }

    /// Pauses and rewinds to the start.
    pub fn stop(&self) -> Result<(), WishError> {
        self.call(|h| unsafe { sys::wish_media_stop(h) }, "media_player.stop")
    }

    /// Moves to `seconds` (clamped to the file), keeping playing/paused.
    pub fn seek(&self, seconds: f64) -> Result<(), WishError> {
        self.call(|h| unsafe { sys::wish_media_seek(h, seconds) }, "media_player.seek")
    }

    /// Sets the volume, 0 (mute) .. 1.
    pub fn set_volume(&self, volume: f32) -> Result<(), WishError> {
        self.call(|h| unsafe { sys::wish_media_set_volume(h, volume) }, "media_player.set_volume")
    }

    /// Whether reaching the end restarts from the beginning.
    pub fn set_loop(&self, loop_: bool) -> Result<(), WishError> {
        self.call(|h| unsafe { sys::wish_media_set_loop(h, loop_ as i32) }, "media_player.set_loop")
    }

    /// The player's state, position and stream info.
    pub fn status(&self) -> Result<MediaStatus, WishError> {
        let mut s = sys::wish_media_status::default();
        self.call(|h| unsafe { sys::wish_media_get_status(h, &mut s) }, "media_player.status")?;
        Ok(MediaStatus {
            state: MediaState::from_raw(s.state),
            position: s.position,
            duration: s.duration,
            width: s.width,
            height: s.height,
            has_video: s.has_video != 0,
            has_audio: s.has_audio != 0,
            volume: s.volume,
            loop_: s.loop_ != 0,
            audio_output: s.audio_output != 0,
        })
    }

    /// Why the player is in [`MediaState::Error`] (empty otherwise).
    pub fn error_message(&self) -> Result<String, WishError> {
        let mut out: *mut c_char = ptr::null_mut();
        self.call(|h| unsafe { sys::wish_media_get_error(h, &mut out) }, "media_player.error_message")?;
        if out.is_null() {
            return Ok(String::new());
        }
        let text = unsafe { CStr::from_ptr(out) }.to_string_lossy().into_owned();
        unsafe { sys::bison_free_string(out) };
        Ok(text)
    }

    /// Calls `handler` for every event (replacing an earlier handler), on the
    /// client's event thread. Events raised before this call are not
    /// replayed.
    pub fn on_event<F>(&mut self, handler: F) -> Result<(), WishError>
    where
        F: FnMut(&MediaEvent) + Send + 'static,
    {
        let mut boxed: Box<EventCtx> = Box::new(Box::new(handler));
        let userdata = &mut *boxed as *mut EventCtx as *mut c_void;
        self.call(
            |h| unsafe { sys::wish_media_on_event(h, Some(media_trampoline), userdata) },
            "media_player.on_event",
        )?;
        // The old handler is no longer referenced by the C side: the call
        // above swapped the callback under the lock the trampoline runs in.
        self.handler = Some(boxed);
        Ok(())
    }

    /// Stops and frees the player on the server; a no-op when closed.
    pub fn close(&mut self) {
        if !self.handle.is_null() {
            unsafe { sys::wish_media_close(self.handle) }; // no callback runs after this
            self.handle = ptr::null_mut();
            self.handler = None;
        }
    }
}

impl Drop for MediaPlayer {
    fn drop(&mut self) {
        self.close();
    }
}

/// C-callable `wish_media_event_fn`; `userdata` is the player's `EventCtx`.
unsafe extern "C" fn media_trampoline(
    _media: sys::wish_media_handle,
    event: i32,
    state: i32,
    message: *const c_char,
    userdata: *mut c_void,
) {
    let kind = match event {
        sys::WISH_MEDIA_EVENT_ENDED => MediaEventKind::Ended,
        sys::WISH_MEDIA_EVENT_ERROR => MediaEventKind::Error,
        _ => MediaEventKind::StateChanged,
    };
    let message = if message.is_null() { String::new() } else { CStr::from_ptr(message).to_string_lossy().into_owned() };
    let ev = MediaEvent { kind, state: MediaState::from_raw(state), message };
    let ctx = &mut *(userdata as *mut EventCtx);
    // Unwinding into C++ is undefined behavior.
    let _ = std::panic::catch_unwind(AssertUnwindSafe(|| ctx(&ev)));
}
