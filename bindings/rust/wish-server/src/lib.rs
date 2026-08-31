// MIT License © 2025 Binary Dice Games
//! Rust bindings for [wish](https://github.com/binary-dice-games/wish)'s
//! **server** C ABI (`wish_server_c.h`) -- host and render a real wish
//! session from Rust, the same protocol implementation (`bdg::wish::server`)
//! the `wish server` CLI uses. Template registration/instantiation gives
//! each widget its own independently addressable proxy, and events work,
//! unlike a server built from bison's generic RMI primitives.
//!
//! This is the server counterpart of the `wish` crate (`bindings/rust/`,
//! which wraps the *client* ABI). They are **separate crates on purpose**:
//! `libwish_client` and `libwish_server` both export the `bison_*`/`rmi_*` C
//! ABI symbols, so a single binary must link exactly one of them -- the same
//! constraint `bindings/cpp/`'s `wish_cpp` / `wish_cpp_server` targets and
//! the Python binding's `wish._native` / `wish._server_native` modules carry.
//! Depend on `wish` to drive a UI, on `wish-server` to host one; not both in
//! the same executable.
//!
//! Like the client crate, this links `wish_server` at **build** time (see
//! `build.rs`) -- but `wish_server` is gated behind a non-default CMake
//! option, so build it explicitly first:
//!
//! ```sh
//! cmake -B build -DWISH_BUILD_SERVER_SHARED=ON
//! cmake --build build --target wish_server_dll
//! ```
//!
//! # Quick start
//!
//! ```no_run
//! use wish_server::{Params, Server};
//!
//! let mut server = Server::tcp("127.0.0.1", 7070);
//! server
//!     .start("sdl3", Some(&Params::new().string("title", "My App").int("width", 1280)))
//!     .unwrap();
//! while !server.should_quit() {
//!     std::thread::sleep(std::time::Duration::from_millis(50));
//! }
//! server.stop().unwrap();
//! ```
//!
//! Any ABI-based wish client (the `wish` crate, the C++/Python/C#/Go
//! bindings, or the compiled `wish` binary) can then connect to it exactly
//! as it would to `wish server`.

pub mod params;
pub mod server;
pub mod sys;

pub use params::Params;
pub use server::{Server, ServerError};
