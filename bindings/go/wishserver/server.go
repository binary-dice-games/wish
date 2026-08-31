// MIT License © 2025 Binary Dice Games

// Package wishserver: this file is the safe, idiomatic Go wrapper around
// wish_server_c.h -- the Go analogue of
// bindings/cpp/include/wish_cpp/server.hpp, bindings/python/wish/server.py,
// and bindings/rust/wish-server/src/server.rs, covering the full
// wish_server_c.h surface: server lifecycle for all four transports,
// log-level control, renderer start/stop, and the renderer-close /
// child-exit quit signal.
package wishserver

/*
#include <stdlib.h>
#include "wish_server_c.h"
*/
import "C"

import (
	"fmt"
	"runtime"
	"unsafe"
)

// ─── Errors ─────────────────────────────────────────────────────────────────

// wish_server_error codes (see wish_server_c.h), exported as typed
// constants so callers can compare a *ServerError's Code field directly.
const (
	ServerErrNull        int32 = -1
	ServerErrTransport   int32 = -3
	ServerErrException   int32 = -4
	ServerErrBadRenderer int32 = -6
)

var serverErrorMessages = map[int32]string{
	ServerErrNull:        "null handle or pointer",
	ServerErrTransport:   "transport listen failed",
	ServerErrException:   "internal C++ exception",
	ServerErrBadRenderer: "unknown renderer_kind, or this library wasn't built with support for it",
}

func serverErrorMessage(code int32) string {
	if msg, ok := serverErrorMessages[code]; ok {
		return msg
	}
	return fmt.Sprintf("unknown error %d", code)
}

// ServerError is returned when a wish_server_* C API call reports a
// non-zero error code. Code is the raw wish_server_error value (see
// wish_server_c.h); Detail (if non-empty) carries the server's
// last_error() message, which is often more specific than the generic
// per-code message.
type ServerError struct {
	Code    int32
	Detail  string
	message string
}

func (e *ServerError) Error() string { return e.message }

func newServerError(code int32, context string, server C.wish_server_handle) *ServerError {
	msg := context + ": " + serverErrorMessage(code)
	detail := ""
	if server != nil {
		detail = lastErrorString(server)
		if detail != "" {
			msg += " (" + detail + ")"
		}
	}
	return &ServerError{Code: code, Detail: detail, message: msg}
}

func checkServer(rc C.wish_server_error, context string, server C.wish_server_handle) error {
	if rc == C.WISH_SERVER_OK {
		return nil
	}
	return newServerError(int32(rc), context, server)
}

func lastErrorString(server C.wish_server_handle) string {
	p := C.wish_server_last_error(server)
	if p == nil {
		return ""
	}
	return C.GoString(p)
}

// ─── Server ─────────────────────────────────────────────────────────────────

// Server wraps a wish_server_handle. Construct via NewTCPServer,
// NewPipeServer, NewTLSServer, or NewTermServer, then call Start to build
// the requested renderer and begin accepting client connections. Destroy
// releases the handle (stopping the server first if still running); a
// runtime.SetFinalizer safety net also calls it if a caller forgets, but
// its timing is not guaranteed, so Destroy (typically via defer) is the
// primary pattern.
//
// This hosts the real bdg::wish::server implementation (the same one the
// "wish server" CLI uses), so template registration/instantiation gives
// each widget its own independently addressable proxy and events work --
// unlike a server built from bison's generic RMI primitives.
type Server struct {
	handle  C.wish_server_handle
	started bool
}

func newServer(h C.wish_server_handle) *Server {
	s := &Server{handle: h}
	runtime.SetFinalizer(s, (*Server).finalize)
	return s
}

func (s *Server) finalize() { s.Destroy() }

// NewTCPServer creates a TCP socket server (not yet listening).
func NewTCPServer(host string, port uint16) (*Server, error) {
	cHost := C.CString(host)
	defer C.free(unsafe.Pointer(cHost))
	h := C.wish_server_tcp_create(cHost, C.uint16_t(port))
	if h == nil {
		return nil, fmt.Errorf("wishserver: wish_server_tcp_create failed")
	}
	return newServer(h), nil
}

// NewPipeServer creates a named-pipe / Unix-socket server (not yet
// listening).
func NewPipeServer(path string) (*Server, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	h := C.wish_server_pipe_create(cPath)
	if h == nil {
		return nil, fmt.Errorf("wishserver: wish_server_pipe_create failed")
	}
	return newServer(h), nil
}

