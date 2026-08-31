"""ctypes bindings for the ``wish_server`` shared library (``wish_server_c.h``).

A *separate* shared library from ``wish_client_dll`` (see ``_native.py``):
``libwish_server.so``/``wish_server.dll`` hosts and renders a real wish
session (SDL3 window, the web/browser renderer, or a lightweight text
``console`` dump), over the real ``bdg::wish::server`` C++ implementation --
not the generic bison RMI server primitives ``bison.rmi.Server`` wraps,
which can't mint independently-addressable widget proxies or emit events
from outside C++. Only imported by :mod:`wish.server`; :mod:`wish.client`
has no dependency on it.

This module deliberately does **not** route ``bison_handle`` construction
through ``bison._native``/``bison.rmi``: that module is a process-wide
*singleton* (one ``ctypes.CDLL`` for the whole process), and a
``bison_handle`` is only valid against the exact shared library that
created it. If a process uses both :mod:`wish.client` (which redirects
``bison._native``'s singleton onto ``wish_client_dll``) and
:mod:`wish.server` in the same process -- e.g. a test that starts a server
and connects a client to it -- whichever redirects first would silently
win, leaving the other's ``Dynamic``/``Proxy`` objects bound to the wrong
library. Instead, :func:`build_params`/:func:`release_params` below call
``bison_create``/``bison_set_*``/``bison_release`` directly through *this*
module's own loaded library (embedded the same way ``wish_client_dll``
embeds them -- see ``src/wish_server_c.cpp``), fully independent of
``bison._native``'s state.
"""

import ctypes
import ctypes.util
import os
import sys
import threading
from typing import Any, Optional

# ─── Shared C type aliases ─────────────────────────────────────────────────

ServerHandle = ctypes.c_void_p  # wish_server_handle
Handle = ctypes.c_void_p  # bison_handle
Hash = ctypes.c_uint32  # bison_hash
Error = ctypes.c_int  # wish_server_error / bison_error

# ─── wish_server_error codes ────────────────────────────────────────────────

WISH_SERVER_OK = 0
WISH_SERVER_ERR_NULL = -1
WISH_SERVER_ERR_TRANSPORT = -3
WISH_SERVER_ERR_EXCEPTION = -4
WISH_SERVER_ERR_BAD_RENDERER = -6

WISH_SERVER_ERROR_MESSAGES = {
    WISH_SERVER_ERR_NULL: "Null handle or pointer",
    WISH_SERVER_ERR_TRANSPORT: "Transport listen failed",
    WISH_SERVER_ERR_EXCEPTION: "Internal C++ exception",
    WISH_SERVER_ERR_BAD_RENDERER: "Unknown renderer_kind, or this library wasn't built with support for it",
}

# ─── Library loading ────────────────────────────────────────────────────────


def _find_library() -> str:
    """Locate ``libwish_server`` via ``WISH_SERVER_LIB``, alongside this
    package (the ``pip install``ed layout), every ``sys.path`` entry's
    ``wish/`` subdirectory (``pip install -e``'s redirect editable mode),
    the ``build/`` dir, or the system library search path."""
    env_path = os.environ.get("WISH_SERVER_LIB")
    if env_path:
        return env_path

    here = os.path.dirname(os.path.abspath(__file__))
    wish_repo_root = os.path.dirname(os.path.dirname(os.path.dirname(here)))
    lib_names = ("libwish_server.so", "libwish_server.dylib", "wish_server.dll")

    installed_candidates = [os.path.join(here, name) for name in lib_names]
    sys_path_candidates = [
        os.path.join(entry, "wish", name) for entry in sys.path if entry for name in lib_names
    ]
    dev_candidates = [
        os.path.join(wish_repo_root, "build", "libwish_server.so"),
        os.path.join(wish_repo_root, "build", "libwish_server.dylib"),
        os.path.join(wish_repo_root, "build", "Release", "wish_server.dll"),
        os.path.join(wish_repo_root, "build", "Debug", "wish_server.dll"),
        os.path.join(wish_repo_root, "build", "wish_server.dll"),
    ]

    for path in installed_candidates + sys_path_candidates + dev_candidates:
        if os.path.isfile(path):
            return path

    found = ctypes.util.find_library("wish_server")
    if found:
        return found

    raise OSError(
        "libwish_server not found. Build it first "
        "(cmake -DWISH_BUILD_SERVER_SHARED=ON -B build && "
        "cmake --build build --target wish_server_dll) and/or set the "
        "WISH_SERVER_LIB environment variable to the full path of "
        "libwish_server.so/.dylib/wish_server.dll."
    )


