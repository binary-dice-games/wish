// MIT License © 2025 Binary Dice Games

// Package wish: this file is the safe, idiomatic Go wrapper around
// rmi_c.h's proxy/future primitives -- the Go analogue of
// bindings/cpp/include/wish_cpp/proxy.hpp and future.hpp.
package wish

/*
#include <stdlib.h>
#include "wish_client_c.h"
#include "shim.h"

// Forward declaration for the //export'd trampoline defined in native.go
// (see shim.c/shim.h for rmi_proxy_on_event_shim itself).
extern void goProxyEventTrampoline(bison_handle params, void* user);
*/
import "C"

import (
	"fmt"
	"runtime"
	"runtime/cgo"
)

func paramsHandle(v *Value) C.bison_handle {
	return v.rawHandle()
}

// ─── Errors ─────────────────────────────────────────────────────────────────

// rmi_error codes (see rmi_c.h), exported as typed constants so callers can
// compare an *RmiError's Code field directly.
const (
	RmiErrNull            int32 = -1
	RmiErrInvalidState    int32 = -2
	RmiErrTimeout         int32 = -3
	RmiErrRemoteException int32 = -4
	RmiErrTransport       int32 = -5
	RmiErrException       int32 = -6
)

var rmiErrorMessages = map[int32]string{
	RmiErrNull:            "null handle or pointer",
	RmiErrInvalidState:    "operation invalid for current state (e.g. not connected)",
	RmiErrTimeout:         "request timed out",
	RmiErrRemoteException: "server raised an exception",
	RmiErrTransport:       "transport error",
	RmiErrException:       "internal C++ exception",
}

func rmiErrorMessage(code int32) string {
	if msg, ok := rmiErrorMessages[code]; ok {
		return msg
	}
	return fmt.Sprintf("unknown error %d", code)
}

// RmiError is returned when an rmi_* C API call reports a non-zero error
// code. Code is the raw rmi_error value (see rmi_c.h).
type RmiError struct {
	Code    int32
	message string
}

func (e *RmiError) Error() string { return e.message }

func newRmiError(code int32, context string) *RmiError {
	msg := rmiErrorMessage(code)
	if context != "" {
		msg = context + ": " + msg
	}
	return &RmiError{Code: code, message: msg}
}

func checkRmi(rc C.rmi_error, context string) error {
	if rc == C.RMI_OK {
		return nil
	}
	return newRmiError(int32(rc), context)
}

// ─── Future ─────────────────────────────────────────────────────────────────

// Future wraps an rmi_future_handle for an in-flight asynchronous
// operation. It is consumed exactly once, via GetValue or GetProxy (both
// nil the internal handle immediately, so a second call to either returns
// an error rather than double-consuming the future) -- or discarded via
// Release, also safe to call multiple times.
type Future struct {
	handle C.rmi_future_handle
}

func newFuture(h C.rmi_future_handle) *Future {
	f := &Future{handle: h}
	runtime.SetFinalizer(f, (*Future).finalize)
	return f
}

func (f *Future) finalize() { f.Release() }

// Wait blocks until the operation completes. It does not consume the
// future.
func (f *Future) Wait(timeoutMs int64) error {
	rc := C.rmi_future_wait(f.handle, C.int64_t(timeoutMs))
	return checkRmi(rc, "future.wait")
}

// GetValue consumes the future and returns its Value result.
func (f *Future) GetValue() (*Value, error) {
	if f.handle == nil {
		return nil, fmt.Errorf("wish: future already consumed")
	}
	var out C.bison_handle
	rc := C.rmi_future_get_dynamic(&f.handle, &out)
	f.handle = nil
	if err := checkRmi(rc, "future.get_value"); err != nil {
		return nil, err
	}
	return newValueOwned(out), nil
}

// GetProxy consumes the future and returns its Proxy result.
func (f *Future) GetProxy() (*Proxy, error) {
	if f.handle == nil {
		return nil, fmt.Errorf("wish: future already consumed")
	}
	var out C.rmi_proxy_handle
	rc := C.rmi_future_get_proxy(&f.handle, &out)
	f.handle = nil
	if err := checkRmi(rc, "future.get_proxy"); err != nil {
		return nil, err
	}
	return newProxy(out), nil
}

// Release discards the future without consuming its result. Safe to call
// multiple times.
func (f *Future) Release() {
	if f.handle != nil {
		C.rmi_future_release(f.handle)
		f.handle = nil
	}
}

// ─── Proxy ──────────────────────────────────────────────────────────────────

// Proxy is a live handle to a remote UI element (rmi_proxy_handle). Get/Set
// project/patch fields; Call invokes a remote method by name; OnEvent
// subscribes to a server-pushed event (e.g. a button's "clicked"). Close
// releases the proxy (rmi_proxy_release).
type Proxy struct {
	handle    C.rmi_proxy_handle
	callbacks []cgo.Handle // OnEvent registrations, freed on Close.
}

