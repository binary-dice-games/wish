// MIT License © 2025 Binary Dice Games

// Package wishserver provides Go bindings for wish
// (https://github.com/binary-dice-games/wish)'s **server** C ABI
// (wish_server_c.h) -- host and render a real wish session from Go, the
// same bdg::wish::server implementation the "wish server" CLI uses.
// Template registration/instantiation gives each widget its own
// independently addressable proxy, and events work, unlike a server built
// from bison's generic RMI primitives.
//
// This is the server counterpart of package wish
// (github.com/binary-dice-games/wish/bindings/go/wish, the *client*
// binding). They are separate packages on purpose: libwish_client and
// libwish_server both export the bison_*/rmi_* C ABI symbols, so a single
// binary must link exactly one of them -- the same constraint
// bindings/cpp/'s two targets, the Python binding's two ctypes modules,
// and bindings/rust/'s two crates all carry. Import wish to drive a UI,
// wishserver to host one; not both in the same executable.
//
// Like package wish, this links directly against the precompiled
// wish_server shared library at build time via cgo -- but wish_server is
// gated behind a non-default CMake option, so build it explicitly first:
//
//	cmake -B build -DWISH_BUILD_SERVER_SHARED=ON
//	cmake --build build --target wish_server_dll
//
// Unlike package wish, there are no callbacks in wish_server_c.h, so this
// package needs no cgo.Handle trampolines or C shims -- just plain cgo
// calls. [Params] (in params.go) builds wish_server_start's renderer/
// listen params through libwish_server's own embedded bison_* functions
// (see params.go), independent of package wish's copy.
//
// # Quick start
//
//	server, _ := wishserver.NewTCPServer("127.0.0.1", 7070)
//	defer server.Destroy()
//	if err := server.Start("sdl3", wishserver.NewParams().SetString("title", "My App")); err != nil {
//		log.Fatal(err)
//	}
//	for !server.ShouldQuit() {
//		time.Sleep(50 * time.Millisecond)
//	}
//	server.Stop()
//
// # Overriding the library location
//
// The default #cgo directives below resolve wish_server_c.h/bison_c.h/
// rmi_c.h and libwish_server against this checkout's sibling include/ and
// build/ directories (three levels up from bindings/go/wishserver/). To
// build against a wish_server installed elsewhere, set CGO_CFLAGS/
// CGO_LDFLAGS before `go build`/`go test` -- the Go toolchain merges them
// with the #cgo directives below (the Go-native equivalent of every other
// wish binding's WISH_SERVER_LIB override):
//
//	export CGO_CFLAGS="-I/path/to/wish/include -I/path/to/wish/extern/bison/include"
//	export CGO_LDFLAGS="-L/path/to/wish/build -lwish_server -Wl,-rpath,/path/to/wish/build"
//	go build ./...
package wishserver

/*
#cgo CFLAGS: -I${SRCDIR}/../../../include -I${SRCDIR}/../../../extern/bison/include
#cgo LDFLAGS: -L${SRCDIR}/../../../build -lwish_server -Wl,-rpath,${SRCDIR}/../../../build
#include <stdlib.h>
#include "wish_server_c.h"
*/
import "C"
