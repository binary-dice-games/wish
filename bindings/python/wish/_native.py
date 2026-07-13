"""ctypes bindings for the ``wish_client`` shared library (``wish_client_c.h``).

The wish ABI is layered directly on bison's: ``libwish_client.so`` embeds
``bison_c.h`` and ``rmi_c.h`` alongside ``wish_client_c.h`` in a single shared
object (see the ``wish_client_dll`` CMake target), so this module locates
that one library and reuses :func:`bison._native._setup_signatures` to bind
every ``bison_*``/``rmi_*`` entry point, then layers the ``wish_*`` entry
points on top of the same handle. This is also why ``bison.Dynamic``,
``bison.rmi.Proxy``, and ``bison.rmi.Future`` work unmodified against wish's
``bison_handle`` / ``rmi_proxy_handle`` / ``rmi_future_handle`` values -- they
are the exact same C ABI types, exported from the exact same library.
"""

import ctypes
import os
import sys
import threading
from typing import Optional

# Make the sibling `bison` package importable regardless of CWD: it lives at
# extern/bison/bindings/python relative to the wish repo root.
_here = os.path.dirname(os.path.abspath(__file__))
_wish_repo_root = os.path.dirname(os.path.dirname(os.path.dirname(_here)))  # bindings/python/wish -> repo root
_bison_bindings = os.path.join(_wish_repo_root, "extern", "bison", "bindings", "python")
if os.path.isdir(_bison_bindings) and _bison_bindings not in sys.path:
    sys.path.insert(0, _bison_bindings)

from bison import _native as _bison_native  # noqa: E402

# ─── Shared C type aliases ─────────────────────────────────────────────────

ClientHandle = ctypes.c_void_p  # wish_client_handle
Hash = ctypes.c_uint32  # wish_hash / bison_hash
Error = ctypes.c_int  # wish_error

# rmi_proxy_handle / rmi_future_handle are bison types; wish_client_c.h
# reuses them verbatim, so reuse bison's own ctypes aliases rather than
# redefining them.
ProxyHandle = _bison_native.ProxyHandle
FutureHandle = _bison_native.FutureHandle

# ─── wish_error codes ───────────────────────────────────────────────────────

WISH_OK = 0
WISH_ERR_NULL = -1
WISH_ERR_NOT_FOUND = -2
WISH_ERR_TRANSPORT = -3
WISH_ERR_EXCEPTION = -4
WISH_ERR_AMBIGUOUS = -5

WISH_ERROR_MESSAGES = {
    WISH_ERR_NULL: "Null handle or pointer",
    WISH_ERR_NOT_FOUND: "Named proxy or resource not found",
    WISH_ERR_TRANSPORT: "Transport connection failed",
    WISH_ERR_EXCEPTION: "Internal C++ exception",
    WISH_ERR_AMBIGUOUS: "App name matches more than one registered app; use the fully-qualified name (see last_error())",
}

# ─── Callback types ─────────────────────────────────────────────────────────

# void (*wish_session_fn)(wish_client_handle client, void* userdata)
SessionFn = ctypes.CFUNCTYPE(None, ClientHandle, ctypes.c_void_p)

# ─── Library loading ────────────────────────────────────────────────────────


def _find_library() -> str:
    """Locate ``libwish_client`` via ``WISH_LIB``, the ``build/`` dir, or the
    system library search path."""
    env_path = os.environ.get("WISH_LIB")
    if env_path:
        return env_path

    # Layout: <repo>/bindings/python/wish/_native.py -> <repo>/build/...
    candidates = [
        os.path.join(_wish_repo_root, "build", "libwish_client.so"),
        os.path.join(_wish_repo_root, "build", "libwish_client.dylib"),
        os.path.join(_wish_repo_root, "build", "Release", "wish_client.dll"),
        os.path.join(_wish_repo_root, "build", "Debug", "wish_client.dll"),
        os.path.join(_wish_repo_root, "build", "wish_client.dll"),
    ]
    for path in candidates:
        if os.path.isfile(path):
            return path

    raise OSError(
        "libwish_client not found. Build it first "
        "(cmake --build build --target wish_client_dll) and/or set the WISH_LIB "
        "environment variable to the full path of "
        "libwish_client.so/.dylib/wish_client.dll."
    )


_lib_lock = threading.Lock()
_lib: Optional[ctypes.CDLL] = None


def get_lib() -> ctypes.CDLL:
    """Return the loaded ``wish_client`` library (singleton, thread-safe).

    Also adopted as bison's own library (via ``BISON_LIB``) so that
    ``bison._native.get_lib()`` -- and therefore ``bison.Dynamic`` /
    ``bison.rmi.Proxy`` / ``bison.rmi.Future`` -- resolve against this exact
    same shared object; see the module docstring.
    """
    global _lib
    if _lib is None:
        with _lib_lock:
            if _lib is None:
                path = _find_library()
                os.environ.setdefault("BISON_LIB", path)
                lib = _bison_native.get_lib()  # binds every bison_*/rmi_* signature
                _setup_wish_signatures(lib)
                _lib = lib
    return _lib


