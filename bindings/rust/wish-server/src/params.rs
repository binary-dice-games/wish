// MIT License © 2025 Binary Dice Games
//! [`Params`] -- a small flat string/int/bool/float map that lowers to the
//! `bison_handle` [`crate::Server::start`] forwards to `wish_server_start()`
//! as its renderer/listen params.
//!
//! This is the server crate's stand-in for the client crate's richer
//! `wish::Value`: `wish_server_start()`'s params are always a flat map of
//! scalars (`title`, `width`, `web_port`, TLS `cert_file`/`key_file`, ...),
//! so a full `bison::dynamic` wrapper would be overkill -- and, more
//! importantly, would have to be built against *this* library's embedded
//! `bison_*` functions anyway (see `sys.rs`'s doc comment). Mirrors
//! `bindings/python/wish/_server_native.py`'s `build_params()` and C#'s
//! `ServerParamsScope`.

use std::ffi::CString;
use std::os::raw::c_char;
use std::ptr;

use crate::sys;

/// A builder for [`crate::Server::start`]'s `params` argument.
///
/// ```no_run
/// use wish_server::{Params, Server};
///
/// let mut server = Server::tcp("127.0.0.1", 7070);
/// server
///     .start("sdl3", Some(&Params::new().string("title", "My App").int("width", 1024)))
///     .unwrap();
/// ```
pub struct Params {
    handle: sys::bison_handle,
}

// SAFETY: a `Params` is only ever accessed by one thread at a time under
// ordinary Rust ownership rules, which is all the underlying library needs.
unsafe impl Send for Params {}

impl Params {
    /// A new, empty parameter map.
    pub fn new() -> Params {
        let h = unsafe { sys::bison_create(0) };
        assert!(!h.is_null(), "bison_create failed");
        Params { handle: h }
    }

    fn key(name: &str) -> sys::bison_hash {
        let c = CString::new(name).expect("params names must not contain NUL bytes");
        unsafe { sys::bison_key(c.as_ptr()) }
    }

    /// Sets a string field, consuming and returning `self` for chaining.
    pub fn string(mut self, name: &str, value: &str) -> Params {
        self.set_string(name, value);
        self
    }

    /// Sets an integer field, consuming and returning `self` for chaining.
    pub fn int(mut self, name: &str, value: i32) -> Params {
        self.set_int(name, value);
        self
    }

    /// Sets a float field, consuming and returning `self` for chaining.
    pub fn float(mut self, name: &str, value: f32) -> Params {
        self.set_float(name, value);
        self
    }

    /// Sets a boolean field, consuming and returning `self` for chaining.
    pub fn bool(mut self, name: &str, value: bool) -> Params {
        self.set_bool(name, value);
        self
    }

    /// Sets a string field in place.
    pub fn set_string(&mut self, name: &str, value: &str) {
        let c = CString::new(value).expect("params string values must not contain NUL bytes");
        unsafe { sys::bison_set_string(self.handle, Self::key(name), c.as_ptr() as *const c_char) };
    }

    /// Sets an integer field in place.
    pub fn set_int(&mut self, name: &str, value: i32) {
        unsafe { sys::bison_set_int(self.handle, Self::key(name), value) };
    }

    /// Sets a float field in place.
    pub fn set_float(&mut self, name: &str, value: f32) {
        unsafe { sys::bison_set_float(self.handle, Self::key(name), value) };
    }

    /// Sets a boolean field in place.
    pub fn set_bool(&mut self, name: &str, value: bool) {
        unsafe { sys::bison_set_bool(self.handle, Self::key(name), value as i32) };
    }

    /// The raw `bison_handle`, borrowed for the duration of a
    /// `wish_server_start()` call. Not exposed publicly -- callers pass a
    /// `&Params` and let [`crate::Server::start`] unwrap it.
    pub(crate) fn raw_handle(&self) -> sys::bison_handle {
        self.handle
    }
}

impl Default for Params {
    fn default() -> Self {
        Params::new()
    }
}

impl Drop for Params {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { sys::bison_release(self.handle) };
            self.handle = ptr::null_mut();
        }
    }
}
