// MIT License © 2025 Binary Dice Games
//! Safe, RAII wrapper around `wish_client_handle` -- the Rust analogue of
//! `bindings/cpp/include/wish_cpp/client.hpp`, covering the full
//! `wish_client_c.h` surface: client lifecycle for all 5 transports, style
//! presets, template register/instantiate/proxy_get/release, direct object
//! instantiation, embedded-app list/run, file transfer, logging, and native
//! automation.

use std::ffi::{CStr, CString};
use std::fmt;
use std::os::raw::{c_char, c_void};
use std::panic::AssertUnwindSafe;
use std::ptr;

use crate::key::{key, key_or_zero};
use crate::proxy::{Future, Proxy, RmiError};
use crate::sys;
use crate::value::{BisonError, Value};

// ─── Errors ─────────────────────────────────────────────────────────────────

/// Raised when a `wish_*` C API call returns a non-zero error code.
#[derive(Debug, Clone)]
pub struct WishError {
    /// The raw `wish_error` code (see `wish_client_c.h`).
    pub code: i32,
    message: String,
}

impl WishError {
    fn new(code: i32, context: &str, client: sys::wish_client_handle) -> Self {
        let mut message = format!("{context}: {}", error_message(code));
        if !client.is_null() {
            if let Some(detail) = last_error_string(client) {
                if !detail.is_empty() {
                    message = format!("{message} ({detail})");
                }
            }
        }
        WishError { code, message }
    }
}

impl fmt::Display for WishError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.message)
    }
}

impl std::error::Error for WishError {}

fn error_message(code: i32) -> &'static str {
    match code {
        sys::WISH_ERR_NULL => "null handle or pointer",
        sys::WISH_ERR_NOT_FOUND => "named proxy or resource not found",
        sys::WISH_ERR_TRANSPORT => "transport connection failed",
        sys::WISH_ERR_EXCEPTION => "internal C++ exception",
        sys::WISH_ERR_AMBIGUOUS => {
            "app name matches more than one registered app; use the fully-qualified name"
        }
        _ => "unknown error",
    }
}

fn check(
    rc: sys::wish_error,
    context: &str,
    client: sys::wish_client_handle,
) -> Result<(), WishError> {
    if rc == sys::WISH_OK {
        Ok(())
    } else {
        Err(WishError::new(rc, context, client))
    }
}

fn last_error_string(client: sys::wish_client_handle) -> Option<String> {
    let p = unsafe { sys::wish_last_error(client) };
    if p.is_null() {
        return None;
    }
    Some(unsafe { CStr::from_ptr(p).to_string_lossy().into_owned() })
}

fn cstr(s: &str) -> CString {
    CString::new(s).unwrap_or_else(|_| CString::new("").unwrap())
}

/// Errors that can be returned from the session callback passed to
/// [`Client::run`]. Wraps whichever of the ABI's three error surfaces
/// (`wish_*`, `rmi_*`, `bison_*`) the failing call raised.
#[derive(Debug)]
pub enum Error {
    Wish(WishError),
    Rmi(RmiError),
    Bison(BisonError),
    /// The session callback panicked; carries the panic payload formatted
    /// as a string (the payload itself is not `Send + Sync + 'static` in
    /// general, so it cannot be stored directly).
    SessionPanic(String),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Wish(e) => write!(f, "{e}"),
            Error::Rmi(e) => write!(f, "{e}"),
            Error::Bison(e) => write!(f, "{e}"),
            Error::SessionPanic(msg) => write!(f, "session callback panicked: {msg}"),
        }
    }
}

impl std::error::Error for Error {}

impl From<WishError> for Error {
    fn from(e: WishError) -> Self {
        Error::Wish(e)
    }
}
impl From<RmiError> for Error {
    fn from(e: RmiError) -> Self {
        Error::Rmi(e)
    }
}
impl From<BisonError> for Error {
    fn from(e: BisonError) -> Self {
        Error::Bison(e)
    }
}

