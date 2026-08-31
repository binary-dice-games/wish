// MIT License © 2025 Binary Dice Games

// Package wish: this file is the safe, idiomatic Go wrapper around the
// subset of bison_c.h this package needs -- the Go analogue of
// bindings/cpp/include/wish_cpp/value.hpp's `value` class and
// bindings/rust/src/value.rs's `Value`.
//
// bdg::bison::dynamic (the native, linked C++ value type) is compiled/
// linked library code, not ABI-reachable in a form this package can bind
// against directly. Like wish_cpp::value, [Value] is a thin,
// self-contained wrapper built directly on the bison_* C ABI functions
// re-exported by libwish_client/wish_client.dll -- no separate bison_abi
// library or class-registry support is needed, since the wish client never
// registers classes or methods, only builds/reads field payloads for
// template/proxy calls.
package wish

/*
#include <stdlib.h>
#include "wish_client_c.h"
*/
import "C"

import (
	"fmt"
	"runtime"
	"sync"
	"unsafe"
)

// ─── Name hashing ───────────────────────────────────────────────────────────

// keyCacheMax bounds the size of the Key() memoization cache -- see
// extern/bison/bindings/go/bison/dynamic.go's identical Key()/keyCacheMax
// for the rationale (field/method/template names are drawn from a small,
// static set reused across many calls).
const keyCacheMax = 4096

var (
	keyCacheMu sync.Mutex
	keyCache   = make(map[string]uint32)
)

// Key returns the 32-bit FNV-1a hash of name (identical to `"name"_key` in
// C++ and `wish.key(name)` in Python).
func Key(name string) uint32 {
	keyCacheMu.Lock()
	if h, ok := keyCache[name]; ok {
		keyCacheMu.Unlock()
		return h
	}
	keyCacheMu.Unlock()

	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))
	hash := uint32(C.wish_key(cName))

	keyCacheMu.Lock()
	if len(keyCache) < keyCacheMax {
		keyCache[name] = hash
	}
	keyCacheMu.Unlock()
	return hash
}

func cKey(name string) C.bison_hash {
	return C.bison_hash(Key(name))
}

func cKeyOrZero(name string) C.bison_hash {
	if name == "" {
		return 0
	}
	return cKey(name)
}

// ─── Errors ─────────────────────────────────────────────────────────────────

// bison_error codes (see bison_c.h), exported as typed constants so callers
// can compare a *BisonError's Code field directly.
const (
	BisonErrNull      int32 = -1
	BisonErrType      int32 = -2
	BisonErrNotFound  int32 = -3
	BisonErrDuplicate int32 = -4
	BisonErrException int32 = -5
	BisonErrParse     int32 = -6
)

var bisonErrorMessages = map[int32]string{
	BisonErrNull:      "null handle or pointer",
	BisonErrType:      "field holds a different type than requested",
	BisonErrNotFound:  "method or field not found",
	BisonErrDuplicate: "duplicate class or method",
	BisonErrException: "internal C++ exception",
	BisonErrParse:     "input string failed to parse (JSON / YAML)",
}

func bisonErrorMessage(code int32) string {
	if msg, ok := bisonErrorMessages[code]; ok {
		return msg
	}
	return fmt.Sprintf("unknown error %d", code)
}

// BisonError is returned when a bison_* C API call reports a non-zero error
// code. Code is the raw bison_error value (see bison_c.h).
type BisonError struct {
	Code    int32
	message string
}

func (e *BisonError) Error() string { return e.message }

func newBisonError(code int32, context string) *BisonError {
	msg := bisonErrorMessage(code)
	if context != "" {
		msg = context + ": " + msg
	}
	return &BisonError{Code: code, message: msg}
}

func checkBison(rc C.bison_error, context string) error {
	if rc == C.BISON_OK {
		return nil
	}
	return newBisonError(int32(rc), context)
}

// ─── Value ──────────────────────────────────────────────────────────────────

// Value wraps a bison_handle -- a reference-counted map/array of typed
// fields, used for proxy Set()/Get()/Call() payloads, event parameters, and
// connect params. Close releases the underlying handle; a
// runtime.SetFinalizer safety net also calls Close if a caller forgets, but
// its timing is not guaranteed, so Close (typically via defer) is the
// primary pattern.
type Value struct {
	handle   C.bison_handle
	released bool
}

