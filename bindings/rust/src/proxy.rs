// MIT License © 2025 Binary Dice Games
//! Safe wrappers around `rmi_proxy_handle` / `rmi_future_handle` -- the Rust
//! analogue of `bindings/cpp/include/wish_cpp/proxy.hpp` and `future.hpp`.

use std::fmt;
use std::os::raw::c_void;
use std::ptr;

use crate::key::key;
use crate::sys;
use crate::value::Value;

// ─── Errors ─────────────────────────────────────────────────────────────────

/// Raised when an `rmi_*` C API call returns a non-zero error code.
#[derive(Debug, Clone)]
pub struct RmiError {
    /// The raw `rmi_error` code (see `rmi_c.h`).
    pub code: i32,
    message: String,
}

impl RmiError {
    fn new(code: i32, context: &str) -> Self {
        RmiError {
            code,
            message: format!("{context}: {}", error_message(code)),
        }
    }
}

impl fmt::Display for RmiError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.message)
    }
}

impl std::error::Error for RmiError {}

fn error_message(code: i32) -> &'static str {
    match code {
        sys::RMI_ERR_NULL => "null handle or pointer",
        sys::RMI_ERR_INVALID_STATE => "operation invalid for current state (e.g. not connected)",
        sys::RMI_ERR_TIMEOUT => "request timed out",
        sys::RMI_ERR_REMOTE_EXCEPTION => "server raised an exception",
        sys::RMI_ERR_TRANSPORT => "transport error",
        sys::RMI_ERR_EXCEPTION => "internal C++ exception",
        _ => "unknown error",
    }
}

fn check(rc: sys::rmi_error, context: &str) -> Result<(), RmiError> {
    if rc == sys::RMI_OK {
        Ok(())
    } else {
        Err(RmiError::new(rc, context))
    }
}

fn params_handle(params: Option<&Value>) -> sys::bison_handle {
    params.map_or(ptr::null_mut(), Value::raw_handle)
}

// ─── Future ─────────────────────────────────────────────────────────────────

/// RAII wrapper around an `rmi_future_handle`. Resolves to either a
/// [`Value`] ([`Future::get`]) or a [`Proxy`] ([`Future::get_proxy`]),
/// matching whichever async call produced it -- both take `self` by value,
/// so the C ABI's "set to null on consume" rule is enforced by Rust's
/// ownership system.
pub struct Future {
    handle: sys::rmi_future_handle,
}

unsafe impl Send for Future {}

impl Future {
    pub(crate) fn from_raw(handle: sys::rmi_future_handle) -> Self {
        Future { handle }
    }

    /// Blocks until the operation completes; does not consume the future.
    pub fn wait(&self, timeout_ms: i64) -> Result<(), RmiError> {
        check(
            unsafe { sys::rmi_future_wait(self.handle, timeout_ms) },
            "future.wait",
        )
    }

    /// Consumes the future and returns its [`Value`] result.
    pub fn get(mut self) -> Result<Value, RmiError> {
        let mut out: sys::bison_handle = ptr::null_mut();
        check(
            unsafe { sys::rmi_future_get_dynamic(&mut self.handle, &mut out) },
            "future.get",
        )?;
        Ok(Value::adopt(out))
    }

    /// Consumes the future and returns its [`Proxy`] result.
    pub fn get_proxy(mut self) -> Result<Proxy, RmiError> {
        let mut out: sys::rmi_proxy_handle = ptr::null_mut();
        check(
            unsafe { sys::rmi_future_get_proxy(&mut self.handle, &mut out) },
            "future.get_proxy",
        )?;
        Ok(Proxy::from_raw(out))
    }
}

impl Drop for Future {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { sys::rmi_future_release(self.handle) };
            self.handle = ptr::null_mut();
        }
    }
}

// ─── Proxy ──────────────────────────────────────────────────────────────────

type EventCallback = dyn FnMut(&Value) + Send;

/// A live handle to a remote UI element (`rmi_proxy_handle`).
///
/// [`Proxy::get`]/[`Proxy::set`] project/patch fields; [`Proxy::call`]
/// invokes a remote method by name; [`Proxy::on_event`] subscribes to a
/// server-pushed event (e.g. a button's `clicked`). `Drop` releases the
/// proxy (`rmi_proxy_release`).
pub struct Proxy {
    handle: sys::rmi_proxy_handle,
    // Raw pointers to `Box<Box<dyn FnMut(&Value) + Send>>` registered via
    // `on_event`, kept alive for the proxy's lifetime and freed on `Drop`
    // (mirrors `wish_cpp::proxy::handlers_`).
    callbacks: Vec<*mut c_void>,
}

unsafe impl Send for Proxy {}
// SAFETY: the underlying C ABI documents its RMI client as safe for
// concurrent use from multiple threads (requests are processed serially
// internally) -- see rmi_c.h's "Thread safety" section -- so sharing a
// `&Proxy` across threads (e.g. via `Arc<Proxy>`) is sound. `on_event`'s
// `&mut self` requirement (for pushing onto `callbacks`) is the only
// exclusive-access need, and that always happens before any sharing.
unsafe impl Sync for Proxy {}

impl Proxy {
    pub(crate) fn from_raw(handle: sys::rmi_proxy_handle) -> Self {
        Proxy {
            handle,
            callbacks: Vec::new(),
        }
    }

    pub fn is_valid(&self) -> bool {
        !self.handle.is_null()
    }

    // ── Remote field access ─────────────────────────────────────────────