/// Lists every embedded app registered by an enabled optional module (see
/// `modules/README.md`), as a raw JSON array string --
/// `[{"name","organization","collection","description","params":[...]}, ...]`.
///
/// Mirrors `wish client --list`. Does not require a connection -- app
/// registration happens at library-load time.
pub fn list_apps_json() -> Result<String, WishError> {
    let mut out: *mut c_char = ptr::null_mut();
    check(
        unsafe { sys::wish_list_apps(&mut out) },
        "list_apps_json",
        ptr::null_mut(),
    )?;
    let s = unsafe { CStr::from_ptr(out).to_string_lossy().into_owned() };
    unsafe { sys::bison_free_string(out) };
    Ok(s)
}

// ─── Client ─────────────────────────────────────────────────────────────────

/// RAII wrapper around a `wish_client_handle`.
///
/// Construct via [`Client::tcp`], [`Client::tls`], [`Client::stream`],
/// [`Client::pipe`], or [`Client::term`], then call [`Client::run`] to
/// connect, drive the session, and disconnect.
pub struct Client {
    handle: sys::wish_client_handle,
}

unsafe impl Send for Client {}

/// A cheap, `Copy`/`Send`/`Sync` token that can call [`Client::quit`] from
/// inside a `'static` event handler ([`crate::Proxy::on_event`] requires its
/// closure to be `'static`, so it cannot borrow a `&Client` directly).
/// Obtained via [`Client::quit_handle`]; safe to call `quit()` on for as
/// long as the originating `Client::run`/`run_with_params` call is still on
/// the stack, mirroring `wish_client_quit`'s own "safe from any thread"
/// contract.
#[derive(Copy, Clone)]
pub struct QuitHandle(sys::wish_client_handle);

unsafe impl Send for QuitHandle {}
unsafe impl Sync for QuitHandle {}

impl QuitHandle {
    /// Signals the session to end; unblocks a concurrent [`Client::wait`].
    pub fn quit(&self) {
        unsafe { sys::wish_client_quit(self.0) };
    }
}

impl Client {
    fn from_raw(handle: sys::wish_client_handle) -> Self {
        Client { handle }
    }

    pub fn tcp(host: &str, port: u16) -> Client {
        let h = unsafe { sys::wish_client_tcp_create(cstr(host).as_ptr(), port) };
        assert!(!h.is_null(), "wish_client_tcp_create failed");
        Client::from_raw(h)
    }

    /// Creates a TLS-secured TCP client (not yet connected). TLS
    /// trust/identity material is supplied via [`Client::run`]'s
    /// `connect_params` (`ca_file`/`ca_pem`, `insecure_skip_verify`,
    /// `cert_file`/`cert_pem`, `key_file`/`key_pem`, `key_password`,
    /// `server_name`).
    pub fn tls(host: &str, port: u16) -> Client {
        let h = unsafe { sys::wish_client_tls_create(cstr(host).as_ptr(), port) };
        assert!(!h.is_null(), "wish_client_tls_create failed");
        Client::from_raw(h)
    }

    /// Creates a `std::iostream`-backed stream (FIFO / named pipe) client.
    /// Linux only.
    pub fn stream(path: &str) -> Client {
        let h = unsafe { sys::wish_client_stream_create(cstr(path).as_ptr()) };
        assert!(!h.is_null(), "wish_client_stream_create failed");
        Client::from_raw(h)
    }

    /// Creates a named-pipe / Unix-socket client.
    pub fn pipe(path: &str) -> Client {
        let h = unsafe { sys::wish_client_pipe_create(cstr(path).as_ptr()) };
        assert!(!h.is_null(), "wish_client_pipe_create failed");
        Client::from_raw(h)
    }

    /// Creates a terminal (OSC-99 framed) client wrapping the calling
    /// process's own inherited stdio.
    pub fn term() -> Client {
        let h = unsafe { sys::wish_client_term_create() };
        assert!(!h.is_null(), "wish_client_term_create failed");
        Client::from_raw(h)
    }

    /// Last error message recorded for this client (empty if none).
    pub fn last_error(&self) -> String {
        last_error_string(self.handle).unwrap_or_default()
    }

    // ── Session lifecycle ────────────────────────────────────────────────

    /// Connects, invokes `session_fn(self)`, then disconnects.
    ///
    /// Blocks until `session_fn` returns; it runs on the RMI worker thread,
    /// so call [`Client::wait`] inside it to keep the session alive while
    /// event handlers update the UI, ending it with [`Client::quit`].
    pub fn run<F>(&self, session_fn: F) -> Result<(), Error>
    where
        F: FnOnce(&Client),
    {
        self.run_with_params(session_fn, None)
    }

