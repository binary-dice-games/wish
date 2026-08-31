// MIT License © 2025 Binary Dice Games

// Package wish provides Go bindings for wish
// (https://github.com/binary-dice-games/wish)'s **client** C ABI
// (wish_client_c.h) -- connect to a running "wish server" process and drive
// UI templates from Go. This is a client-only binding: it does not wrap
// wish_server_c.h (the real UI-hosting server, SDL3/web rendering), which
// has no analogue here -- see docs/bindings.md's Python/C# sections for why
// that's out of scope.
//
// This package links directly against the precompiled wish_client shared
// library at build time via cgo (see this file's preamble below) -- the
// same model bindings/cpp/ and bindings/rust/ use, unlike the Python and
// C# bindings, which dlopen/P-Invoke it at run time. cgo is also the only
// practical way for this package to hand C a function pointer that calls
// back into Go (wish_session_fn and rmi_proxy_event_fn both need this),
// since Go's //export mechanism requires it.
//
// [Value] (in value.go) is a thin, self-contained wrapper around the
// subset of bison_c.h this crate needs (mirroring
// bindings/cpp/include/wish_cpp/value.hpp); [Proxy] and [Future] (in
// proxy.go) wrap rmi_c.h's proxy/future primitives; [Client] (in
// client.go) wraps wish_client_c.h. This file (native.go) carries the cgo
// preamble and the C-callable trampolines that dispatch session/event
// callbacks into Go closures; native.go, value.go, proxy.go, and client.go
// each carry their own `import "C"` (cgo preambles are per file, but the
// #cgo CFLAGS/LDFLAGS directives declared here apply package-wide, so the
// other files only need to repeat the header #includes).
//
// # Quick start
//
//	client := wish.NewTCPClient("127.0.0.1", 7070)
//	defer client.Destroy()
//	err := client.Run(func(c *wish.Client) {
//		c.SetStylePreset("dark")
//		c.RegisterTemplate("ui", `{"type":"Window","title":"Hi",
//			"children":{"label":{"type":"Label","text":""}}}`)
//		root, _ := c.InstantiateTemplate("ui", "ui")
//		defer root.Close()
//		label, _ := c.ProxyGet("ui.label")
//		defer label.Close()
//		fields, _ := wish.NewValue()
//		defer fields.Close()
//		fields.SetString("text", "Hello from Go")
//		label.Set(fields, -1)
//	})
//
// # Overriding the library location
//
// The default #cgo directives below resolve wish_client_c.h/bison_c.h/
// rmi_c.h and libwish_client against this checkout's sibling include/ and
// build/ directories (three levels up from bindings/go/wish/). To build
// against a wish_client installed elsewhere, set the CGO_CFLAGS/CGO_LDFLAGS
// environment variables before `go build`/`go test` -- the Go toolchain
// automatically merges env-supplied CGO_CFLAGS/CGO_LDFLAGS with the #cgo
// directives below. This is the Go-native equivalent of every other wish
// binding's WISH_LIB override:
//
//	export CGO_CFLAGS="-I/path/to/wish/include"
//	export CGO_LDFLAGS="-L/path/to/wish/build -lwish_client -Wl,-rpath,/path/to/wish/build"
//	go build ./...
package wish

/*
#cgo CFLAGS: -I${SRCDIR}/../../../include -I${SRCDIR}/../../../extern/bison/include
#cgo LDFLAGS: -L${SRCDIR}/../../../build -lwish_client -Wl,-rpath,${SRCDIR}/../../../build
#include <stdlib.h>
#include "wish_client_c.h"
#include "shim.h"

// Forward declarations so shim.c's registration calls (and this package's
// other cgo files) can reference these //export'd Go functions as C
// function pointers before cgo's generated header is available.
extern void goSessionTrampoline(wish_client_handle client, void* userdata);
extern void goProxyEventTrampoline(bison_handle params, void* user);
*/
import "C"

import (
	"runtime/cgo"
	"unsafe"
)

// goSessionTrampoline is the C-callable entry point for every session
// callback passed to Client.Run/Client.RunWithParams; `userdata` is a
// runtime/cgo.Handle resolving to the registered Go closure. A panic
// inside the closure is recovered here and re-raised by Run/RunWithParams
// after wish_client_run_with_params returns (unwinding across this
// extern "C" boundary back into C++ would be undefined behavior).
//
//export goSessionTrampoline
func goSessionTrampoline(client C.wish_client_handle, userdata unsafe.Pointer) {
	h := cgo.Handle(uintptr(userdata))
	ctx, ok := h.Value().(*sessionCtx)
	if !ok {
		return
	}
	defer func() {
		if r := recover(); r != nil {
			ctx.panicValue = r
		}
	}()
	ctx.fn(&Client{handle: client})
}

// goProxyEventTrampoline is the C-callable entry point for every handler
// registered via Proxy.OnEvent. See goSessionTrampoline's doc comment for
// the panic-recovery convention, which is identical here -- a panic in an
// event handler is recovered and swallowed at the C ABI boundary rather
// than allowed to unwind into C++.
//
//export goProxyEventTrampoline
func goProxyEventTrampoline(params C.bison_handle, user unsafe.Pointer) {
	defer func() { _ = recover() }()
	h := cgo.Handle(uintptr(user))
	fn, ok := h.Value().(func(params *Value))
	if !ok {
		return
	}
	// `params` is only valid for the duration of this call and must not be
	// released by us -- take our own reference before wrapping it, since
	// Value.Close releases whatever handle it holds.
	var owned C.bison_handle
	if params != nil {
		owned = C.bison_add_ref(params)
	}
	paramsVal := &Value{handle: owned}
	defer paramsVal.Close()
	fn(paramsVal)
}
