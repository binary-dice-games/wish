// MIT License © 2025 Binary Dice Games

// Tests for the wish Go server binding.
//
// Mirrors bindings/python/tests/test_server.py's and
// bindings/rust/wish-server/tests/server_tests.rs's lifecycle coverage. A
// full client/server round trip is deliberately not tested here: it would
// require also linking package wish (the client binding), and
// libwish_client / libwish_server both export the bison_* C ABI -- exactly
// the "one binary, one library" constraint this package documents. The
// round trip is covered instead by running examples/basic_server_example
// against any ABI client (see docs/bindings.md). What we can prove without
// a client is that the server actually binds and accepts TCP connections,
// via a raw net.Dial (below).
package wishserver

import (
	"net"
	"strconv"
	"testing"
	"time"
)

// freePort grabs a free localhost TCP port by listening on :0 and
// immediately closing.
func freePort(t *testing.T) uint16 {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := uint16(l.Addr().(*net.TCPAddr).Port)
	l.Close()
	return port
}

// ═════════════════════════════════════════════════════════════════════════
// Lifecycle (no client required)
// ═════════════════════════════════════════════════════════════════════════

func TestTCPServerConstructionDoesNotCrash(t *testing.T) {
	server, err := NewTCPServer("127.0.0.1", freePort(t))
	if err != nil {
		t.Fatal(err)
	}
	defer server.Destroy()
	if server.LastError() != "" {
		t.Fatalf("last_error = %q", server.LastError())
	}
}

func TestTLSServerConstructionDoesNotCrash(t *testing.T) {
	server, err := NewTLSServer("127.0.0.1", freePort(t))
	if err != nil {
		t.Fatal(err)
	}
	server.Destroy()
	// Idempotent.
	server.Destroy()
}

func TestPipeServerConstructionDoesNotCrash(t *testing.T) {
	server, err := NewPipeServer("/tmp/wish-go-server-binding-test.sock")
	if err != nil {
		t.Fatal(err)
	}
	defer server.Destroy()
}

func TestStopBeforeStartIsNoop(t *testing.T) {
	server, err := NewTCPServer("127.0.0.1", freePort(t))
	if err != nil {
		t.Fatal(err)
	}
	defer server.Destroy()
	if err := server.Stop(); err != nil {
		t.Fatalf("stop before start: %v", err)
	}
}

func TestShouldQuitFalseBeforeStart(t *testing.T) {
	server, err := NewTCPServer("127.0.0.1", freePort(t))
	if err != nil {
		t.Fatal(err)
	}
	defer server.Destroy()
	if server.ShouldQuit() {
		t.Fatal("should_quit true before start")
	}
}

func TestBadRendererKindMapsToBadRendererError(t *testing.T) {
	server, err := NewTCPServer("127.0.0.1", freePort(t))
	if err != nil {
		t.Fatal(err)
	}
	defer server.Destroy()

	startErr := server.Start("not-a-real-renderer", nil)
	if startErr == nil {
		t.Fatal("expected error for unknown renderer_kind")
	}
	// Regression check: an unknown renderer_kind must map to
	// ServerErrBadRenderer specifically, not the generic ServerErrException
	// every other internal failure uses.
	se, ok := startErr.(*ServerError)
	if !ok || se.Code != ServerErrBadRenderer {
		t.Fatalf("expected *ServerError code %d, got %v", ServerErrBadRenderer, startErr)
	}
}

func TestSetLogLevelRejectsUnknownLevel(t *testing.T) {
	server, err := NewTCPServer("127.0.0.1", freePort(t))
	if err != nil {
		t.Fatal(err)
	}
	defer server.Destroy()
	if err := server.SetLogLevel("not-a-level"); err == nil {
		t.Fatal("expected error for unknown log level")
	}
}

// ═════════════════════════════════════════════════════════════════════════
// Console renderer: start / accept / stop
// ═════════════════════════════════════════════════════════════════════════

func TestConsoleServerBindsAndAcceptsATCPConnection(t *testing.T) {
	port := freePort(t)
	server, err := NewTCPServer("127.0.0.1", port)
	if err != nil {
		t.Fatal(err)
	}
	defer server.Destroy()

	params := NewParams().SetString("title", "Test").SetInt("width", 800)
	defer params.Close()
	if err := server.Start("console", params); err != nil {
		t.Fatalf("start: %v", err)
	}

	// The accept loop runs on a background thread; give it a moment to
	// actually be listening (matching test_server.py's own 0.1s sleep).
	time.Sleep(200 * time.Millisecond)

	conn, err := net.DialTimeout("tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(int(port))), 2*time.Second)
	if err != nil {
		t.Fatalf("server should be accepting connections: %v", err)
	}
	conn.Close()

	if server.ShouldQuit() {
		t.Fatal("should_quit true while running")
	}
	if err := server.Stop(); err != nil {
		t.Fatalf("stop: %v", err)
	}
}

func TestStartTwiceErrors(t *testing.T) {
	server, err := NewTCPServer("127.0.0.1", freePort(t))
	if err != nil {
		t.Fatal(err)
	}
	defer server.Destroy()

	if err := server.Start("console", nil); err != nil {
		t.Fatalf("first start: %v", err)
	}
	if err := server.Start("console", nil); err == nil {
		t.Fatal("expected error starting an already-started server")
	}
	if err := server.Stop(); err != nil {
		t.Fatalf("stop: %v", err)
	}
}
