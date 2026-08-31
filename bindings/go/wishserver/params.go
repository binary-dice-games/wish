// MIT License © 2025 Binary Dice Games

// Package wishserver: this file is [Params] -- a small flat
// string/int/bool/float map that lowers to the bison_handle
// [Server.Start] forwards to wish_server_start as its renderer/listen
// params.
//
// It is the server package's stand-in for package wish's richer Value:
// wish_server_start's params are always a flat map of scalars (title,
// width, web_port, TLS cert_file/key_file, ...), so a full bison::dynamic
// wrapper would be overkill -- and, more importantly, would have to be
// built against *this* library's embedded bison_* functions anyway (a
// bison_handle is only valid against the exact library that created it,
// and wish_server_start decodes it with libwish_server's copy). Mirrors
// bindings/python/wish/_server_native.py's build_params, C#'s
// ServerParamsScope, and bindings/rust/wish-server's Params.
package wishserver

/*
#include <stdlib.h>
#include "wish_server_c.h"
*/
import "C"

import (
	"runtime"
	"unsafe"
)

// Params builds [Server.Start]'s params argument. Construct with
// [NewParams]; the setters return the receiver so calls can be chained.
// Close releases the underlying handle; a runtime.SetFinalizer safety net
// also calls it if a caller forgets, but [Server.Start] does not consume
// the Params, so an explicit Close (typically via defer) is the primary
// pattern.
type Params struct {
	handle   C.bison_handle
	released bool
}

// NewParams creates a new, empty parameter map.
func NewParams() *Params {
	h := C.bison_create(0)
	if h == nil {
		panic("wishserver: bison_create failed")
	}
	p := &Params{handle: h}
	runtime.SetFinalizer(p, (*Params).finalize)
	return p
}

func (p *Params) finalize() { p.Close() }

// Close releases the underlying handle. Safe to call multiple times.
func (p *Params) Close() {
	if p != nil && !p.released && p.handle != nil {
		p.released = true
		C.bison_release(p.handle)
	}
}

func (p *Params) rawHandle() C.bison_handle {
	if p == nil {
		return nil
	}
	return p.handle
}

func (p *Params) key(name string) C.bison_hash {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))
	return C.bison_key(cName)
}

// SetString sets a string field and returns p for chaining.
func (p *Params) SetString(name, value string) *Params {
	cValue := C.CString(value)
	defer C.free(unsafe.Pointer(cValue))
	C.bison_set_string(p.handle, p.key(name), cValue)
	return p
}

// SetInt sets an integer field and returns p for chaining.
func (p *Params) SetInt(name string, value int32) *Params {
	C.bison_set_int(p.handle, p.key(name), C.int32_t(value))
	return p
}

// SetFloat sets a float field and returns p for chaining.
func (p *Params) SetFloat(name string, value float32) *Params {
	C.bison_set_float(p.handle, p.key(name), C.float(value))
	return p
}

// SetBool sets a boolean field and returns p for chaining.
func (p *Params) SetBool(name string, value bool) *Params {
	var cv C.int
	if value {
		cv = 1
	}
	C.bison_set_bool(p.handle, p.key(name), cv)
	return p
}
