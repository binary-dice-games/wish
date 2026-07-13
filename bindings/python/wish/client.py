"""Pythonic ``ctypes`` wrapper around ``wish_client_c.h`` -- the wish client
C ABI.

Layered on bison's own Python bindings: proxies and futures returned here
are plain ``bison.rmi.Proxy`` / ``bison.rmi.Future`` instances (``get`` /
``set`` / ``call`` / ``on_event`` all work unchanged), since wish's shared
library re-exports the bison and RMI C ABIs alongside its own.
"""

import ctypes
import json
from typing import Any, Callable, List, Optional

from . import _native as _n
from bison.rmi import Future, Proxy, _as_params

__all__ = ["WishError", "Client", "key", "list_apps"]


class WishError(RuntimeError):
    """Raised when a ``wish_*`` C API call returns a non-zero error code."""

    def __init__(self, code: int, context: str = ""):
        msg = _n.WISH_ERROR_MESSAGES.get(code, f"Unknown error {code}")
        super().__init__(f"{context}: {msg}" if context else msg)
        self.code = code


def _check(rc: int, context: str = "") -> None:
    if rc != _n.WISH_OK:
        raise WishError(rc, context)


def key(name: str) -> int:
    """Return the FNV-1a hash of *name* (identical to ``bison.key()`` /
    the C++ ``"name"_key`` literal)."""
    return _n.get_lib().wish_key(name.encode())


def list_apps() -> List[dict]:
    """List every embedded app registered by an enabled optional module (see
    ``modules/README.md``), as a list of
    ``{"name", "organization", "collection", "description", "params"}`` dicts
    -- ``params`` is a list of ``{"name", "description"}`` dicts.
    ``organization``/``collection`` are the module's location in the
    ``modules/<organization>/<collection>/<name>`` tree (empty if not
    populated); ``name`` alone is what :meth:`Client.run_app` takes.

    Mirrors ``wish client --list``. Does not require a connection: app
    registration happens at library load time, independent of any session.
    """
    lib = _n.get_lib()
    out = ctypes.c_char_p()
    _check(lib.wish_list_apps(ctypes.byref(out)), "list_apps")
    try:
        return json.loads(out.value.decode("utf-8"))
    finally:
        lib.bison_free_string(out)