func newValueOwned(h C.bison_handle) *Value {
	v := &Value{handle: h}
	runtime.SetFinalizer(v, (*Value).finalize)
	return v
}

func (v *Value) finalize() { _ = v.Close() }

// NewValue creates a new, empty object (mirrors `wish::value{}`).
func NewValue() (*Value, error) {
	h := C.bison_create(0)
	if h == nil {
		return nil, fmt.Errorf("wish: bison_create failed")
	}
	return newValueOwned(h), nil
}

// Close releases the underlying handle. Safe to call multiple times.
func (v *Value) Close() error {
	if !v.released && v.handle != nil {
		v.released = true
		C.bison_release(v.handle)
	}
	return nil
}

func (v *Value) rawHandle() C.bison_handle {
	if v == nil {
		return nil
	}
	return v.handle
}

// Clone performs a deep clone (bison_clone): nested Value fields are
// recursively cloned too, so the result shares no mutable state with v.
func (v *Value) Clone() (*Value, error) {
	h := C.bison_clone(v.handle)
	if h == nil {
		return nil, fmt.Errorf("wish: bison_clone failed")
	}
	return newValueOwned(h), nil
}

// ── Import / export ─────────────────────────────────────────────────────

// ParseJSON parses a JSON string and returns the root object.
func ParseJSON(json string) (*Value, error) {
	cJSON := C.CString(json)
	defer C.free(unsafe.Pointer(cJSON))
	h := C.bison_from_json(cJSON)
	if h == nil {
		return nil, fmt.Errorf("wish: parse_json: invalid or unsupported JSON")
	}
	return newValueOwned(h), nil
}

// ParseYAML parses a YAML string and returns the root object.
func ParseYAML(yaml string) (*Value, error) {
	cYAML := C.CString(yaml)
	defer C.free(unsafe.Pointer(cYAML))
	h := C.bison_from_yaml(cYAML)
	if h == nil {
		return nil, fmt.Errorf("wish: parse_yaml: invalid or unsupported YAML")
	}
	return newValueOwned(h), nil
}

// ToJSON serializes to a JSON string. Pass indent = -1 for compact output.
func (v *Value) ToJSON(indent int) (string, error) {
	var out *C.char
	rc := C.bison_to_json(v.handle, C.int(indent), &out)
	if err := checkBison(rc, "to_json"); err != nil {
		return "", err
	}
	defer C.bison_free_string(out)
	return C.GoString(out), nil
}

// ToYAML serializes to a YAML string.
func (v *Value) ToYAML() (string, error) {
	var out *C.char
	rc := C.bison_to_yaml(v.handle, &out)
	if err := checkBison(rc, "to_yaml"); err != nil {
		return "", err
	}
	defer C.bison_free_string(out)
	return C.GoString(out), nil
}

// ── Scalar field setters (named) ────────────────────────────────────────

func (v *Value) SetInt(name string, value int32) error {
	rc := C.bison_set_int(v.handle, cKey(name), C.int32_t(value))
	return checkBison(rc, fmt.Sprintf("set_int[%s]", name))
}

func (v *Value) SetFloat(name string, value float32) error {
	rc := C.bison_set_float(v.handle, cKey(name), C.float(value))
	return checkBison(rc, fmt.Sprintf("set_float[%s]", name))
}

func (v *Value) SetBool(name string, value bool) error {
	var cv C.int
	if value {
		cv = 1
	}
	rc := C.bison_set_bool(v.handle, cKey(name), cv)
	return checkBison(rc, fmt.Sprintf("set_bool[%s]", name))
}

func (v *Value) SetString(name string, value string) error {
	cValue := C.CString(value)
	defer C.free(unsafe.Pointer(cValue))
	rc := C.bison_set_string(v.handle, cKey(name), cValue)
	return checkBison(rc, fmt.Sprintf("set_string[%s]", name))
}

// SetObject sets a nested object field by name. The library increments
// value's ref-count, so value remains independently owned by the caller;
// pass nil to set a null object reference.
func (v *Value) SetObject(name string, value *Value) error {
	rc := C.bison_set_object(v.handle, cKey(name), value.rawHandle())
	return checkBison(rc, fmt.Sprintf("set_object[%s]", name))
}

// ── Scalar field getters (named) ────────────────────────────────────────

func (v *Value) GetInt(name string) (int32, error) {
	var out C.int32_t
	rc := C.bison_get_int(v.handle, cKey(name), &out)
	if err := checkBison(rc, fmt.Sprintf("get_int[%s]", name)); err != nil {
		return 0, err
	}
	return int32(out), nil
}

