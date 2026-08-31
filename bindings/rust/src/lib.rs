// MIT License © 2025 Binary Dice Games
//! Rust bindings for [wish](https://github.com/binary-dice-games/wish)'s
//! **client** C ABI (`wish_client_c.h`) -- connect to a running `wish
//! server` process and drive UI templates from Rust.
//!
//! This crate links directly against the precompiled `wish_client`
//! shared library at build time (see `build.rs`), the same model
//! `bindings/cpp/` uses -- unlike the Python and C# bindings, which
//! `dlopen`/P-Invoke it at run time. [`sys`] is the hand-maintained raw FFI
//! layer; [`value`], [`proxy`], and [`client`] are the safe, idiomatic
//! wrappers most callers should use. This binding is client-only: it wraps
//! `wish_client_c.h` (session/template/proxy calls) plus the underlying
//! `bison_c.h`/`rmi_c.h` primitives needed to build/read field payloads and
//! drive proxies/futures -- not `wish_server_c.h` (a real UI-hosting server
//! with SDL3/web rendering), which has no analogue in this crate.
//!
//! # Quick start
//!
//! ```no_run
//! use wish::{Client, Value};
//!
//! let client = Client::tcp("127.0.0.1", 7070);
//! client.run(|c| {
//!     c.set_style_preset("dark").unwrap();
//!     c.register_template("ui", r#"{
//!         "type": "Window", "title": "Hi",
//!         "children": { "label": { "type": "Label", "text": "" } }
//!     }"#).unwrap();
//!     let _root = c.instantiate_template("ui", "ui").unwrap();
//!     let label = c.proxy_get("ui.label").unwrap();
//!     let mut fields = Value::new();
//!     fields.set_string("text", "Hello from Rust").unwrap();
//!     label.set(&fields, -1).unwrap();
//! }).unwrap();
//! ```

pub mod client;
pub mod key;
pub mod proxy;
pub mod sys;
pub mod value;

pub use client::{list_apps_json, Client, Error, QuitHandle, WishError};
pub use key::key;
pub use proxy::{Future, Proxy, RmiError};
pub use value::{BisonError, Value};
