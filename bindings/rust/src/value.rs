// MIT License © 2025 Binary Dice Games
//! Safe, RAII wrapper around `bison_handle` -- the Rust analogue of
//! `bindings/cpp/include/wish_cpp/value.hpp`'s `value` class.
//!
//! `bdg::bison::dynamic` (the native, linked C++ value type) is compiled/
//! linked library code, not header-only or ABI-reachable in a form this
//! crate can bind against directly. Like `wish_cpp::value`, this type is a
//! thin, self-contained wrapper built directly on the `bison_*` C ABI
//! functions re-exported by `libwish_client`/`wish_client.dll` -- no
//! separate `bison_abi` library or class-registry support is needed, since
//! the wish client never registers classes or methods, only builds/reads
//! field payloads for template/proxy calls.

use std::ffi::{CStr, CString};
use std::fmt;
use std::os::raw::c_char;
use std::ptr;

use crate::key::key;
use crate::sys;

// ─── Errors ─────────────────────────────────────────────────────────────────

/// Raised when a `bison_*` C API call returns a non-zero error code.
#[derive(Debug, Clone)]
pub struct BisonError {
    /// The raw `bison_error` code (see `bison_c.h`).
    pub code: i32,
    message: String,
}

impl BisonError {
    fn new(code: i32, context: &str) -> Self {
        BisonError {
            code,
            message: format!("{context}: {}", error_message(code)),
        }
    }
}

impl fmt::Display for BisonError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.message)
    }
}

impl std::error::Error for BisonError {}

fn error_message(code: i32) -> &'static str {
    match code {
        sys::BISON_ERR_NULL => "null handle or pointer",
        sys::BISON_ERR_TYPE => "field holds a different type than requested",
        sys::BISON_ERR_NOT_FOUND => "method or field not found",
        sys::BISON_ERR_DUPLICATE => "duplicate class or method",
        sys::BISON_ERR_EXCEPTION => "internal C++ exception",
        sys::BISON_ERR_PARSE => "input string failed to parse (JSON / YAML)",
        _ => "unknown error",
    }
}

fn check(rc: sys::bison_error, context: &str) -> Result<(), BisonError> {
    if rc == sys::BISON_OK {
        Ok(())
    } else {
        Err(BisonError::new(rc, context))
    }
}

// ─── Value ──────────────────────────────────────────────────────────────────

/// RAII wrapper around a `bison_handle` -- a reference-counted map/array of
/// typed fields, used for proxy `set()`/`get()`/`call()` payloads, event
/// parameters, and connect params.
pub struct Value {
    handle: sys::bison_handle,
}

// SAFETY: see `extern/bison/bindings/rust/src/dynamic.rs`'s identical note --
// a `Value` is only ever accessed by one thread at a time under ordinary
// Rust ownership rules, which is all the underlying library requires.
unsafe impl Send for Value {}

impl Value {
    /// A new, empty object (mirrors `wish::value{}` / `bison::dynamic{}`).
    pub fn new() -> Value {
        let h = unsafe { sys::bison_create(0) };
        assert!(!h.is_null(), "bison_create failed");
        Value { handle: h }
    }

    /// Adopts ownership of an existing handle (may be null).
    pub(crate) fn adopt(h: sys::bison_handle) -> Value {
        Value { handle: h }
    }

    pub(crate) fn raw_handle(&self) -> sys::bison_handle {
        self.handle
    }

    /// A raw handle for a `NULL` payload, distinct from an empty object --
    /// used where the C ABI treats `NULL` specially (e.g. "no projection").
    pub(crate) fn null_handle() -> sys::bison_handle {
        ptr::null_mut()
    }

    // ── Import / export ─────────────────────────────────────────────────

