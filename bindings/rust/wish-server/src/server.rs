// MIT License © 2025 Binary Dice Games
//! Safe, RAII wrapper around `wish_server_handle` -- the Rust analogue of
//! `bindings/cpp/include/wish_cpp/server.hpp` and `bindings/python/wish/
//! server.py`, covering the full `wish_server_c.h` surface: server lifecycle
//! for all four transports, log-level control, renderer start/stop, and the
//! renderer-close / child-exit quit signal.

use std::ffi::{CStr, CString};
use std::fmt;
use std::ptr;

use crate::params::Params;
use crate::sys;

// ─── Errors ─────────────────────────────────────────────────────────────────

/// Raised when a `wish_server_*` C API call returns a non-zero error code.
#[derive(Debug, Clone)]
pub struct ServerError {
    /// The raw `wish_server_error` code (see `wish_server_c.h`).
    pub code: i32,
    message: String,
}

impl ServerError {
    fn new(code: i32, context: &str, server: sys::wish_server_handle) -> Self {
        let mut message = format!("{context}: {}", error_message(code));
        if !server.is_null() {
            if let Some(detail) = last_error_string(server) {
                if !detail.is_empty() {
                    message = format!("{message} ({detail})");
                }
            }
        }
        ServerError { code, message }
    }
}

impl fmt::Display for ServerError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.message)
    }
}

impl std::error::Error for ServerError {}

fn error_message(code: i32) -> &'static str {
    match code {
        sys::WISH_SERVER_ERR_NULL => "null handle or pointer",
        sys::WISH_SERVER_ERR_TRANSPORT => "transport listen failed",
        sys::WISH_SERVER_ERR_EXCEPTION => "internal C++ exception",
        sys::WISH_SERVER_ERR_BAD_RENDERER => {
            "unknown renderer_kind, or this library wasn't built with support for it"
        }
        _ => "unknown error",
    }
}

fn check(
    rc: sys::wish_server_error,
    context: &str,
    server: sys::wish_server_handle,
) -> Result<(), ServerError> {
    if rc == sys::WISH_SERVER_OK {
        Ok(())
    } else {
        Err(ServerError::new(rc, context, server))
    }
}

fn last_error_string(server: sys::wish_server_handle) -> Option<String> {
    let p = unsafe { sys::wish_server_last_error(server) };
    if p.is_null() {
        return None;
    }
    Some(unsafe { CStr::from_ptr(p).to_string_lossy().into_owned() })
}

fn cstr(s: &str) -> CString {
    CString::new(s).unwrap_or_else(|_| CString::new("").unwrap())
}

// ─── Server ─────────────────────────────────────────────────────────────────

/// RAII wrapper around a `wish_server_handle`.
///
/// Construct via [`Server::tcp`], [`Server::pipe`], [`Server::tls`], or
/// [`Server::term`], then [`Server::start`] to build the requested renderer
/// and begin accepting client connections. [`Server::stop`] runs on `Drop`.
///
/// This hosts the real `bdg::wish::server` implementation (the same one the
/// `wish server` CLI uses), so template registration/instantiation gives
/// each widget its own independently addressable proxy and events work --
/// unlike a server built from bison's generic RMI primitives.
pub struct Server {
    handle: sys::wish_server_handle,
    started: bool,
}

unsafe impl Send for Server {}

impl Server {
    fn from_raw(handle: sys::wish_server_handle) -> Server {
        Server {
            handle,
            started: false,
        }
    }

    /// Creates a TCP socket server (not yet listening).
    pub fn tcp(host: &str, port: u16) -> Server {
        let h = unsafe { sys::wish_server_tcp_create(cstr(host).as_ptr(), port) };
        assert!(!h.is_null(), "wish_server_tcp_create failed");
        Server::from_raw(h)
    }

    /// Creates a named-pipe / Unix-socket server (not yet listening).
    pub fn pipe(path: &str) -> Server {
        let h = unsafe { sys::wish_server_pipe_create(cstr(path).as_ptr()) };
        assert!(!h.is_null(), "wish_server_pipe_create failed");
        Server::from_raw(h)
    }

    /// Creates a TLS-secured TCP server (not yet listening). TLS material
    /// (`cert_file`/`cert_pem`, `key_file`/`key_pem`, `key_password`, and
    /// optionally `client_auth`/`ca_file`/`ca_pem` for mutual TLS) is
    /// supplied via [`Server::start`]'s `params`.
    pub fn tls(host: &str, port: u16) -> Server {
        let h = unsafe { sys::wish_server_tls_create(cstr(host).as_ptr(), port) };
        assert!(!h.is_null(), "wish_server_tls_create failed");
        Server::from_raw(h)
    }