    /// Identical to [`Client::run`], except `connect_params` is forwarded
    /// to both the transport's connection setup and the server's connect
    /// handshake payload (e.g. fields a server-side auth module inspects).
    pub fn run_with_params<F>(
        &self,
        session_fn: F,
        connect_params: Option<&Value>,
    ) -> Result<(), Error>
    where
        F: FnOnce(&Client),
    {
        let mut ctx: SessionCtx<F> = SessionCtx {
            fn_: Some(session_fn),
            panic: None,
        };
        let user = &mut ctx as *mut SessionCtx<F> as *mut c_void;
        let params_h = connect_params.map_or(ptr::null_mut(), Value::raw_handle);
        let rc = unsafe {
            sys::wish_client_run_with_params(self.handle, session_trampoline::<F>, user, params_h)
        };
        if let Some(panic_msg) = ctx.panic {
            return Err(Error::SessionPanic(panic_msg));
        }
        check(rc, "client.run", self.handle)?;
        Ok(())
    }

    /// Blocks until [`Client::quit`] is called (from any thread).
    pub fn wait(&self) {
        unsafe { sys::wish_client_wait(self.handle) };
    }

    /// Signals the session to end; unblocks a concurrent [`Client::wait`].
    /// Safe to call from any thread, including an event handler.
    pub fn quit(&self) {
        unsafe { sys::wish_client_quit(self.handle) };
    }

    /// Returns a cheap, `'static`-safe token whose [`QuitHandle::quit`]
    /// calls back into this client -- use this from inside a
    /// [`Proxy::on_event`] closure, which cannot borrow `&Client` directly
    /// since its callback must be `'static`.
    pub fn quit_handle(&self) -> QuitHandle {
        QuitHandle(self.handle)
    }

    // ── Style ────────────────────────────────────────────────────────────

    /// Applies a built-in style preset: `"wish"`, `"dark"`, `"light"`, or
    /// `"classic"`.
    pub fn set_style_preset(&self, preset: &str) -> Result<(), WishError> {
        check(
            unsafe { sys::wish_set_style_preset(self.handle, cstr(preset).as_ptr()) },
            "client.set_style_preset",
            self.handle,
        )
    }

    pub fn set_style_preset_async(&self, preset: &str) -> Result<Future, WishError> {
        let mut f: sys::rmi_future_handle = ptr::null_mut();
        check(
            unsafe { sys::wish_set_style_preset_async(self.handle, cstr(preset).as_ptr(), &mut f) },
            "client.set_style_preset_async",
            self.handle,
        )?;
        Ok(Future::from_raw(f))
    }

    // ── Template management ──────────────────────────────────────────────

    /// Registers a named UI template (JSON or YAML descriptor text).
    pub fn register_template(&self, name: &str, descriptor: &str) -> Result<(), WishError> {
        check(
            unsafe {
                sys::wish_register_template(
                    self.handle,
                    cstr(name).as_ptr(),
                    cstr(descriptor).as_ptr(),
                )
            },
            &format!("client.register_template({name:?})"),
            self.handle,
        )
    }

    pub fn register_template_async(
        &self,
        name: &str,
        descriptor: &str,
    ) -> Result<Future, WishError> {
        let mut f: sys::rmi_future_handle = ptr::null_mut();
        check(
            unsafe {
                sys::wish_register_template_async(
                    self.handle,
                    cstr(name).as_ptr(),
                    cstr(descriptor).as_ptr(),
                    &mut f,
                )
            },
            &format!("client.register_template_async({name:?})"),
            self.handle,
        )?;
        Ok(Future::from_raw(f))
    }

    /// Instantiates a registered template under dot-path `prefix` and
    /// returns a proxy to its root; descendants are reachable via
    /// [`Client::proxy_get`]`("prefix.child.path")`.
    pub fn instantiate_template(&self, name: &str, prefix: &str) -> Result<Proxy, WishError> {
        let h = unsafe {
            sys::wish_instantiate_template(self.handle, cstr(name).as_ptr(), cstr(prefix).as_ptr())
        };
        if h.is_null() {
            return Err(WishError::new(
                sys::WISH_ERR_EXCEPTION,
                &format!("client.instantiate_template({name:?}, {prefix:?})"),
                self.handle,
            ));
        }
        Ok(Proxy::from_raw(h))
    }