    pub fn parse_json(json: &str) -> Result<Value, BisonError> {
        let c = CString::new(json).map_err(|_| {
            BisonError::new(
                sys::BISON_ERR_PARSE,
                "parse_json: input contains a NUL byte",
            )
        })?;
        let h = unsafe { sys::bison_from_json(c.as_ptr()) };
        if h.is_null() {
            return Err(BisonError::new(
                sys::BISON_ERR_PARSE,
                "parse_json: invalid JSON",
            ));
        }
        Ok(Value { handle: h })
    }

    pub fn parse_yaml(yaml: &str) -> Result<Value, BisonError> {
        let c = CString::new(yaml).map_err(|_| {
            BisonError::new(
                sys::BISON_ERR_PARSE,
                "parse_yaml: input contains a NUL byte",
            )
        })?;
        let h = unsafe { sys::bison_from_yaml(c.as_ptr()) };
        if h.is_null() {
            return Err(BisonError::new(
                sys::BISON_ERR_PARSE,
                "parse_yaml: invalid YAML",
            ));
        }
        Ok(Value { handle: h })
    }

    pub fn to_json(&self, indent: i32) -> Result<String, BisonError> {
        let mut out: *mut c_char = ptr::null_mut();
        check(
            unsafe { sys::bison_to_json(self.handle, indent, &mut out) },
            "to_json",
        )?;
        let s = unsafe { CStr::from_ptr(out).to_string_lossy().into_owned() };
        unsafe { sys::bison_free_string(out) };
        Ok(s)
    }

    pub fn to_yaml(&self) -> Result<String, BisonError> {
        let mut out: *mut c_char = ptr::null_mut();
        check(
            unsafe { sys::bison_to_yaml(self.handle, &mut out) },
            "to_yaml",
        )?;
        let s = unsafe { CStr::from_ptr(out).to_string_lossy().into_owned() };
        unsafe { sys::bison_free_string(out) };
        Ok(s)
    }

    // ── Scalar field setters (named) ─────────────────────────────────────

    pub fn set_int(&mut self, name: &str, value: i32) -> Result<(), BisonError> {
        check(
            unsafe { sys::bison_set_int(self.handle, key(name), value) },
            "set_int",
        )
    }
    pub fn set_float(&mut self, name: &str, value: f32) -> Result<(), BisonError> {
        check(
            unsafe { sys::bison_set_float(self.handle, key(name), value) },
            "set_float",
        )
    }
    pub fn set_bool(&mut self, name: &str, value: bool) -> Result<(), BisonError> {
        check(
            unsafe { sys::bison_set_bool(self.handle, key(name), value as i32) },
            "set_bool",
        )
    }
    pub fn set_string(&mut self, name: &str, value: &str) -> Result<(), BisonError> {
        let c = CString::new(value).map_err(|_| {
            BisonError::new(
                sys::BISON_ERR_EXCEPTION,
                "set_string: value contains a NUL byte",
            )
        })?;
        check(
            unsafe { sys::bison_set_string(self.handle, key(name), c.as_ptr()) },
            "set_string",
        )
    }
    pub fn set_object(&mut self, name: &str, value: Option<&Value>) -> Result<(), BisonError> {
        let h = value.map_or(ptr::null_mut(), |v| v.handle);
        check(
            unsafe { sys::bison_set_object(self.handle, key(name), h) },
            "set_object",
        )
    }

    // ── Scalar field getters (named) ─────────────────────────────────────