_lib_lock = threading.Lock()
_lib: Optional[ctypes.CDLL] = None


def get_lib() -> ctypes.CDLL:
    """Return the loaded ``wish_server`` library (singleton, thread-safe).

    Unlike ``wish._native.get_lib()``, this does **not** redirect
    ``BISON_LIB``/touch ``bison._native``'s own singleton -- see the module
    docstring for why (this library's own embedded ``bison_*`` functions are
    called directly, via :func:`build_params`, instead).
    """
    global _lib
    if _lib is None:
        with _lib_lock:
            if _lib is None:
                lib = ctypes.CDLL(_find_library())
                _setup_signatures(lib)
                _lib = lib
    return _lib


def _setup_signatures(lib: ctypes.CDLL) -> None:
    lib.wish_server_tcp_create.restype = ServerHandle
    lib.wish_server_tcp_create.argtypes = [ctypes.c_char_p, ctypes.c_uint16]

    lib.wish_server_pipe_create.restype = ServerHandle
    lib.wish_server_pipe_create.argtypes = [ctypes.c_char_p]

    lib.wish_server_tls_create.restype = ServerHandle
    lib.wish_server_tls_create.argtypes = [ctypes.c_char_p, ctypes.c_uint16]

    lib.wish_server_term_create.restype = ServerHandle
    lib.wish_server_term_create.argtypes = [ctypes.c_char_p]

    lib.wish_server_start.restype = Error
    lib.wish_server_start.argtypes = [ServerHandle, ctypes.c_char_p, Handle]

    lib.wish_server_stop.restype = Error
    lib.wish_server_stop.argtypes = [ServerHandle]

    lib.wish_server_should_quit.restype = ctypes.c_int
    lib.wish_server_should_quit.argtypes = [ServerHandle]

    lib.wish_server_set_verbose.restype = Error
    lib.wish_server_set_verbose.argtypes = [ServerHandle, ctypes.c_int]

    lib.wish_server_set_log_level.restype = Error
    lib.wish_server_set_log_level.argtypes = [ServerHandle, ctypes.c_char_p]

    lib.wish_server_destroy.restype = None
    lib.wish_server_destroy.argtypes = [ServerHandle]

    lib.wish_server_last_error.restype = ctypes.c_char_p
    lib.wish_server_last_error.argtypes = [ServerHandle]

    # ── bison_* subset needed to build wish_server_start()'s `params` ──────
    # (This library embeds the full bison C ABI -- see the module docstring
    # for why these are called directly, not via bison._native.)
    lib.bison_key.restype = Hash
    lib.bison_key.argtypes = [ctypes.c_char_p]

    lib.bison_create.restype = Handle
    lib.bison_create.argtypes = [Hash]

    lib.bison_release.restype = None
    lib.bison_release.argtypes = [Handle]

    lib.bison_set_int.restype = Error
    lib.bison_set_int.argtypes = [Handle, Hash, ctypes.c_int32]

    lib.bison_set_float.restype = Error
    lib.bison_set_float.argtypes = [Handle, Hash, ctypes.c_float]

    lib.bison_set_bool.restype = Error
    lib.bison_set_bool.argtypes = [Handle, Hash, ctypes.c_int]

    lib.bison_set_string.restype = Error
    lib.bison_set_string.argtypes = [Handle, Hash, ctypes.c_char_p]


def build_params(lib: ctypes.CDLL, fields: dict) -> Handle:
    """Build a ``bison_handle`` (via *this* library's own embedded
    ``bison_*`` functions) from a flat ``{name: value}`` dict of ``str``,
    ``bool``, ``int``, or ``float`` values -- everything
    ``wish_server_start()``'s renderer params need. Release with
    :func:`release_params`."""
    h = lib.bison_create(0)
    if not h:
        raise MemoryError("bison_create failed")
    for name, value in fields.items():
        k = lib.bison_key(str(name).encode())
        if isinstance(value, bool):
            rc = lib.bison_set_bool(h, k, int(value))
        elif isinstance(value, int):
            rc = lib.bison_set_int(h, k, value)
        elif isinstance(value, float):
            rc = lib.bison_set_float(h, k, value)
        elif isinstance(value, str):
            rc = lib.bison_set_string(h, k, value.encode())
        else:
            lib.bison_release(h)
            raise TypeError(f"Unsupported params value type for {name!r}: {type(value)}")
        if rc != 0:
            lib.bison_release(h)
            raise RuntimeError(f"failed to set params field {name!r} (bison_error {rc})")
    return h


def release_params(lib: ctypes.CDLL, handle: Any) -> None:
    if handle:
        lib.bison_release(handle)