    pub fn instantiate_template_async(
        &self,
        name: &str,
        prefix: &str,
    ) -> Result<Future, WishError> {
        let mut f: sys::rmi_future_handle = ptr::null_mut();
        check(
            unsafe {
                sys::wish_instantiate_template_async(
                    self.handle,
                    cstr(name).as_ptr(),
                    cstr(prefix).as_ptr(),
                    &mut f,
                )
            },
            &format!("client.instantiate_template_async({name:?}, {prefix:?})"),
            self.handle,
        )?;
        Ok(Future::from_raw(f))
    }

    /// Resolves a dot-joined element path (see [`Client::instantiate_template`])
    /// from the client's local proxy map -- no round trip to the server.
    pub fn proxy_get(&self, dot_path: &str) -> Result<Proxy, WishError> {
        let h = unsafe { sys::wish_proxy_get(self.handle, cstr(dot_path).as_ptr()) };
        if h.is_null() {
            return Err(WishError::new(
                sys::WISH_ERR_NOT_FOUND,
                &format!("client.proxy_get({dot_path:?})"),
                self.handle,
            ));
        }
        Ok(Proxy::from_raw(h))
    }

    /// Releases every proxy cached under `prefix` and its descendants.
    pub fn release(&self, prefix: &str) -> Result<(), WishError> {
        check(
            unsafe { sys::wish_release(self.handle, cstr(prefix).as_ptr()) },
            &format!("client.release({prefix:?})"),
            self.handle,
        )
    }

    // ── Object instantiation ─────────────────────────────────────────────

    /// Instantiates a remote object directly (no UI template involved).
    /// Unlike [`Client::instantiate_template`], the result is not merged
    /// into the dot-path proxy map; the caller keeps and drops the returned
    /// proxy directly. `ns` is `""` for the global namespace.
    pub fn instantiate(
        &self,
        klass: &str,
        ns: &str,
        params: Option<&Value>,
    ) -> Result<Proxy, WishError> {
        let params_h = params.map_or(ptr::null_mut(), Value::raw_handle);
        let h =
            unsafe { sys::wish_instantiate(self.handle, key_or_zero(ns), key(klass), params_h) };
        if h.is_null() {
            return Err(WishError::new(
                sys::WISH_ERR_EXCEPTION,
                &format!("client.instantiate({klass:?})"),
                self.handle,
            ));
        }
        Ok(Proxy::from_raw(h))
    }

    // ── Embedded apps ────────────────────────────────────────────────────

    /// Connects, runs the named embedded app (see [`list_apps_json`]),
    /// blocks until it signals completion, then disconnects. `name` may be
    /// a short name (e.g. `"bc"`) or its fully-qualified
    /// `"organization/collection/name"` form.
    pub fn run_app(&self, name: &str, args: &[&str]) -> Result<(), WishError> {
        let c_args: Vec<CString> = args.iter().map(|a| cstr(a)).collect();
        let argv: Vec<*const c_char> = c_args.iter().map(|c| c.as_ptr()).collect();
        let argv_ptr = if argv.is_empty() {
            ptr::null()
        } else {
            argv.as_ptr()
        };
        check(
            unsafe { sys::wish_run_app(self.handle, cstr(name).as_ptr(), argv_ptr, argv.len()) },
            &format!("client.run_app({name:?})"),
            self.handle,
        )
    }

    // ── File transfer ────────────────────────────────────────────────────

    /// Uploads a file to the server's sandboxed session resource directory.
    pub fn upload_file(&self, name: &str, data: &[u8]) -> Result<(), WishError> {
        let ptr_data = if data.is_empty() {
            ptr::null()
        } else {
            data.as_ptr() as *const c_char
        };
        check(
            unsafe {
                sys::wish_upload_file(self.handle, cstr(name).as_ptr(), ptr_data, data.len())
            },
            &format!("client.upload_file({name:?})"),
            self.handle,
        )
    }

