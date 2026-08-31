// MIT License © 2025 Binary Dice Games
//! Tests for the wish Rust server binding.
//!
//! Mirrors `bindings/python/tests/test_server.py`'s lifecycle coverage. A
//! full client/server round trip is deliberately *not* tested here: it would
//! require also linking the `wish` (client) crate, and `libwish_client` /
//! `libwish_server` both export the `bison_*` C ABI -- exactly the "one
//! binary, one library" constraint this crate documents. The round trip is
//! covered instead by running `examples/basic_server` against any ABI
//! client (see docs/bindings.md). What we *can* prove without a client is
//! that the server actually binds and accepts TCP connections, via a raw
//! `std::net::TcpStream` (below).

use std::net::{TcpListener, TcpStream};
use std::time::Duration;

use wish_server::{Params, Server};

/// Grabs a free localhost TCP port by binding to :0 and immediately
/// dropping the listener.
fn free_port() -> u16 {
    TcpListener::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .port()
}

// ═════════════════════════════════════════════════════════════════════════
// Lifecycle (no client required)
// ═════════════════════════════════════════════════════════════════════════

#[test]
fn tcp_create_and_drop_does_not_crash() {
    let server = Server::tcp("127.0.0.1", free_port());
    assert_eq!(server.last_error(), "");
}

#[test]
fn tls_create_does_not_crash() {
    let _server = Server::tls("127.0.0.1", free_port());
}

#[test]
fn pipe_create_does_not_crash() {
    let _server = Server::pipe("/tmp/wish-rust-server-binding-test.sock");
}

#[test]
fn stop_before_start_is_noop() {
    let mut server = Server::tcp("127.0.0.1", free_port());
    server.stop().unwrap(); // must not error even though start() was never called
}

#[test]
fn should_quit_is_false_before_start() {
    let server = Server::tcp("127.0.0.1", free_port());
    assert!(!server.should_quit());
}

#[test]
fn bad_renderer_kind_maps_to_bad_renderer_error() {
    let mut server = Server::tcp("127.0.0.1", free_port());
    let err = server.start("not-a-real-renderer", None).unwrap_err();
    // Regression check: an unknown renderer_kind must map to
    // WISH_SERVER_ERR_BAD_RENDERER specifically, not the generic
    // WISH_SERVER_ERR_EXCEPTION every other internal failure uses.
    assert_eq!(err.code, wish_server::sys::WISH_SERVER_ERR_BAD_RENDERER);
}

#[test]
fn set_log_level_rejects_unknown_level() {
    let server = Server::tcp("127.0.0.1", free_port());
    assert!(server.set_log_level("not-a-level").is_err());
}

// ═════════════════════════════════════════════════════════════════════════
// Console renderer: start / accept / stop
// ═════════════════════════════════════════════════════════════════════════

#[test]
fn console_server_binds_and_accepts_a_tcp_connection() {
    let port = free_port();
    let mut server = Server::tcp("127.0.0.1", port);
    server
        .start(
            "console",
            Some(&Params::new().string("title", "Test").int("width", 800)),
        )
        .unwrap();

    // The accept loop runs on a background thread; give it a moment to
    // actually be listening (matching test_server.py's own 0.1s sleep).
    std::thread::sleep(Duration::from_millis(200));

    let conn = TcpStream::connect(("127.0.0.1", port));
    assert!(
        conn.is_ok(),
        "server should be accepting connections: {conn:?}"
    );

    assert!(!server.should_quit());
    server.stop().unwrap();
}

#[test]
fn start_twice_errors() {
    let mut server = Server::tcp("127.0.0.1", free_port());
    server.start("console", None).unwrap();
    let err = server.start("console", None).unwrap_err();
    assert_eq!(err.code, wish_server::sys::WISH_SERVER_ERR_EXCEPTION);
    server.stop().unwrap();
}