    /// Creates a terminal (OSC-99 framed) server by spawning a child process
    /// attached to a new pseudo-terminal. The child is expected to be a wish
    /// client using `wish::Client::term()` over its own inherited stdio.
    /// [`Server::should_quit`] also returns `true` once that child exits.
    ///
    /// `cmd` empty spawns the operator's `$SHELL` (Linux/MSYS2) or
    /// `cmd.exe` (Windows).
    pub fn term(cmd: &str) -> Server {
        let c = cstr(cmd);
        let ptr = if cmd.is_empty() {
            ptr::null()
        } else {
            c.as_ptr()
        };
        let h = unsafe { sys::wish_server_term_create(ptr) };
        assert!(!h.is_null(), "wish_server_term_create failed");
        Server::from_raw(h)
    }

    /// Last error message recorded for this server (empty if none).
    pub fn last_error(&self) -> String {
        last_error_string(self.handle).unwrap_or_default()
    }

    /// Deprecated: prefer [`Server::set_log_level`]. `true` maps to log level
    /// `"trace"`, `false` to `"none"`. Must be called before
    /// [`Server::start`].
    pub fn set_verbose(&self, verbose: bool) -> Result<(), ServerError> {
        check(
            unsafe { sys::wish_server_set_verbose(self.handle, verbose as i32) },
            "server.set_verbose",
            self.handle,
        )
    }

    /// Sets the server log verbosity: one of `"none"`, `"fatal"`, `"error"`,
    /// `"warning"`, `"info"`, `"trace"` (default `"none"`). RMI trace lines
    /// appear at `"info"` and above, decoded payloads at `"trace"`. Must be
    /// called before [`Server::start`].
    pub fn set_log_level(&self, level: &str) -> Result<(), ServerError> {
        check(
            unsafe { sys::wish_server_set_log_level(self.handle, cstr(level).as_ptr()) },
            "server.set_log_level",
            self.handle,
        )
    }

    /// Builds `renderer` and begins accepting client connections.
    ///
    /// `renderer` is one of:
    /// - `"sdl3"` -- a real SDL3 window (needs `WISH_ENABLE_SDL3=ON`).
    /// - `"web"` -- the browser renderer on its own embedded HTTP+WebSocket
    ///   listener (needs `WISH_ENABLE_WEB=ON`); pass `web_bind`/`web_port`
    ///   and open the printed URL.
    /// - `"console"` -- a lightweight text dump of the widget tree to
    ///   stdout; no display needed, meant for tests/CI.
    ///
    /// `params` (optional) carries renderer-specific fields, all optional:
    /// `title`, `width`, `height`, `font_size` for `"sdl3"`/`"web"`;
    /// `web_bind`, `web_port` for `"web"`. It is also forwarded unchanged as
    /// transport listen params -- e.g. `cert_file`/`key_file` for a
    /// [`Server::tls`] server; ignored by every other transport.
    pub fn start(&mut self, renderer: &str, params: Option<&Params>) -> Result<(), ServerError> {
        let params_h = params.map_or(ptr::null_mut(), Params::raw_handle);
        check(
            unsafe { sys::wish_server_start(self.handle, cstr(renderer).as_ptr(), params_h) },
            &format!("server.start({renderer:?})"),
            self.handle,
        )?;
        self.started = true;
        Ok(())
    }

    /// Stops the accept loop, render loop, and joins all threads. A no-op if
    /// [`Server::start`] was never called (or already stopped).
    pub fn stop(&mut self) -> Result<(), ServerError> {
        if !self.started {
            return Ok(());
        }
        check(
            unsafe { sys::wish_server_stop(self.handle) },
            "server.stop",
            self.handle,
        )?;
        self.started = false;
        Ok(())
    }

    /// Returns `true` once the renderer signals it should close (e.g. the
    /// SDL3 window was closed), or -- for a [`Server::term`] server -- once
    /// the spawned child process has exited. The web and console renderers
    /// never set this on their own; stop those with an explicit
    /// [`Server::stop`].
    pub fn should_quit(&self) -> bool {
        unsafe { sys::wish_server_should_quit(self.handle) != 0 }
    }

    pub fn raw_handle(&self) -> sys::wish_server_handle {
        self.handle
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            // wish_server_destroy() stops the server first if still running.
            unsafe { sys::wish_server_destroy(self.handle) };
            self.handle = ptr::null_mut();
        }
    }
}
