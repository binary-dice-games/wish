// MIT License © 2025 Binary Dice Games
//! Hand-maintained `extern "C"` mirror of the subset of `bison_c.h` /
//! `rmi_c.h` / `wish_client_c.h` this crate needs.
//!
//! This module is the Rust analogue of `wish/_native.py`'s
//! `_setup_signatures()` / C#'s `Native.cs`: every exported `bison_*` /
//! `rmi_*` / `wish_*` C function this crate calls gets one raw declaration
//! here, grouped by the same section dividers the headers use. No
//! `bindgen` codegen is used, matching `extern/bison/bindings/rust`'s own
//! `sys.rs` (consistency with the other bindings, none of which use one,
//! and no new libclang build-dependency). Only the subset wish's client
//! binding actually needs is declared -- e.g. no class-registry or
//! vector-field functions, since `bindings/cpp/include/wish_cpp/value.hpp`
//! (this crate's structural template) does not use them either.
//!
//! This is a private, unsafe layer; public consumers use [`crate::value`],
//! [`crate::proxy`], and [`crate::client`] instead.

#![allow(non_camel_case_types)]

use std::os::raw::{c_char, c_int, c_void};

// ─── Shared C type aliases ──────────────────────────────────────────────────

pub type bison_handle = *mut c_void;
pub type bison_hash = u32;
pub type bison_error = c_int;

pub type rmi_proxy_handle = *mut c_void;
pub type rmi_future_handle = *mut c_void;
pub type rmi_error = c_int;

pub type wish_client_handle = *mut c_void;
pub type wish_hash = bison_hash;
pub type wish_error = c_int;

// ─── bison_error codes ──────────────────────────────────────────────────────

pub const BISON_OK: bison_error = 0;
pub const BISON_ERR_NULL: bison_error = -1;
pub const BISON_ERR_TYPE: bison_error = -2;
pub const BISON_ERR_NOT_FOUND: bison_error = -3;
pub const BISON_ERR_DUPLICATE: bison_error = -4;
pub const BISON_ERR_EXCEPTION: bison_error = -5;
pub const BISON_ERR_PARSE: bison_error = -6;

// ─── rmi_error codes ────────────────────────────────────────────────────────

pub const RMI_OK: rmi_error = 0;
pub const RMI_ERR_NULL: rmi_error = -1;
pub const RMI_ERR_INVALID_STATE: rmi_error = -2;
pub const RMI_ERR_TIMEOUT: rmi_error = -3;
pub const RMI_ERR_REMOTE_EXCEPTION: rmi_error = -4;
pub const RMI_ERR_TRANSPORT: rmi_error = -5;
pub const RMI_ERR_EXCEPTION: rmi_error = -6;

// ─── wish_error codes ───────────────────────────────────────────────────────

pub const WISH_OK: wish_error = 0;
pub const WISH_ERR_NULL: wish_error = -1;
pub const WISH_ERR_NOT_FOUND: wish_error = -2;
pub const WISH_ERR_TRANSPORT: wish_error = -3;
pub const WISH_ERR_EXCEPTION: wish_error = -4;
pub const WISH_ERR_AMBIGUOUS: wish_error = -5;

// ─── Callback types ─────────────────────────────────────────────────────────

/// `rmi_proxy_event_fn`: `void (*)(bison_handle params, void* user)`
pub type rmi_proxy_event_fn = unsafe extern "C" fn(params: bison_handle, user: *mut c_void);

/// `wish_session_fn`: `void (*)(wish_client_handle client, void* userdata)`
pub type wish_session_fn = unsafe extern "C" fn(client: wish_client_handle, userdata: *mut c_void);

