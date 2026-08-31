// MIT License © 2025 Binary Dice Games
//! Hand-maintained `extern "C"` mirror of `wish_server_c.h`, plus the small
//! subset of `bison_c.h` needed to build `wish_server_start()`'s `params`
//! argument.
//!
//! This is the server counterpart of the client crate's `wish::sys` -- and,
//! like `bindings/python/wish/_server_native.py` and C#'s `ServerNative`, it
//! deliberately keeps its own `bison_*` declarations rather than reaching
//! into the client binding's: `libwish_server` embeds its own copy of the
//! bison C ABI (see `src/wish_server_c.cpp`), and a `bison_handle` is only
//! valid against the exact library that created it. [`crate::params`] builds
//! the params handle through *these* declarations so it always matches the
//! `libwish_server` that `wish_server_start()` decodes it with.
//!
//! No `bindgen` codegen is used, matching every other wish/bison binding.
//!
//! This is a private, unsafe layer; public consumers use [`crate::Server`]
//! and [`crate::Params`] instead.

#![allow(non_camel_case_types)]

use std::os::raw::{c_char, c_int, c_void};

// ─── Shared C type aliases ──────────────────────────────────────────────────

pub type bison_handle = *mut c_void;
pub type bison_hash = u32;
pub type bison_error = c_int;

pub type wish_server_handle = *mut c_void;
pub type wish_server_error = c_int;

// ─── wish_server_error codes ────────────────────────────────────────────────

pub const WISH_SERVER_OK: wish_server_error = 0;
pub const WISH_SERVER_ERR_NULL: wish_server_error = -1;
pub const WISH_SERVER_ERR_TRANSPORT: wish_server_error = -3;
pub const WISH_SERVER_ERR_EXCEPTION: wish_server_error = -4;
pub const WISH_SERVER_ERR_BAD_RENDERER: wish_server_error = -6;

// ─── bison_error codes ──────────────────────────────────────────────────────

pub const BISON_OK: bison_error = 0;

extern "C" {
    // ── wish_server_c.h: lifecycle ─────────────────────────────────────────

    pub fn wish_server_tcp_create(host: *const c_char, port: u16) -> wish_server_handle;
    pub fn wish_server_pipe_create(path: *const c_char) -> wish_server_handle;
    pub fn wish_server_tls_create(host: *const c_char, port: u16) -> wish_server_handle;
    pub fn wish_server_term_create(cmd: *const c_char) -> wish_server_handle;

    pub fn wish_server_start(
        server: wish_server_handle,
        renderer_kind: *const c_char,
        params: bison_handle,
    ) -> wish_server_error;
    pub fn wish_server_stop(server: wish_server_handle) -> wish_server_error;
    pub fn wish_server_should_quit(server: wish_server_handle) -> c_int;
    pub fn wish_server_set_verbose(server: wish_server_handle, verbose: c_int)
        -> wish_server_error;
    pub fn wish_server_set_log_level(
        server: wish_server_handle,
        level: *const c_char,
    ) -> wish_server_error;
    pub fn wish_server_destroy(server: wish_server_handle);
    pub fn wish_server_last_error(server: wish_server_handle) -> *const c_char;

    // ── bison_c.h: the subset needed to build `params` ─────────────────────
    // (Embedded in `libwish_server` -- see this module's doc comment for why
    // these are declared here and not shared with the client crate.)

    pub fn bison_key(name: *const c_char) -> bison_hash;
    pub fn bison_create(klass_name: bison_hash) -> bison_handle;
    pub fn bison_release(h: bison_handle);
    pub fn bison_set_int(h: bison_handle, name: bison_hash, value: i32) -> bison_error;
    pub fn bison_set_float(h: bison_handle, name: bison_hash, value: f32) -> bison_error;
    pub fn bison_set_bool(h: bison_handle, name: bison_hash, value: c_int) -> bison_error;
    pub fn bison_set_string(h: bison_handle, name: bison_hash, value: *const c_char)
        -> bison_error;
}