    /// Downloads a previously uploaded file from the server.
    pub fn download_file(&self, name: &str) -> Result<Vec<u8>, WishError> {
        let mut out: *mut c_char = ptr::null_mut();
        let mut len: usize = 0;
        check(
            unsafe {
                sys::wish_download_file(self.handle, cstr(name).as_ptr(), &mut out, &mut len)
            },
            &format!("client.download_file({name:?})"),
            self.handle,
        )?;
        let data = unsafe { std::slice::from_raw_parts(out as *const u8, len).to_vec() };
        unsafe { sys::bison_free_string(out) };
        Ok(data)
    }

    /// Uploads a file, streaming it in chunks from a local file on disk.
    pub fn upload_file_from_path(&self, name: &str, local_path: &str) -> Result<(), WishError> {
        check(
            unsafe {
                sys::wish_upload_file_from_path(
                    self.handle,
                    cstr(name).as_ptr(),
                    cstr(local_path).as_ptr(),
                )
            },
            &format!("client.upload_file_from_path({name:?})"),
            self.handle,
        )
    }

    /// Downloads a file, streaming it directly to a local file on disk.
    pub fn download_file_to_path(&self, name: &str, local_path: &str) -> Result<(), WishError> {
        check(
            unsafe {
                sys::wish_download_file_to_path(
                    self.handle,
                    cstr(name).as_ptr(),
                    cstr(local_path).as_ptr(),
                )
            },
            &format!("client.download_file_to_path({name:?})"),
            self.handle,
        )
    }

    /// Uploads a local zip archive and has the server unpack it into a
    /// sandboxed destination directory.
    pub fn upload_package(&self, dest_path: &str, local_zip_path: &str) -> Result<(), WishError> {
        check(
            unsafe {
                sys::wish_upload_package_from_path(
                    self.handle,
                    cstr(dest_path).as_ptr(),
                    cstr(local_zip_path).as_ptr(),
                )
            },
            &format!("client.upload_package({dest_path:?})"),
            self.handle,
        )
    }

    // ── Logging ──────────────────────────────────────────────────────────

    pub fn log(&self, level: &str, msg: &str) -> Result<(), WishError> {
        check(
            unsafe { sys::wish_log(self.handle, cstr(level).as_ptr(), cstr(msg).as_ptr()) },
            "client.log",
            self.handle,
        )
    }
    pub fn log_debug(&self, msg: &str) -> Result<(), WishError> {
        check(
            unsafe { sys::wish_log_debug(self.handle, cstr(msg).as_ptr()) },
            "client.log_debug",
            self.handle,
        )
    }
    pub fn log_info(&self, msg: &str) -> Result<(), WishError> {
        check(
            unsafe { sys::wish_log_info(self.handle, cstr(msg).as_ptr()) },
            "client.log_info",
            self.handle,
        )
    }
    pub fn log_warn(&self, msg: &str) -> Result<(), WishError> {
        check(
            unsafe { sys::wish_log_warn(self.handle, cstr(msg).as_ptr()) },
            "client.log_warn",
            self.handle,
        )
    }
    pub fn log_error(&self, msg: &str) -> Result<(), WishError> {
        check(
            unsafe { sys::wish_log_error(self.handle, cstr(msg).as_ptr()) },
            "client.log_error",
            self.handle,
        )
    }

    // ── Automation ───────────────────────────────────────────────────────
    //
    // Native (ABI-driven) automation: query the widget tree, take
    // screenshots, and inject synthetic input -- only available when the
    // connected server's active renderer implements the automation backend
    // (currently only the SDL3 renderer); see `src/automation/DESIGN.md`.

    /// Queries the current widget tree/hit-test snapshot as a raw JSON
    /// string, optionally filtered to `root` and its descendants (pass `""`
    /// for the whole tree).
    pub fn automation_get_tree(&self, root: &str) -> Result<String, WishError> {
        let mut out: *mut c_char = ptr::null_mut();
        check(
            unsafe { sys::wish_automation_get_tree(self.handle, cstr(root).as_ptr(), &mut out) },
            "client.automation_get_tree",
            self.handle,
        )?;
        let s = unsafe { CStr::from_ptr(out).to_string_lossy().into_owned() };
        unsafe { sys::bison_free_string(out) };
        Ok(s)
    }