    pub fn get_int(&self, name: &str) -> Option<i32> {
        let mut out = 0i32;
        (unsafe { sys::bison_get_int(self.handle, key(name), &mut out) } == sys::BISON_OK)
            .then_some(out)
    }
    pub fn get_float(&self, name: &str) -> Option<f32> {
        let mut out = 0f32;
        (unsafe { sys::bison_get_float(self.handle, key(name), &mut out) } == sys::BISON_OK)
            .then_some(out)
    }
    pub fn get_bool(&self, name: &str) -> Option<bool> {
        let mut out = 0i32;
        (unsafe { sys::bison_get_bool(self.handle, key(name), &mut out) } == sys::BISON_OK)
            .then_some(out != 0)
    }
    pub fn get_string(&self, name: &str) -> Option<String> {
        let k = key(name);
        let mut len_out: usize = 0;
        if unsafe { sys::bison_get_string(self.handle, k, ptr::null_mut(), 0, &mut len_out) }
            != sys::BISON_OK
        {
            return None;
        }
        let mut buf = vec![0u8; len_out + 1];
        if unsafe {
            sys::bison_get_string(
                self.handle,
                k,
                buf.as_mut_ptr() as *mut c_char,
                buf.len(),
                ptr::null_mut(),
            )
        } != sys::BISON_OK
        {
            return None;
        }
        buf.truncate(len_out);
        Some(String::from_utf8_lossy(&buf).into_owned())
    }
    pub fn get_object(&self, name: &str) -> Option<Value> {
        let mut out: sys::bison_handle = ptr::null_mut();
        if unsafe { sys::bison_get_object(self.handle, key(name), &mut out) } != sys::BISON_OK
            || out.is_null()
        {
            return None;
        }
        Some(Value { handle: out })
    }

    // ── Scalar field access (indexed) ────────────────────────────────────

    pub fn set_int_at(&mut self, index: usize, value: i32) -> Result<(), BisonError> {
        check(
            unsafe { sys::bison_set_int_at(self.handle, index, value) },
            "set_int_at",
        )
    }
    pub fn set_float_at(&mut self, index: usize, value: f32) -> Result<(), BisonError> {
        check(
            unsafe { sys::bison_set_float_at(self.handle, index, value) },
            "set_float_at",
        )
    }
    pub fn set_string_at(&mut self, index: usize, value: &str) -> Result<(), BisonError> {
        let c = CString::new(value).map_err(|_| {
            BisonError::new(
                sys::BISON_ERR_EXCEPTION,
                "set_string_at: value contains a NUL byte",
            )
        })?;
        check(
            unsafe { sys::bison_set_string_at(self.handle, index, c.as_ptr()) },
            "set_string_at",
        )
    }

    pub fn get_int_at(&self, index: usize) -> Option<i32> {
        let mut out = 0i32;
        (unsafe { sys::bison_get_int_at(self.handle, index, &mut out) } == sys::BISON_OK)
            .then_some(out)
    }
    pub fn get_float_at(&self, index: usize) -> Option<f32> {
        let mut out = 0f32;
        (unsafe { sys::bison_get_float_at(self.handle, index, &mut out) } == sys::BISON_OK)
            .then_some(out)
    }
    pub fn get_string_at(&self, index: usize) -> Option<String> {
        let mut len_out: usize = 0;
        if unsafe { sys::bison_get_string_at(self.handle, index, ptr::null_mut(), 0, &mut len_out) }
            != sys::BISON_OK
        {
            return None;
        }
        let mut buf = vec![0u8; len_out + 1];
        if unsafe {
            sys::bison_get_string_at(
                self.handle,
                index,
                buf.as_mut_ptr() as *mut c_char,
                buf.len(),
                ptr::null_mut(),
            )
        } != sys::BISON_OK
        {
            return None;
        }
        buf.truncate(len_out);
        Some(String::from_utf8_lossy(&buf).into_owned())
    }

    /// Number of array-like (numeric-key) elements.
    pub fn size(&self) -> usize {
        unsafe { sys::bison_size(self.handle) }
    }
}

impl Default for Value {
    fn default() -> Self {
        Value::new()
    }
}

impl Clone for Value {
    fn clone(&self) -> Self {
        let h = unsafe { sys::bison_clone(self.handle) };
        assert!(!h.is_null(), "bison_clone failed");
        Value { handle: h }
    }
}

impl Drop for Value {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { sys::bison_release(self.handle) };
            self.handle = ptr::null_mut();
        }
    }
}

impl fmt::Debug for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Value").field("size", &self.size()).finish()
    }
}