// NewTLSServer creates a TLS-secured TCP server (not yet listening). TLS
// material (cert_file/cert_pem, key_file/key_pem, key_password, and
// optionally client_auth/ca_file/ca_pem for mutual TLS) is supplied via
// Start's params.
func NewTLSServer(host string, port uint16) (*Server, error) {
	cHost := C.CString(host)
	defer C.free(unsafe.Pointer(cHost))
	h := C.wish_server_tls_create(cHost, C.uint16_t(port))
	if h == nil {
		return nil, fmt.Errorf("wishserver: wish_server_tls_create failed")
	}
	return newServer(h), nil
}

// NewTermServer creates a terminal (OSC-99 framed) server by spawning a
// child process attached to a new pseudo-terminal. The child is expected
// to be a wish client using wish.NewTermClient() over its own inherited
// stdio. ShouldQuit also returns true once that child exits.
//
// cmd empty spawns the operator's $SHELL (Linux/MSYS2) or cmd.exe
// (Windows).
func NewTermServer(cmd string) (*Server, error) {
	var cCmd *C.char
	if cmd != "" {
		cCmd = C.CString(cmd)
		defer C.free(unsafe.Pointer(cCmd))
	}
	h := C.wish_server_term_create(cCmd)
	if h == nil {
		return nil, fmt.Errorf("wishserver: wish_server_term_create failed")
	}
	return newServer(h), nil
}

// Destroy releases the server handle, stopping the server first if still
// running. Safe to call multiple times.
func (s *Server) Destroy() {
	if s.handle != nil {
		C.wish_server_destroy(s.handle)
		s.handle = nil
	}
}

// LastError returns the last error message recorded for this server
// (empty if none).
func (s *Server) LastError() string {
	if s.handle == nil {
		return ""
	}
	return lastErrorString(s.handle)
}

// SetVerbose is deprecated: prefer SetLogLevel. true maps to log level
// "trace", false to "none". Must be called before Start.
func (s *Server) SetVerbose(verbose bool) error {
	var cv C.int
	if verbose {
		cv = 1
	}
	return checkServer(C.wish_server_set_verbose(s.handle, cv), "set_verbose", s.handle)
}

// SetLogLevel sets the server log verbosity: one of "none", "fatal",
// "error", "warning", "info", "trace" (default "none"). RMI trace lines
// appear at "info" and above, decoded payloads at "trace". Must be called
// before Start.
func (s *Server) SetLogLevel(level string) error {
	cLevel := C.CString(level)
	defer C.free(unsafe.Pointer(cLevel))
	return checkServer(C.wish_server_set_log_level(s.handle, cLevel), "set_log_level", s.handle)
}

// Start builds renderer and begins accepting client connections.
//
// renderer is one of:
//   - "sdl3": a real SDL3 window (needs WISH_ENABLE_SDL3=ON).
//   - "web": the browser renderer on its own embedded HTTP+WebSocket
//     listener (needs WISH_ENABLE_WEB=ON); pass web_bind/web_port in
//     params and open the printed URL.
//   - "console": a lightweight text dump of the widget tree to stdout;
//     no display needed, meant for tests/CI.
//
// params (may be nil) carries renderer-specific fields, all optional:
// title, width, height, font_size for "sdl3"/"web"; web_bind, web_port for
// "web". It is also forwarded unchanged as transport listen params -- e.g.
// cert_file/key_file for a NewTLSServer server; ignored by every other
// transport. Start does not take ownership of params; Close it yourself.
func (s *Server) Start(renderer string, params *Params) error {
	cRenderer := C.CString(renderer)
	defer C.free(unsafe.Pointer(cRenderer))
	rc := C.wish_server_start(s.handle, cRenderer, params.rawHandle())
	if err := checkServer(rc, fmt.Sprintf("start(%s)", renderer), s.handle); err != nil {
		return err
	}
	s.started = true
	return nil
}

// Stop stops the accept loop, render loop, and joins all threads. A no-op
// if Start was never called (or already stopped).
func (s *Server) Stop() error {
	if !s.started {
		return nil
	}
	if err := checkServer(C.wish_server_stop(s.handle), "stop", s.handle); err != nil {
		return err
	}
	s.started = false
	return nil
}

// ShouldQuit reports whether the renderer has signalled it should close
// (e.g. the SDL3 window was closed), or -- for a NewTermServer server --
// whether the spawned child process has exited. The web and console
// renderers never set this on their own; stop those with an explicit
// Stop.
func (s *Server) ShouldQuit() bool {
	return C.wish_server_should_quit(s.handle) != 0
}