    /// Retrieves the session's buffered automation log entries as a raw
    /// JSON string.
    pub fn automation_get_logs(&self) -> Result<String, WishError> {
        let mut out: *mut c_char = ptr::null_mut();
        check(
            unsafe { sys::wish_automation_get_logs(self.handle, &mut out) },
            "client.automation_get_logs",
            self.handle,
        )?;
        let s = unsafe { CStr::from_ptr(out).to_string_lossy().into_owned() };
        unsafe { sys::bison_free_string(out) };
        Ok(s)
    }

    /// Captures a screenshot of the next frame the server renders, as
    /// PNG-encoded bytes.
    pub fn automation_screenshot(&self) -> Result<Vec<u8>, WishError> {
        let mut out: *mut c_char = ptr::null_mut();
        let mut len: usize = 0;
        check(
            unsafe { sys::wish_automation_screenshot(self.handle, &mut out, &mut len) },
            "client.automation_screenshot",
            self.handle,
        )?;
        let data = unsafe { std::slice::from_raw_parts(out as *const u8, len).to_vec() };
        unsafe { sys::bison_free_string(out) };
        Ok(data)
    }

    /// Injects a synthetic mouse-move event (window-relative coordinates).
    pub fn automation_mouse_move(&self, x: f32, y: f32) -> Result<(), WishError> {
        check(
            unsafe { sys::wish_automation_mouse_move(self.handle, x, y) },
            "client.automation_mouse_move",
            self.handle,
        )
    }

    /// Injects a synthetic mouse-button press/release. `button`: 0 = left,
    /// 1 = right, 2 = middle.
    pub fn automation_mouse_button(&self, button: i32, down: bool) -> Result<(), WishError> {
        check(
            unsafe { sys::wish_automation_mouse_button(self.handle, button, down as i32) },
            "client.automation_mouse_button",
            self.handle,
        )
    }

    /// Injects a synthetic key press/release (`keycode` is the platform
    /// keycode, `SDL_Keycode` for the SDL3 renderer).
    pub fn automation_key_event(&self, keycode: i32, down: bool) -> Result<(), WishError> {
        check(
            unsafe { sys::wish_automation_key_event(self.handle, keycode, down as i32) },
            "client.automation_key_event",
            self.handle,
        )
    }

    /// Injects synthetic UTF-8 text input (e.g. for typing into an
    /// `InputText`).
    pub fn automation_text_input(&self, text: &str) -> Result<(), WishError> {
        check(
            unsafe { sys::wish_automation_text_input(self.handle, cstr(text).as_ptr()) },
            "client.automation_text_input",
            self.handle,
        )
    }

    pub fn raw_handle(&self) -> sys::wish_client_handle {
        self.handle
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { sys::wish_client_destroy(self.handle) };
            self.handle = ptr::null_mut();
        }
    }
}

// ─── Session trampoline ─────────────────────────────────────────────────────

struct SessionCtx<F> {
    fn_: Option<F>,
    panic: Option<String>,
}

/// C-callable entry point for `wish_client_run_with_params`'s `session_fn`.
/// `userdata` points at a `SessionCtx<F>` living on `Client::run`'s stack
/// frame for the duration of the call. A panic inside the user closure is
/// caught here (unwinding across an `extern "C"` boundary back into C++ is
/// undefined behavior) and re-raised as [`Error::SessionPanic`] by the
/// caller once `wish_client_run_with_params` returns.
unsafe extern "C" fn session_trampoline<F>(client: sys::wish_client_handle, userdata: *mut c_void)
where
    F: FnOnce(&Client),
{
    let ctx = &mut *(userdata as *mut SessionCtx<F>);
    let Some(f) = ctx.fn_.take() else { return };
    let client_view = Client::from_raw(client);
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| f(&client_view)));
    // The caller (Client::run) owns `client`'s destruction, not this view.
    std::mem::forget(client_view);
    if let Err(payload) = result {
        ctx.panic = Some(panic_message(payload));
    }
}

fn panic_message(payload: Box<dyn std::any::Any + Send>) -> String {
    if let Some(s) = payload.downcast_ref::<&str>() {
        s.to_string()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        "unknown panic payload".to_string()
    }
}