extern "C" {
    // ── bison_c.h: lifecycle ────────────────────────────────────────────────

    pub fn bison_create(klass_name: bison_hash) -> bison_handle;
    pub fn bison_add_ref(h: bison_handle) -> bison_handle;
    pub fn bison_release(h: bison_handle);
    pub fn bison_clone(h: bison_handle) -> bison_handle;

    // ── Import / export helpers ─────────────────────────────────────────────

    pub fn bison_from_json(json: *const c_char) -> bison_handle;
    pub fn bison_from_yaml(yaml: *const c_char) -> bison_handle;
    pub fn bison_to_json(h: bison_handle, indent: c_int, out: *mut *mut c_char) -> bison_error;
    pub fn bison_to_yaml(h: bison_handle, out: *mut *mut c_char) -> bison_error;
    pub fn bison_free_string(s: *mut c_char);

    // ── Field access -- scalar (named) ──────────────────────────────────────

    pub fn bison_set_int(h: bison_handle, name: bison_hash, value: i32) -> bison_error;
    pub fn bison_set_float(h: bison_handle, name: bison_hash, value: f32) -> bison_error;
    pub fn bison_set_bool(h: bison_handle, name: bison_hash, value: c_int) -> bison_error;
    pub fn bison_set_string(h: bison_handle, name: bison_hash, value: *const c_char)
        -> bison_error;
    pub fn bison_set_object(h: bison_handle, name: bison_hash, value: bison_handle) -> bison_error;

    pub fn bison_get_int(h: bison_handle, name: bison_hash, out: *mut i32) -> bison_error;
    pub fn bison_get_float(h: bison_handle, name: bison_hash, out: *mut f32) -> bison_error;
    pub fn bison_get_bool(h: bison_handle, name: bison_hash, out: *mut c_int) -> bison_error;
    pub fn bison_get_string(
        h: bison_handle,
        name: bison_hash,
        buf: *mut c_char,
        buf_len: usize,
        len_out: *mut usize,
    ) -> bison_error;
    pub fn bison_get_object(
        h: bison_handle,
        name: bison_hash,
        out: *mut bison_handle,
    ) -> bison_error;

    // ── Field access -- scalar (indexed) ────────────────────────────────────

    pub fn bison_set_int_at(h: bison_handle, index: usize, value: i32) -> bison_error;
    pub fn bison_set_float_at(h: bison_handle, index: usize, value: f32) -> bison_error;
    pub fn bison_set_string_at(h: bison_handle, index: usize, value: *const c_char) -> bison_error;

    pub fn bison_get_int_at(h: bison_handle, index: usize, out: *mut i32) -> bison_error;
    pub fn bison_get_float_at(h: bison_handle, index: usize, out: *mut f32) -> bison_error;
    pub fn bison_get_string_at(
        h: bison_handle,
        index: usize,
        buf: *mut c_char,
        buf_len: usize,
        len_out: *mut usize,
    ) -> bison_error;

    pub fn bison_size(h: bison_handle) -> usize;

    // ── Utility ──────────────────────────────────────────────────────────────

    pub fn bison_key(name: *const c_char) -> bison_hash;

    // ── rmi_c.h: futures ─────────────────────────────────────────────────────

    pub fn rmi_future_wait(future: rmi_future_handle, timeout_ms: i64) -> rmi_error;
    pub fn rmi_future_get_dynamic(
        future: *mut rmi_future_handle,
        out_value: *mut bison_handle,
    ) -> rmi_error;
    pub fn rmi_future_get_proxy(
        future: *mut rmi_future_handle,
        out_proxy: *mut rmi_proxy_handle,
    ) -> rmi_error;
    pub fn rmi_future_release(future: rmi_future_handle);

    // ── rmi_c.h: proxy ───────────────────────────────────────────────────────

    pub fn rmi_proxy_release(proxy: rmi_proxy_handle);
    pub fn rmi_proxy_on_event(
        proxy: rmi_proxy_handle,
        event_name: bison_hash,
        handler: rmi_proxy_event_fn,
        user: *mut c_void,
    ) -> rmi_error;
    pub fn rmi_proxy_clear(proxy: rmi_proxy_handle, timeout_ms: i64) -> rmi_error;
    pub fn rmi_proxy_clear_async(
        proxy: rmi_proxy_handle,
        out_future: *mut rmi_future_handle,
    ) -> rmi_error;
    pub fn rmi_proxy_set(
        proxy: rmi_proxy_handle,
        fields: bison_handle,
        timeout_ms: i64,
    ) -> rmi_error;
    pub fn rmi_proxy_set_async(
        proxy: rmi_proxy_handle,
        fields: bison_handle,
        out_future: *mut rmi_future_handle,
    ) -> rmi_error;
    pub fn rmi_proxy_get(
        proxy: rmi_proxy_handle,
        projection: bison_handle,
        out_result: *mut bison_handle,
        timeout_ms: i64,
    ) -> rmi_error;
    pub fn rmi_proxy_get_async(
        proxy: rmi_proxy_handle,
        projection: bison_handle,
        out_future: *mut rmi_future_handle,
    ) -> rmi_error;
    pub fn rmi_proxy_call(
        proxy: rmi_proxy_handle,
        method: bison_hash,
        params: bison_handle,
        out_result: *mut bison_handle,
        timeout_ms: i64,
    ) -> rmi_error;
    pub fn rmi_proxy_call_async(
        proxy: rmi_proxy_handle,
        method: bison_hash,
        params: bison_handle,
        out_future: *mut rmi_future_handle,
    ) -> rmi_error;

    // ── wish_client_c.h: key hashing ─────────────────────────────────────────

    pub fn wish_key(name: *const c_char) -> wish_hash;

    // ── wish_client_c.h: client lifecycle ────────────────────────────────────

    pub fn wish_client_tcp_create(host: *const c_char, port: u16) -> wish_client_handle;
    pub fn wish_client_tls_create(host: *const c_char, port: u16) -> wish_client_handle;
    pub fn wish_client_stream_create(path: *const c_char) -> wish_client_handle;
    pub fn wish_client_pipe_create(path: *const c_char) -> wish_client_handle;
    pub fn wish_client_term_create() -> wish_client_handle;
    pub fn wish_client_destroy(client: wish_client_handle);

    pub fn wish_client_run(
        client: wish_client_handle,
        session_fn: wish_session_fn,
        userdata: *mut c_void,
    ) -> wish_error;
    pub fn wish_client_run_with_params(
        client: wish_client_handle,
        session_fn: wish_session_fn,
        userdata: *mut c_void,
        connect_params: bison_handle,
    ) -> wish_error;
    pub fn wish_client_wait(client: wish_client_handle);
    pub fn wish_client_quit(client: wish_client_handle);
    pub fn wish_last_error(client: wish_client_handle) -> *const c_char;

    // ── wish_client_c.h: style ────────────────────────────────────────────────

    pub fn wish_set_style_preset(client: wish_client_handle, preset: *const c_char) -> wish_error;
    pub fn wish_set_style_preset_async(
        client: wish_client_handle,
        preset: *const c_char,
        out_future: *mut rmi_future_handle,
    ) -> wish_error;

    // ── wish_client_c.h: template management ─────────────────────────────────

    pub fn wish_register_template(
        client: wish_client_handle,
        name: *const c_char,
        descriptor: *const c_char,
    ) -> wish_error;
    pub fn wish_register_template_async(
        client: wish_client_handle,
        name: *const c_char,
        descriptor: *const c_char,
        out_future: *mut rmi_future_handle,
    ) -> wish_error;
    pub fn wish_instantiate_template(
        client: wish_client_handle,
        name: *const c_char,
        prefix: *const c_char,
    ) -> rmi_proxy_handle;
    pub fn wish_instantiate_template_async(
        client: wish_client_handle,
        name: *const c_char,
        prefix: *const c_char,
        out_future: *mut rmi_future_handle,
    ) -> wish_error;
    pub fn wish_proxy_get(client: wish_client_handle, dot_path: *const c_char) -> rmi_proxy_handle;
    pub fn wish_release(client: wish_client_handle, prefix: *const c_char) -> wish_error;

    // ── wish_client_c.h: object instantiation ────────────────────────────────

    pub fn wish_instantiate(
        client: wish_client_handle,
        ns: wish_hash,
        klass: wish_hash,
        params: bison_handle,
    ) -> rmi_proxy_handle;

    // ── wish_client_c.h: embedded apps ───────────────────────────────────────

    pub fn wish_list_apps(out: *mut *mut c_char) -> wish_error;
    pub fn wish_run_app(
        client: wish_client_handle,
        app_name: *const c_char,
        args: *const *const c_char,
        nargs: usize,
    ) -> wish_error;

    // ── wish_client_c.h: file transfer ────────────────────────────────────────

    pub fn wish_upload_file(
        client: wish_client_handle,
        name: *const c_char,
        data: *const c_char,
        data_len: usize,
    ) -> wish_error;
    pub fn wish_download_file(
        client: wish_client_handle,
        name: *const c_char,
        out_data: *mut *mut c_char,
        out_len: *mut usize,
    ) -> wish_error;
    pub fn wish_upload_file_from_path(
        client: wish_client_handle,
        name: *const c_char,
        local_path: *const c_char,
    ) -> wish_error;
    pub fn wish_download_file_to_path(
        client: wish_client_handle,
        name: *const c_char,
        local_path: *const c_char,
    ) -> wish_error;
    pub fn wish_upload_package_from_path(
        client: wish_client_handle,
        dest_path: *const c_char,
        local_zip_path: *const c_char,
    ) -> wish_error;

    // ── wish_client_c.h: logging ──────────────────────────────────────────────

    pub fn wish_log(
        client: wish_client_handle,
        level: *const c_char,
        msg: *const c_char,
    ) -> wish_error;
    pub fn wish_log_debug(client: wish_client_handle, msg: *const c_char) -> wish_error;
    pub fn wish_log_info(client: wish_client_handle, msg: *const c_char) -> wish_error;
    pub fn wish_log_warn(client: wish_client_handle, msg: *const c_char) -> wish_error;
    pub fn wish_log_error(client: wish_client_handle, msg: *const c_char) -> wish_error;

    // ── wish_client_c.h: automation ───────────────────────────────────────────

    pub fn wish_automation_get_tree(
        client: wish_client_handle,
        root: *const c_char,
        out_json: *mut *mut c_char,
    ) -> wish_error;
    pub fn wish_automation_get_logs(
        client: wish_client_handle,
        out_json: *mut *mut c_char,
    ) -> wish_error;
    pub fn wish_automation_screenshot(
        client: wish_client_handle,
        out_png_data: *mut *mut c_char,
        out_len: *mut usize,
    ) -> wish_error;
    pub fn wish_automation_mouse_move(client: wish_client_handle, x: f32, y: f32) -> wish_error;
    pub fn wish_automation_mouse_button(
        client: wish_client_handle,
        button: c_int,
        down: c_int,
    ) -> wish_error;
    pub fn wish_automation_key_event(
        client: wish_client_handle,
        keycode: c_int,
        down: c_int,
    ) -> wish_error;
    pub fn wish_automation_text_input(
        client: wish_client_handle,
        utf8_text: *const c_char,
    ) -> wish_error;
}
