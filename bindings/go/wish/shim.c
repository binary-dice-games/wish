// MIT License © 2025 Binary Dice Games
//
// wish_client_run/wish_client_run_with_params/rmi_proxy_on_event accept a
// raw `void*` context pointer. Go's cgo pointer-passing rules disallow
// converting a runtime/cgo.Handle (an opaque uintptr) to unsafe.Pointer
// directly on the Go side (go vet's unsafeptr check flags it), so these
// shims accept `uintptr_t` from Go and perform the uintptr_t->void* cast
// entirely on the C side instead.
//
// These live in an ordinary .c file (compiled alongside the Go sources by
// cgo automatically) rather than as function bodies inside a Go file's cgo
// preamble comment: cgo re-emits a preamble comment's *entire* text
// (definitions included, not just declarations) into the generated
// _cgo_export.c for any file that also contains `//export` trampolines,
// which would otherwise duplicate-define these functions and fail to link
// (see extern/bison/bindings/go/bison/shim.c's identical rationale).
#include "shim.h"

wish_error wish_client_run_shim(
    wish_client_handle client,
    wish_session_fn session_fn,
    uintptr_t userdata) {
  return wish_client_run(client, session_fn, (void*)userdata);
}

wish_error wish_client_run_with_params_shim(
    wish_client_handle client,
    wish_session_fn session_fn,
    uintptr_t userdata,
    bison_handle connect_params) {
  return wish_client_run_with_params(client, session_fn, (void*)userdata, connect_params);
}

rmi_error rmi_proxy_on_event_shim(
    rmi_proxy_handle proxy,
    bison_hash event_name,
    rmi_proxy_event_fn handler,
    uintptr_t user) {
  return rmi_proxy_on_event(proxy, event_name, handler, (void*)user);
}