def _setup_wish_signatures(lib: ctypes.CDLL) -> None:
    P = ctypes.POINTER

    lib.wish_key.restype = Hash
    lib.wish_key.argtypes = [ctypes.c_char_p]

    # ── Client lifecycle ────────────────────────────────────────────────────
    lib.wish_client_tcp_create.restype = ClientHandle
    lib.wish_client_tcp_create.argtypes = [ctypes.c_char_p, ctypes.c_uint16]

    lib.wish_client_stream_create.restype = ClientHandle
    lib.wish_client_stream_create.argtypes = [ctypes.c_char_p]

    lib.wish_client_pipe_create.restype = ClientHandle
    lib.wish_client_pipe_create.argtypes = [ctypes.c_char_p]

    lib.wish_client_term_create.restype = ClientHandle
    lib.wish_client_term_create.argtypes = []

    lib.wish_client_destroy.restype = None
    lib.wish_client_destroy.argtypes = [ClientHandle]

    lib.wish_client_run.restype = Error
    lib.wish_client_run.argtypes = [ClientHandle, SessionFn, ctypes.c_void_p]

    lib.wish_client_run_with_params.restype = Error
    lib.wish_client_run_with_params.argtypes = [ClientHandle, SessionFn, ctypes.c_void_p, _bison_native.Handle]

    lib.wish_client_wait.restype = None
    lib.wish_client_wait.argtypes = [ClientHandle]

    lib.wish_client_quit.restype = None
    lib.wish_client_quit.argtypes = [ClientHandle]

    lib.wish_last_error.restype = ctypes.c_char_p
    lib.wish_last_error.argtypes = [ClientHandle]

    # ── Style ────────────────────────────────────────────────────────────────
    lib.wish_set_style_preset.restype = Error
    lib.wish_set_style_preset.argtypes = [ClientHandle, ctypes.c_char_p]

    lib.wish_set_style_preset_async.restype = Error
    lib.wish_set_style_preset_async.argtypes = [ClientHandle, ctypes.c_char_p, P(FutureHandle)]

    # ── Template management ─────────────────────────────────────────────────
    lib.wish_register_template.restype = Error
    lib.wish_register_template.argtypes = [ClientHandle, ctypes.c_char_p, ctypes.c_char_p]

    lib.wish_register_template_async.restype = Error
    lib.wish_register_template_async.argtypes = [ClientHandle, ctypes.c_char_p, ctypes.c_char_p, P(FutureHandle)]

    lib.wish_instantiate_template.restype = ProxyHandle
    lib.wish_instantiate_template.argtypes = [ClientHandle, ctypes.c_char_p, ctypes.c_char_p]

    lib.wish_instantiate_template_async.restype = Error
    lib.wish_instantiate_template_async.argtypes = [ClientHandle, ctypes.c_char_p, ctypes.c_char_p, P(FutureHandle)]

    lib.wish_proxy_get.restype = ProxyHandle
    lib.wish_proxy_get.argtypes = [ClientHandle, ctypes.c_char_p]

    lib.wish_release.restype = Error
    lib.wish_release.argtypes = [ClientHandle, ctypes.c_char_p]

    # ── Object instantiation ────────────────────────────────────────────────
    lib.wish_instantiate.restype = ProxyHandle
    lib.wish_instantiate.argtypes = [ClientHandle, Hash, Hash, _bison_native.Handle]

    # ── Embedded apps ────────────────────────────────────────────────────────
    lib.wish_list_apps.restype = Error
    lib.wish_list_apps.argtypes = [P(ctypes.c_char_p)]

    lib.wish_run_app.restype = Error
    lib.wish_run_app.argtypes = [ClientHandle, ctypes.c_char_p, P(ctypes.c_char_p), ctypes.c_size_t]

    # ── File transfer ────────────────────────────────────────────────────────
    lib.wish_upload_file.restype = Error
    lib.wish_upload_file.argtypes = [ClientHandle, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]

    lib.wish_download_file.restype = Error
    lib.wish_download_file.argtypes = [ClientHandle, ctypes.c_char_p, P(ctypes.c_char_p), P(ctypes.c_size_t)]

    # ── Logging ──────────────────────────────────────────────────────────────
    lib.wish_log.restype = Error
    lib.wish_log.argtypes = [ClientHandle, ctypes.c_char_p, ctypes.c_char_p]

    lib.wish_log_debug.restype = Error
    lib.wish_log_debug.argtypes = [ClientHandle, ctypes.c_char_p]

    lib.wish_log_info.restype = Error
    lib.wish_log_info.argtypes = [ClientHandle, ctypes.c_char_p]

    lib.wish_log_warn.restype = Error
    lib.wish_log_warn.argtypes = [ClientHandle, ctypes.c_char_p]

    lib.wish_log_error.restype = Error
    lib.wish_log_error.argtypes = [ClientHandle, ctypes.c_char_p]