class Client:
    """RAII wrapper around a ``wish_client_handle``.

    Construct via :meth:`tcp`, :meth:`stream`, :meth:`pipe`, or :meth:`term`,
    then call :meth:`run` to connect, drive the session, and disconnect --
    mirroring ``wish_client_run()``: the session callback runs on the
    library's RMI worker thread and the call blocks until it returns (or a
    concurrent :meth:`quit` unblocks a :meth:`wait` inside it).
    """

    __slots__ = ("_lib", "_handle", "_session_cb")

    def __init__(self, handle: int):
        self._lib = _n.get_lib()
        self._handle = _n.ClientHandle(handle)
        self._session_cb: Optional[ctypes._FuncPointer] = None

    @classmethod
    def tcp(cls, host: str, port: int) -> "Client":
        h = _n.get_lib().wish_client_tcp_create(host.encode(), port)
        if not h:
            raise MemoryError("wish_client_tcp_create failed")
        return cls(h)

    @classmethod
    def stream(cls, path: str) -> "Client":
        h = _n.get_lib().wish_client_stream_create(path.encode())
        if not h:
            raise MemoryError("wish_client_stream_create failed")
        return cls(h)

    @classmethod
    def pipe(cls, path: str) -> "Client":
        h = _n.get_lib().wish_client_pipe_create(path.encode())
        if not h:
            raise MemoryError("wish_client_pipe_create failed")
        return cls(h)

    @classmethod
    def term(cls) -> "Client":
        h = _n.get_lib().wish_client_term_create()
        if not h:
            raise MemoryError("wish_client_term_create failed")
        return cls(h)

    def destroy(self) -> None:
        """Free the client. Must not be called while :meth:`run` is active."""
        if self._handle:
            self._lib.wish_client_destroy(self._handle)
            self._handle = _n.ClientHandle(0)

    def __del__(self):
        try:
            self.destroy()
        except Exception:
            pass

    def last_error(self) -> str:
        """Return the last error message recorded for this client."""
        msg = self._lib.wish_last_error(self._handle)
        return msg.decode() if msg else ""

    # ── Session lifecycle ────────────────────────────────────────────────────

    def run(self, session_fn: Callable[["Client"], None], params: Any = None) -> None:
        """Connect, invoke ``session_fn(self)``, then disconnect.

        Blocks until ``session_fn`` returns. It runs on the RMI worker
        thread; call :meth:`wait` inside it to keep the session alive while
        event handlers update the UI, and end it with :meth:`quit` (typically
        from an event handler).

        ``params`` (a ``dict``, ``bison.Dynamic``, or ``None``) is forwarded
        to both the transport's connection setup and the server's connect
        handshake payload -- e.g. fields a server-side auth module inspects
        (see ``src/auth/DESIGN.md``).
        """
        errors: list = []

        def c_callback(_client_h, _userdata) -> None:
            try:
                session_fn(self)
            except Exception as exc:  # C ABI boundary: cannot propagate directly
                errors.append(exc)

        self._session_cb = _n.SessionFn(c_callback)
        try:
            with _as_params(params) as ph:
                rc = self._lib.wish_client_run_with_params(self._handle, self._session_cb, None, ph)
        finally:
            self._session_cb = None
        if errors:
            raise errors[0]
        _check(rc, "run")

    def wait(self) -> None:
        """Block until :meth:`quit` is called (from any thread)."""
        self._lib.wish_client_wait(self._handle)

    def quit(self) -> None:
        """Signal the session to end; unblocks a concurrent :meth:`wait`."""
        self._lib.wish_client_quit(self._handle)

    # ── Style ────────────────────────────────────────────────────────────────

    def set_style_preset(self, preset: str) -> None:
        """Apply a built-in style preset: "dark", "light", or "classic"."""
        _check(self._lib.wish_set_style_preset(self._handle, preset.encode()), "set_style_preset")

    def set_style_preset_async(self, preset: str) -> Future:
        out = _n.FutureHandle(0)
        _check(
            self._lib.wish_set_style_preset_async(self._handle, preset.encode(), ctypes.byref(out)),
            "set_style_preset_async",
        )
        return Future(out.value)

    # ── Template management ──────────────────────────────────────────────────

    def register_template(self, name: str, descriptor: str) -> None:
        """Register a named UI template (JSON or YAML descriptor string)."""
        _check(
            self._lib.wish_register_template(self._handle, name.encode(), descriptor.encode()),
            f"register_template({name!r})",
        )

    def register_template_async(self, name: str, descriptor: str) -> Future:
        out = _n.FutureHandle(0)
        _check(
            self._lib.wish_register_template_async(
                self._handle, name.encode(), descriptor.encode(), ctypes.byref(out)
            ),
            f"register_template_async({name!r})",
        )
        return Future(out.value)

    def instantiate_template(self, name: str, prefix: str) -> Proxy:
        """Instantiate a registered template under dot-path *prefix* and
        return a :class:`bison.rmi.Proxy` to its root."""
        h = self._lib.wish_instantiate_template(self._handle, name.encode(), prefix.encode())
        if not h:
            raise WishError(_n.WISH_ERR_EXCEPTION, f"instantiate_template({name!r}, {prefix!r})")
        return Proxy(h)

    def instantiate_template_async(self, name: str, prefix: str) -> Future:
        out = _n.FutureHandle(0)
        _check(
            self._lib.wish_instantiate_template_async(
                self._handle, name.encode(), prefix.encode(), ctypes.byref(out)
            ),
            f"instantiate_template_async({name!r}, {prefix!r})",
        )
        return Future(out.value)

    def proxy_get(self, dot_path: str) -> Proxy:
        """Resolve a dot-joined element path (see :meth:`instantiate_template`)
        to a :class:`bison.rmi.Proxy`, from the client's local proxy map."""
        h = self._lib.wish_proxy_get(self._handle, dot_path.encode())
        if not h:
            raise WishError(_n.WISH_ERR_NOT_FOUND, f"proxy_get({dot_path!r})")
        return Proxy(h)

    def release(self, prefix: str) -> None:
        """Release every proxy cached under *prefix* and its descendants."""
        _check(self._lib.wish_release(self._handle, prefix.encode()), f"release({prefix!r})")

    # ── Object instantiation ─────────────────────────────────────────────────

    def instantiate(self, klass_name: str, ns_name: str = "", params: Optional[dict] = None) -> Proxy:
        """Instantiate a remote object directly (no UI template involved).

        Unlike :meth:`instantiate_template`, the result is not merged into
        the dot-path proxy map used by :meth:`proxy_get`; the caller keeps
        and releases the returned proxy directly.
        """
        from bison import Dynamic

        ns_key = _n.get_lib().wish_key(ns_name.encode()) if ns_name else 0
        klass_key = _n.get_lib().wish_key(klass_name.encode())

        params_dyn: Optional[Dynamic] = None
        try:
            if params:
                params_dyn = Dynamic()
                for k, v in params.items():
                    params_dyn[k] = v
            params_handle = params_dyn._handle if params_dyn is not None else None
            h = self._lib.wish_instantiate(self._handle, ns_key, klass_key, params_handle)
        finally:
            if params_dyn is not None:
                params_dyn.release()
        if not h:
            raise WishError(_n.WISH_ERR_EXCEPTION, f"instantiate({klass_name!r}, {ns_name!r})")
        return Proxy(h)

    # ── Embedded apps ─────────────────────────────────────────────────────────

    def run_app(self, name: str, args: Optional[List[str]] = None) -> None:
        """Connect, run the named embedded app (see :func:`list_apps`), block
        until it signals completion, then disconnect.

        Mirrors ``wish client --run=<name> -- <args...>``. *args* are
        forwarded to the app the same way (e.g. notepad's optional startup
        file).

        *name* may be a short name (``"calculator"``) or its fully-qualified
        ``"organization/collection/name"`` form. Two different modules may
        register the same short name -- if *name* is short and ambiguous
        between more than one, this raises :class:`WishError` with code
        ``WISH_ERR_AMBIGUOUS``; use the fully-qualified name from
        :func:`list_apps` instead.
        """
        encoded = [a.encode() for a in (args or [])]
        arr = (ctypes.c_char_p * len(encoded))(*encoded)
        _check(
            self._lib.wish_run_app(self._handle, name.encode(), arr, len(encoded)),
            f"run_app({name!r})",
        )

    # ── File transfer ─────────────────────────────────────────────────────────

    def upload_file(self, name: str, data: bytes) -> None:
        """Upload a file to the server's sandboxed session resource directory."""
        _check(
            self._lib.wish_upload_file(self._handle, name.encode(), data, len(data)),
            f"upload_file({name!r})",
        )

    def download_file(self, name: str) -> bytes:
        """Download a previously uploaded file from the server."""
        out_data = ctypes.c_char_p()
        out_len = ctypes.c_size_t(0)
        _check(
            self._lib.wish_download_file(self._handle, name.encode(), ctypes.byref(out_data), ctypes.byref(out_len)),
            f"download_file({name!r})",
        )
        try:
            return ctypes.string_at(out_data, out_len.value)
        finally:
            self._lib.bison_free_string(out_data)

    # ── Logging ──────────────────────────────────────────────────────────────

    def log(self, level: str, msg: str) -> None:
        """Send a structured log message: level is "debug"/"info"/"warn"/"error"."""
        _check(self._lib.wish_log(self._handle, level.encode(), msg.encode()), "log")

    def log_debug(self, msg: str) -> None:
        _check(self._lib.wish_log_debug(self._handle, msg.encode()), "log_debug")

    def log_info(self, msg: str) -> None:
        _check(self._lib.wish_log_info(self._handle, msg.encode()), "log_info")

    def log_warn(self, msg: str) -> None:
        _check(self._lib.wish_log_warn(self._handle, msg.encode()), "log_warn")

    def log_error(self, msg: str) -> None:
        _check(self._lib.wish_log_error(self._handle, msg.encode()), "log_error")
