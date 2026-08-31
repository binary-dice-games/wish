// MIT License © 2025 Binary Dice Games
//! Tests for the wish Rust client binding.
//!
//! Mirrors `bindings/python/tests/test_client.py`'s coverage and its
//! rationale: the wish client ABI has no standalone/in-process mode (see
//! `wish_client_c.h` -- there is no `wish_client_memory_create()`), so these
//! tests exercise only what doesn't require a live `wish server`: key
//! hashing, `Value` get/set round trips, and client handle
//! construction/error-code plumbing. `run()` against an unreachable port
//! still proves params/callbacks are marshaled through the FFI boundary
//! cleanly (an error, not a crash); a real end-to-end round trip against a
//! live server is exercised by `examples/calculator.rs` (see
//! docs/bindings.md's "Running the calculator example").

use wish::{key, Client, Value};

// ═════════════════════════════════════════════════════════════════════════
// Key hashing
// ═════════════════════════════════════════════════════════════════════════

#[test]
fn key_is_deterministic() {
    assert_eq!(key("clicked"), key("clicked"));
}

#[test]
fn key_different_names_differ() {
    assert_ne!(key("clicked"), key("text"));
}

#[test]
fn key_high_bit_set() {
    assert!(key("clicked") & 0x8000_0000 != 0);
}

// ═════════════════════════════════════════════════════════════════════════
// Value: scalar / nested / indexed field access
// ═════════════════════════════════════════════════════════════════════════

#[test]
fn value_scalar_round_trip() {
    let mut v = Value::new();
    v.set_int("i", 42).unwrap();
    v.set_float("f", 1.5).unwrap();
    v.set_bool("b", true).unwrap();
    v.set_string("s", "hi").unwrap();

    assert_eq!(v.get_int("i"), Some(42));
    assert!((v.get_float("f").unwrap() - 1.5).abs() < 1e-6);
    assert_eq!(v.get_bool("b"), Some(true));
    assert_eq!(v.get_string("s").as_deref(), Some("hi"));

    // bison_get_int() auto-vivifies an absent field as its type's default
    // (0) rather than failing -- same behavior as bison::dynamic::operator[]
    // at the C++ level.
    assert_eq!(v.get_int("missing"), Some(0));
}

#[test]
fn value_nested_object_round_trips() {
    let mut inner = Value::new();
    inner.set_int("x", 1).unwrap();
    let mut outer = Value::new();
    outer.set_object("child", Some(&inner)).unwrap();

    let child = outer.get_object("child").unwrap();
    assert_eq!(child.get_int("x"), Some(1));
}

#[test]
fn value_json_round_trips() {
    let v = Value::parse_json(r#"{"a":1,"b":"two"}"#).unwrap();
    assert_eq!(v.get_int("a"), Some(1));
    assert_eq!(v.get_string("b").as_deref(), Some("two"));
}

#[test]
fn value_array_index_round_trips() {
    let v = Value::parse_json(r#"{"items":["a","b","c"]}"#).unwrap();
    let items = v.get_object("items").unwrap();
    assert_eq!(items.size(), 3);
    assert_eq!(items.get_string_at(0).as_deref(), Some("a"));
    assert_eq!(items.get_string_at(2).as_deref(), Some("c"));
}

#[test]
fn value_invalid_json_errors() {
    assert!(Value::parse_json("not json").is_err());
}

#[test]
fn value_to_json_round_trips() {
    let v = Value::parse_json(r#"{"x":1}"#).unwrap();
    let s = v.to_json(-1).unwrap();
    assert!(s.contains('1'));
}

#[test]
fn value_clone_is_independent() {
    let mut v = Value::new();
    v.set_int("n", 1).unwrap();
    let mut clone = v.clone();
    clone.set_int("n", 2).unwrap();
    assert_eq!(v.get_int("n"), Some(1));
    assert_eq!(clone.get_int("n"), Some(2));
}

// ═════════════════════════════════════════════════════════════════════════
// Client lifecycle (no live server required)
// ═════════════════════════════════════════════════════════════════════════

#[test]
fn tcp_client_construction_does_not_crash() {
    let client = Client::tcp("127.0.0.1", 7070);
    assert_eq!(client.last_error(), "");
}

#[test]
fn tls_client_construction_does_not_crash() {
    let _client = Client::tls("127.0.0.1", 7070);
}

#[test]
fn pipe_client_construction_does_not_crash() {
    let _client = Client::pipe("/tmp/wish-rust-binding-test.sock");
}

#[test]
fn run_against_unreachable_port_fails_cleanly() {
    // Port 1 is a reserved/privileged port essentially never listening.
    let client = Client::tcp("127.0.0.1", 1);
    let result = client.run(|_c| {
        panic!("session callback must not run for a failed connection");
    });
    assert!(result.is_err());
}

#[test]
fn run_with_params_against_unreachable_port_fails_cleanly() {
    let client = Client::tcp("127.0.0.1", 1);
    let mut params = Value::new();
    params.set_string("username", "alice").unwrap();
    let result = client.run_with_params(
        |_c| panic!("session callback must not run for a failed connection"),
        Some(&params),
    );
    assert!(result.is_err());
}

#[test]
fn list_apps_json_returns_a_json_array() {
    // Does not require a connection -- app registration happens at
    // library-load time, independent of any session.
    let json = wish::list_apps_json().unwrap();
    assert!(json.trim_start().starts_with('['));
}
