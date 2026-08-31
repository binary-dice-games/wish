// MIT License © 2025 Binary Dice Games

// Tests for the wish Go client binding.
//
// Mirrors bindings/python/tests/test_client.py's coverage and its
// rationale: the wish client ABI has no standalone/in-process mode (see
// wish_client_c.h -- there is no wish_client_memory_create()), so these
// tests exercise only what doesn't require a live wish server: key
// hashing, Value get/set round trips, and client handle
// construction/error-code plumbing. Run() against an unreachable port
// still proves params/callbacks are marshaled through the FFI boundary
// cleanly (an error, not a crash); a real end-to-end round trip against a
// live server is exercised by examples/calculator_example (see
// docs/bindings.md's "Running the calculator example").
package wish

import (
	"strings"
	"testing"
)

func must(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatal(err)
	}
}

// ═════════════════════════════════════════════════════════════════════════
// Key hashing
// ═════════════════════════════════════════════════════════════════════════

func TestKeyIsDeterministic(t *testing.T) {
	if Key("clicked") != Key("clicked") {
		t.Fatal("Key(\"clicked\") is not deterministic")
	}
}

func TestKeyDifferentNamesDiffer(t *testing.T) {
	if Key("clicked") == Key("text") {
		t.Fatal("distinct names hashed to the same key")
	}
}

func TestKeyHighBitSet(t *testing.T) {
	if Key("clicked")&0x8000_0000 == 0 {
		t.Fatal("expected MSB set")
	}
}

// ═════════════════════════════════════════════════════════════════════════
// Value: scalar / nested / indexed field access
// ═════════════════════════════════════════════════════════════════════════

func TestValueScalarRoundTrip(t *testing.T) {
	v, err := NewValue()
	must(t, err)
	defer v.Close()

	must(t, v.SetInt("i", 42))
	must(t, v.SetFloat("f", 1.5))
	must(t, v.SetBool("b", true))
	must(t, v.SetString("s", "hi"))

	i, err := v.GetInt("i")
	must(t, err)
	if i != 42 {
		t.Fatalf("i = %d", i)
	}
	f, err := v.GetFloat("f")
	must(t, err)
	if f != 1.5 {
		t.Fatalf("f = %v", f)
	}
	b, err := v.GetBool("b")
	must(t, err)
	if !b {
		t.Fatal("b = false")
	}
	s, err := v.GetString("s")
	must(t, err)
	if s != "hi" {
		t.Fatalf("s = %q", s)
	}

	// bison_get_int() auto-vivifies an absent field as its type's default
	// (0) rather than failing -- same behavior as bison::dynamic::
	// operator[]() at the C++ level.
	missing, err := v.GetInt("missing")
	must(t, err)
	if missing != 0 {
		t.Fatalf("missing = %d", missing)
	}
}

func TestValueNestedObjectRoundTrips(t *testing.T) {
	inner, err := NewValue()
	must(t, err)
	defer inner.Close()
	must(t, inner.SetInt("x", 1))

	outer, err := NewValue()
	must(t, err)
	defer outer.Close()
	must(t, outer.SetObject("child", inner))

	child, err := outer.GetObject("child")
	must(t, err)
	defer child.Close()
	x, err := child.GetInt("x")
	must(t, err)
	if x != 1 {
		t.Fatalf("x = %d", x)
	}
}

func TestValueJSONRoundTrips(t *testing.T) {
	v, err := ParseJSON(`{"a":1,"b":"two"}`)
	must(t, err)
	defer v.Close()

	a, err := v.GetInt("a")
	must(t, err)
	if a != 1 {
		t.Fatalf("a = %d", a)
	}
	b, err := v.GetString("b")
	must(t, err)
	if b != "two" {
		t.Fatalf("b = %q", b)
	}
}

func TestValueArrayIndexRoundTrips(t *testing.T) {
	v, err := ParseJSON(`{"items":["a","b","c"]}`)
	must(t, err)
	defer v.Close()

	items, err := v.GetObject("items")
	must(t, err)
	defer items.Close()
	if items.Size() != 3 {
		t.Fatalf("size = %d", items.Size())
	}
	first, err := items.GetStringAt(0)
	must(t, err)
	if first != "a" {
		t.Fatalf("items[0] = %q", first)
	}
}

func TestValueInvalidJSONErrors(t *testing.T) {
	if _, err := ParseJSON("not json"); err == nil {
		t.Fatal("expected error")
	}
}

func TestValueToJSONRoundTrips(t *testing.T) {
	v, err := ParseJSON(`{"x":1}`)
	must(t, err)
	defer v.Close()
	s, err := v.ToJSON(-1)
	must(t, err)
	if !strings.Contains(s, "1") {
		t.Fatalf("to_json output missing value: %q", s)
	}
}

func TestValueCloneIsIndependent(t *testing.T) {
	v, err := NewValue()
	must(t, err)
	defer v.Close()
	must(t, v.SetInt("n", 1))

	clone, err := v.Clone()
	must(t, err)
	defer clone.Close()
	must(t, clone.SetInt("n", 2))

	vn, err := v.GetInt("n")
	must(t, err)
	cn, err := clone.GetInt("n")
	must(t, err)
	if vn != 1 || cn != 2 {
		t.Fatalf("v.n=%d clone.n=%d", vn, cn)
	}
}

// ═════════════════════════════════════════════════════════════════════════
// Client lifecycle (no live server required)
// ═════════════════════════════════════════════════════════════════════════

func TestTCPClientConstructionDoesNotCrash(t *testing.T) {
	client, err := NewTCPClient("127.0.0.1", 7070)
	must(t, err)
	defer client.Destroy()
	if client.LastError() != "" {
		t.Fatalf("last_error = %q", client.LastError())
	}
}

func TestTLSClientConstructionDoesNotCrash(t *testing.T) {
	client, err := NewTLSClient("127.0.0.1", 7070)
	must(t, err)
	client.Destroy()
	// Idempotent.
	client.Destroy()
}

func TestPipeClientConstructionDoesNotCrash(t *testing.T) {
	client, err := NewPipeClient("/tmp/wish-go-binding-test.sock")
	must(t, err)
	defer client.Destroy()
}

func TestRunAgainstUnreachablePortFailsCleanly(t *testing.T) {
	// Port 1 is a reserved/privileged port essentially never listening.
	client, err := NewTCPClient("127.0.0.1", 1)
	must(t, err)
	defer client.Destroy()

	ranSession := false
	runErr := client.Run(func(c *Client) { ranSession = true })
	if runErr == nil {
		t.Fatal("expected error")
	}
	if ranSession {
		t.Fatal("session callback must not run for a failed connection")
	}
}

func TestRunWithParamsAgainstUnreachablePortFailsCleanly(t *testing.T) {
	client, err := NewTCPClient("127.0.0.1", 1)
	must(t, err)
	defer client.Destroy()

	params, err := NewValue()
	must(t, err)
	defer params.Close()
	must(t, params.SetString("username", "alice"))

	runErr := client.RunWithParams(func(c *Client) {
		t.Fatal("session callback must not run for a failed connection")
	}, params)
	if runErr == nil {
		t.Fatal("expected error")
	}
}

func TestListAppsJSONReturnsAJSONArray(t *testing.T) {
	// Does not require a connection -- app registration happens at
	// library-load time, independent of any session.
	json, err := ListAppsJSON()
	must(t, err)
	trimmed := strings.TrimSpace(json)
	if !strings.HasPrefix(trimmed, "[") {
		t.Fatalf("expected a JSON array, got %q", json)
	}
}
