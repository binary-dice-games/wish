// MIT License © 2025 Binary Dice Games
// Small C shims bridging wish_client_c.h/rmi_c.h's `void* user`/`void*
// userdata` callback context parameters to Go's runtime/cgo.Handle (an
// opaque uintptr) -- see shim.c for why these exist as real C functions
// rather than as bodies inside a Go cgo preamble comment.
#ifndef WISH_GO_SHIM_H
#define WISH_GO_SHIM_H

#include <stdint.h>

#include "wish_client_c.h"

wish_error wish_client_run_shim(
    wish_client_handle client,
    wish_session_fn session_fn,
    uintptr_t userdata);

wish_error wish_client_run_with_params_shim(
    wish_client_handle client,
    wish_session_fn session_fn,
    uintptr_t userdata,
    bison_handle connect_params);

rmi_error rmi_proxy_on_event_shim(
    rmi_proxy_handle proxy,
    bison_hash event_name,
    rmi_proxy_event_fn handler,
    uintptr_t user);

#endif /* WISH_GO_SHIM_H */