func (v *Value) GetFloat(name string) (float32, error) {
	var out C.float
	rc := C.bison_get_float(v.handle, cKey(name), &out)
	if err := checkBison(rc, fmt.Sprintf("get_float[%s]", name)); err != nil {
		return 0, err
	}
	return float32(out), nil
}

func (v *Value) GetBool(name string) (bool, error) {
	var out C.int
	rc := C.bison_get_bool(v.handle, cKey(name), &out)
	if err := checkBison(rc, fmt.Sprintf("get_bool[%s]", name)); err != nil {
		return false, err
	}
	return out != 0, nil
}

func (v *Value) GetString(name string) (string, error) {
	k := cKey(name)
	var lenOut C.size_t
	rc := C.bison_get_string(v.handle, k, nil, 0, &lenOut)
	if err := checkBison(rc, fmt.Sprintf("get_string[%s]", name)); err != nil {
		return "", err
	}
	if lenOut == 0 {
		return "", nil
	}
	buf := make([]byte, int(lenOut)+1)
	rc2 := C.bison_get_string(v.handle, k, (*C.char)(unsafe.Pointer(&buf[0])), C.size_t(len(buf)), nil)
	if err := checkBison(rc2, fmt.Sprintf("get_string[%s]", name)); err != nil {
		return "", err
	}
	return string(buf[:lenOut]), nil
}

// GetObject reads a nested object field by name. A nil *Value with a nil
// error indicates a null object reference (distinct from a not-found/type
// error).
func (v *Value) GetObject(name string) (*Value, error) {
	var out C.bison_handle
	rc := C.bison_get_object(v.handle, cKey(name), &out)
	if err := checkBison(rc, fmt.Sprintf("get_object[%s]", name)); err != nil {
		return nil, err
	}
	if out == nil {
		return nil, nil
	}
	return newValueOwned(out), nil
}

// ── Scalar field access (indexed) ───────────────────────────────────────

func (v *Value) SetIntAt(index int, value int32) error {
	rc := C.bison_set_int_at(v.handle, C.size_t(index), C.int32_t(value))
	return checkBison(rc, fmt.Sprintf("set_int_at[%d]", index))
}

func (v *Value) SetFloatAt(index int, value float32) error {
	rc := C.bison_set_float_at(v.handle, C.size_t(index), C.float(value))
	return checkBison(rc, fmt.Sprintf("set_float_at[%d]", index))
}

func (v *Value) SetStringAt(index int, value string) error {
	cValue := C.CString(value)
	defer C.free(unsafe.Pointer(cValue))
	rc := C.bison_set_string_at(v.handle, C.size_t(index), cValue)
	return checkBison(rc, fmt.Sprintf("set_string_at[%d]", index))
}

func (v *Value) GetIntAt(index int) (int32, error) {
	var out C.int32_t
	rc := C.bison_get_int_at(v.handle, C.size_t(index), &out)
	if err := checkBison(rc, fmt.Sprintf("get_int_at[%d]", index)); err != nil {
		return 0, err
	}
	return int32(out), nil
}

func (v *Value) GetFloatAt(index int) (float32, error) {
	var out C.float
	rc := C.bison_get_float_at(v.handle, C.size_t(index), &out)
	if err := checkBison(rc, fmt.Sprintf("get_float_at[%d]", index)); err != nil {
		return 0, err
	}
	return float32(out), nil
}

func (v *Value) GetStringAt(index int) (string, error) {
	var lenOut C.size_t
	rc := C.bison_get_string_at(v.handle, C.size_t(index), nil, 0, &lenOut)
	if err := checkBison(rc, fmt.Sprintf("get_string_at[%d]", index)); err != nil {
		return "", err
	}
	if lenOut == 0 {
		return "", nil
	}
	buf := make([]byte, int(lenOut)+1)
	rc2 := C.bison_get_string_at(v.handle, C.size_t(index), (*C.char)(unsafe.Pointer(&buf[0])), C.size_t(len(buf)), nil)
	if err := checkBison(rc2, fmt.Sprintf("get_string_at[%d]", index)); err != nil {
		return "", err
	}
	return string(buf[:lenOut]), nil
}

// Size returns the number of array-like (numeric-key) elements.
func (v *Value) Size() int {
	return int(C.bison_size(v.handle))
}