    /// Fetches a full field snapshot.
    pub fn get(&self, timeout_ms: i64) -> Result<Value, RmiError> {
        let mut out: sys::bison_handle = ptr::null_mut();
        check(
            unsafe { sys::rmi_proxy_get(self.handle, Value::null_handle(), &mut out, timeout_ms) },
            "proxy.get",
        )?;
        Ok(Value::adopt(out))
    }

    /// Fetches only the fields named in `projection`.
    pub fn get_projected(&self, projection: &Value, timeout_ms: i64) -> Result<Value, RmiError> {
        let mut out: sys::bison_handle = ptr::null_mut();
        check(
            unsafe {
                sys::rmi_proxy_get(self.handle, projection.raw_handle(), &mut out, timeout_ms)
            },
            "proxy.get",
        )?;
        Ok(Value::adopt(out))
    }

    pub fn get_async(&self) -> Result<Future, RmiError> {
        let mut out: sys::rmi_future_handle = ptr::null_mut();
        check(
            unsafe { sys::rmi_proxy_get_async(self.handle, Value::null_handle(), &mut out) },
            "proxy.get_async",
        )?;
        Ok(Future::from_raw(out))
    }

    /// Applies a partial field update without resetting unspecified fields.
    pub fn set(&self, fields: &Value, timeout_ms: i64) -> Result<(), RmiError> {
        check(
            unsafe { sys::rmi_proxy_set(self.handle, fields.raw_handle(), timeout_ms) },
            "proxy.set",
        )
    }

    pub fn set_async(&self, fields: &Value) -> Result<Future, RmiError> {
        let mut out: sys::rmi_future_handle = ptr::null_mut();
        check(
            unsafe { sys::rmi_proxy_set_async(self.handle, fields.raw_handle(), &mut out) },
            "proxy.set_async",
        )?;
        Ok(Future::from_raw(out))
    }

    /// Resets explicitly-set fields back to prototype/inherited defaults.
    pub fn clear(&self, timeout_ms: i64) -> Result<(), RmiError> {
        check(
            unsafe { sys::rmi_proxy_clear(self.handle, timeout_ms) },
            "proxy.clear",
        )
    }

    pub fn clear_async(&self) -> Result<Future, RmiError> {
        let mut out: sys::rmi_future_handle = ptr::null_mut();
        check(
            unsafe { sys::rmi_proxy_clear_async(self.handle, &mut out) },
            "proxy.clear_async",
        )?;
        Ok(Future::from_raw(out))
    }

    // ── Remote method calls ─────────────────────────────────────────────

    /// Invokes a named remote method with `params` (`None` for no
    /// arguments).
    pub fn call(
        &self,
        name: &str,
        params: Option<&Value>,
        timeout_ms: i64,
    ) -> Result<Value, RmiError> {
        let mut out: sys::bison_handle = ptr::null_mut();
        check(
            unsafe {
                sys::rmi_proxy_call(
                    self.handle,
                    key(name),
                    params_handle(params),
                    &mut out,
                    timeout_ms,
                )
            },
            &format!("proxy.call({name:?})"),
        )?;
        Ok(Value::adopt(out))
    }

    pub fn call_async(&self, name: &str, params: Option<&Value>) -> Result<Future, RmiError> {
        let mut out: sys::rmi_future_handle = ptr::null_mut();
        check(
            unsafe {
                sys::rmi_proxy_call_async(self.handle, key(name), params_handle(params), &mut out)
            },
            &format!("proxy.call_async({name:?})"),
        )?;
        Ok(Future::from_raw(out))
    }

    // ── Events ───────────────────────────────────────────────────────────

    /// Subscribes to a server-initiated event on this element (e.g. a
    /// button's `"clicked"`). `handler` must be `Send`: it is invoked from
    /// the RMI worker thread that delivers the push event, not necessarily
    /// the thread that called `on_event`.
    pub fn on_event<F>(&mut self, name: &str, handler: F) -> Result<(), RmiError>
    where
        F: FnMut(&Value) + Send + 'static,
    {
        let boxed: Box<EventCallback> = Box::new(handler);
        let raw = Box::into_raw(Box::new(boxed)) as *mut c_void;
        let rc = unsafe { sys::rmi_proxy_on_event(self.handle, key(name), event_trampoline, raw) };
        if rc != sys::RMI_OK {
            unsafe { drop(Box::from_raw(raw as *mut Box<EventCallback>)) };
            return Err(RmiError::new(rc, &format!("on_event({name:?})")));
        }
        self.callbacks.push(raw);
        Ok(())
    }
}

impl Drop for Proxy {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { sys::rmi_proxy_release(self.handle) };
            self.handle = ptr::null_mut();
        }
        for raw in self.callbacks.drain(..) {
            unsafe { drop(Box::from_raw(raw as *mut Box<EventCallback>)) };
        }
    }
}

/// Trampoline invoked by the C library for every `rmi_proxy_on_event`
/// registration in this crate; `user` points at the boxed Rust closure.
/// Panics are caught here since unwinding across an `extern "C"` call (back
/// into C++) is undefined behavior.
unsafe extern "C" fn event_trampoline(params_h: sys::bison_handle, user: *mut c_void) {
    let closure = &mut *(user as *mut Box<EventCallback>);
    // `params_h` is only valid for the duration of this call and must not be
    // released by us -- take our own reference before wrapping it, since
    // `Value`'s `Drop` releases whatever handle it holds.
    let owned = if params_h.is_null() {
        ptr::null_mut()
    } else {
        sys::bison_add_ref(params_h)
    };
    let params_val = Value::adopt(owned);
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        closure(&params_val);
    }));
}