func newProxy(h C.rmi_proxy_handle) *Proxy {
	p := &Proxy{handle: h}
	runtime.SetFinalizer(p, (*Proxy).finalize)
	return p
}

func (p *Proxy) finalize() { _ = p.Close() }

// IsValid reports whether this proxy holds a non-null handle.
func (p *Proxy) IsValid() bool { return p != nil && p.handle != nil }

// Close releases this proxy. Safe to call multiple times.
func (p *Proxy) Close() error {
	if p.handle != nil {
		C.rmi_proxy_release(p.handle)
		p.handle = nil
	}
	for _, h := range p.callbacks {
		h.Delete()
	}
	p.callbacks = nil
	return nil
}

// Get fetches a full field snapshot, or a projected subset when projection
// is non-nil.
func (p *Proxy) Get(projection *Value, timeoutMs int64) (*Value, error) {
	var out C.bison_handle
	rc := C.rmi_proxy_get(p.handle, paramsHandle(projection), &out, C.int64_t(timeoutMs))
	if err := checkRmi(rc, "proxy.get"); err != nil {
		return nil, err
	}
	return newValueOwned(out), nil
}

// GetAsync is the asynchronous counterpart to Get.
func (p *Proxy) GetAsync(projection *Value) (*Future, error) {
	var out C.rmi_future_handle
	rc := C.rmi_proxy_get_async(p.handle, paramsHandle(projection), &out)
	if err := checkRmi(rc, "proxy.get_async"); err != nil {
		return nil, err
	}
	return newFuture(out), nil
}

// Set applies a partial field update without resetting unspecified fields.
func (p *Proxy) Set(fields *Value, timeoutMs int64) error {
	rc := C.rmi_proxy_set(p.handle, paramsHandle(fields), C.int64_t(timeoutMs))
	return checkRmi(rc, "proxy.set")
}

// SetAsync is the asynchronous counterpart to Set.
func (p *Proxy) SetAsync(fields *Value) (*Future, error) {
	var out C.rmi_future_handle
	rc := C.rmi_proxy_set_async(p.handle, paramsHandle(fields), &out)
	if err := checkRmi(rc, "proxy.set_async"); err != nil {
		return nil, err
	}
	return newFuture(out), nil
}

// Clear resets explicitly-set fields back to prototype/inherited defaults.
func (p *Proxy) Clear(timeoutMs int64) error {
	rc := C.rmi_proxy_clear(p.handle, C.int64_t(timeoutMs))
	return checkRmi(rc, "proxy.clear")
}

// ClearAsync is the asynchronous counterpart to Clear.
func (p *Proxy) ClearAsync() (*Future, error) {
	var out C.rmi_future_handle
	rc := C.rmi_proxy_clear_async(p.handle, &out)
	if err := checkRmi(rc, "proxy.clear_async"); err != nil {
		return nil, err
	}
	return newFuture(out), nil
}

// Call invokes a named remote method with params (nil for no arguments).
func (p *Proxy) Call(name string, params *Value, timeoutMs int64) (*Value, error) {
	var out C.bison_handle
	rc := C.rmi_proxy_call(p.handle, cKey(name), paramsHandle(params), &out, C.int64_t(timeoutMs))
	if err := checkRmi(rc, fmt.Sprintf("proxy.call(%q)", name)); err != nil {
		return nil, err
	}
	return newValueOwned(out), nil
}

// CallAsync is the asynchronous counterpart to Call.
func (p *Proxy) CallAsync(name string, params *Value) (*Future, error) {
	var out C.rmi_future_handle
	rc := C.rmi_proxy_call_async(p.handle, cKey(name), paramsHandle(params), &out)
	if err := checkRmi(rc, fmt.Sprintf("proxy.call_async(%q)", name)); err != nil {
		return nil, err
	}
	return newFuture(out), nil
}

// OnEvent subscribes to a server-initiated event on this element (e.g. a
// button's "clicked"). handler receives a non-owning view of the event
// payload (do not Close it) and may be invoked from any goroutine/thread
// that receives the push event, not necessarily the one that called
// OnEvent; a panic inside handler is recovered and swallowed at the C ABI
// boundary rather than allowed to unwind into C++.
func (p *Proxy) OnEvent(name string, handler func(params *Value)) error {
	h := cgo.NewHandle(handler)
	rc := C.rmi_proxy_on_event_shim(p.handle, cKey(name), C.rmi_proxy_event_fn(C.goProxyEventTrampoline), C.uintptr_t(h))
	if rc != C.RMI_OK {
		h.Delete()
		return checkRmi(rc, fmt.Sprintf("on_event(%q)", name))
	}
	p.callbacks = append(p.callbacks, h)
	return nil
}
