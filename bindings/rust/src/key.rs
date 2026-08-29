// MIT License © 2025 Binary Dice Games
//! Field/method/template name hashing.
//!
//! Rust has no `constexpr`-equivalent reachable from this crate the way
//! `bindings/cpp/include/wish_cpp/key.hpp`'s `"name"_key` user-defined
//! literal does, so -- like the Python, C#, and Go bindings -- every access
//! funnels through a runtime call to `wish_key()` (bit-identical to
//! `bison_key()`), memoized in a small bounded cache (mirroring
//! `extern/bison/bindings/rust/src/dynamic.rs`'s `KEY_CACHE_MAX` and
//! `wish/_native.py`'s memoized `key()`).

use std::collections::HashMap;
use std::ffi::CString;
use std::sync::{Mutex, OnceLock};

use crate::sys;

const KEY_CACHE_MAX: usize = 4096;

fn key_cache() -> &'static Mutex<HashMap<String, u32>> {
    static CACHE: OnceLock<Mutex<HashMap<String, u32>>> = OnceLock::new();
    CACHE.get_or_init(|| Mutex::new(HashMap::new()))
}

/// Returns the 32-bit FNV-1a hash of `name` (identical to `"name"_key` in
/// C++ and `wish.key(name)` in Python).
pub fn key(name: &str) -> u32 {
    if let Some(&h) = key_cache().lock().unwrap().get(name) {
        return h;
    }
    let c = CString::new(name).expect("field/method/template names must not contain NUL bytes");
    let hash = unsafe { sys::wish_key(c.as_ptr()) };
    let mut cache = key_cache().lock().unwrap();
    if cache.len() < KEY_CACHE_MAX {
        cache.insert(name.to_string(), hash);
    }
    hash
}

pub(crate) fn key_or_zero(name: &str) -> u32 {
    if name.is_empty() {
        0
    } else {
        key(name)
    }
}
