"""Pythonic ``ctypes`` wrapper around ``wish_client_c.h`` -- the wish client
C ABI.

Layered on bison's own Python bindings: proxies and futures returned here
are plain ``bison.rmi.Proxy`` / ``bison.rmi.Future`` instances (``get`` /
``set`` / ``call`` / ``on_event`` all work unchanged), since wish's shared
library re-exports the bison and RMI C ABIs alongside its own.
"""

import ctypes
import json
import time
from typing import Any, Callable, List, Optional, Tuple

from . import _native as _n
from bison.rmi import Future, Proxy, _as_params

__all__ = ["WishError", "Client", "key", "list_apps"]


class WishError(RuntimeError):
    """Raised when a ``wish_*`` C API call returns a non-zero error code.

    *detail*, when non-empty, is the underlying ``wish_last_error()`` text
    (e.g. the ``std::exception::what()`` of whatever C++ exception was
    caught at the ABI boundary) appended to the generic per-code message --
    without it, every failure looks like the unhelpful "Internal C++
    exception", regardless of whether the real cause was a refused TCP
    connection, a missing template, or something else entirely.
    """

    def __init__(self, code: int, context: str = "", detail: str = ""):
        msg = _n.WISH_ERROR_MESSAGES.get(code, f"Unknown error {code}")
        if detail:
            msg = f"{msg} ({detail})"
        super().__init__(f"{context}: {msg}" if context else msg)
        self.code = code


def _check(rc: int, context: str = "", last_error: Optional[Callable[[], str]] = None) -> None:
    if rc != _n.WISH_OK:
        raise WishError(rc, context, last_error() if last_error else "")


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

    Construct via :meth:`tcp`, :meth:`tls`, :meth:`stream`, :meth:`pipe`, or :meth:`term`,
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
    def tls(cls, host: str, port: int) -> "Client":
        """Create a TLS-secured TCP client (not yet connected).

        TLS trust/identity material (``ca_file``/``ca_pem``,
        ``insecure_skip_verify``, ``cert_file``/``cert_pem``,
        ``key_file``/``key_pem``, ``key_password``, ``server_name``) is
        supplied via :meth:`run`'s *connect_params*.
        """
        h = _n.get_lib().wish_client_tls_create(host.encode(), port)
        if not h:
            raise MemoryError("wish_client_tls_create failed")
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
        _check(rc, "run", self.last_error)

    def wait(self) -> None:
        """Block until :meth:`quit` is called (from any thread)."""
        self._lib.wish_client_wait(self._handle)

    def quit(self) -> None:
        """Signal the session to end; unblocks a concurrent :meth:`wait`."""
        self._lib.wish_client_quit(self._handle)

    # ── Style ────────────────────────────────────────────────────────────────

    def set_style_preset(self, preset: str) -> None:
        """Apply a built-in style preset: "dark", "light", or "classic"."""
        _check(self._lib.wish_set_style_preset(self._handle, preset.encode()), "set_style_preset", self.last_error)

    def set_style_preset_async(self, preset: str) -> Future:
        out = _n.FutureHandle(0)
        _check(
            self._lib.wish_set_style_preset_async(self._handle, preset.encode(), ctypes.byref(out)),
            "set_style_preset_async",
            self.last_error,
        )
        return Future(out.value)

    # ── Template management ──────────────────────────────────────────────────

    def register_template(self, name: str, descriptor: str) -> None:
        """Register a named UI template (JSON or YAML descriptor string)."""
        _check(
            self._lib.wish_register_template(self._handle, name.encode(), descriptor.encode()),
            f"register_template({name!r})",
            self.last_error,
        )

    def register_template_async(self, name: str, descriptor: str) -> Future:
        out = _n.FutureHandle(0)
        _check(
            self._lib.wish_register_template_async(
                self._handle, name.encode(), descriptor.encode(), ctypes.byref(out)
            ),
            f"register_template_async({name!r})",
            self.last_error,
        )
        return Future(out.value)

    def instantiate_template(self, name: str, prefix: str) -> Proxy:
        """Instantiate a registered template under dot-path *prefix* and
        return a :class:`bison.rmi.Proxy` to its root."""
        h = self._lib.wish_instantiate_template(self._handle, name.encode(), prefix.encode())
        if not h:
            raise WishError(_n.WISH_ERR_EXCEPTION, f"instantiate_template({name!r}, {prefix!r})", self.last_error())
        return Proxy(h)

    def instantiate_template_async(self, name: str, prefix: str) -> Future:
        out = _n.FutureHandle(0)
        _check(
            self._lib.wish_instantiate_template_async(
                self._handle, name.encode(), prefix.encode(), ctypes.byref(out)
            ),
            f"instantiate_template_async({name!r}, {prefix!r})",
            self.last_error,
        )
        return Future(out.value)

    def proxy_get(self, dot_path: str) -> Proxy:
        """Resolve a dot-joined element path (see :meth:`instantiate_template`)
        to a :class:`bison.rmi.Proxy`, from the client's local proxy map."""
        h = self._lib.wish_proxy_get(self._handle, dot_path.encode())
        if not h:
            raise WishError(_n.WISH_ERR_NOT_FOUND, f"proxy_get({dot_path!r})", self.last_error())
        return Proxy(h)

    def release(self, prefix: str) -> None:
        """Release every proxy cached under *prefix* and its descendants."""
        _check(self._lib.wish_release(self._handle, prefix.encode()), f"release({prefix!r})", self.last_error)

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
            raise WishError(_n.WISH_ERR_EXCEPTION, f"instantiate({klass_name!r}, {ns_name!r})", self.last_error())
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
            self.last_error,
        )

    # ── File transfer ─────────────────────────────────────────────────────────

    def upload_file(self, name: str, data: bytes) -> None:
        """Upload a file to the server's sandboxed session resource directory."""
        _check(
            self._lib.wish_upload_file(self._handle, name.encode(), data, len(data)),
            f"upload_file({name!r})",
            self.last_error,
        )

    def download_file(self, name: str) -> bytes:
        """Download a previously uploaded file from the server."""
        out_data = ctypes.c_char_p()
        out_len = ctypes.c_size_t(0)
        _check(
            self._lib.wish_download_file(self._handle, name.encode(), ctypes.byref(out_data), ctypes.byref(out_len)),
            f"download_file({name!r})",
            self.last_error,
        )
        try:
            return ctypes.string_at(out_data, out_len.value)
        finally:
            self._lib.bison_free_string(out_data)

    def upload_file_from_path(self, name: str, local_path: str) -> None:
        """Upload a file to the server, streaming it in chunks from a local
        file on disk instead of buffering the whole content in memory."""
        _check(
            self._lib.wish_upload_file_from_path(self._handle, name.encode(), local_path.encode()),
            f"upload_file_from_path({name!r}, {local_path!r})",
            self.last_error,
        )

    def download_file_to_path(self, name: str, local_path: str) -> None:
        """Download a previously uploaded file, streaming it in chunks
        directly to a local file on disk instead of buffering the whole
        content in memory."""
        _check(
            self._lib.wish_download_file_to_path(self._handle, name.encode(), local_path.encode()),
            f"download_file_to_path({name!r}, {local_path!r})",
            self.last_error,
        )

    def upload_package(self, dest_path: str, local_zip_path: str) -> None:
        """Upload a local zip archive and have the server unpack it into a
        sandboxed destination directory, e.g.
        ``upload_package("my_folder/my_package", "package.zip")`` extracts
        into ``my_folder/my_package/`` in the sandbox."""
        _check(
            self._lib.wish_upload_package_from_path(self._handle, dest_path.encode(), local_zip_path.encode()),
            f"upload_package({dest_path!r}, {local_zip_path!r})",
            self.last_error,
        )

    # ── Logging ──────────────────────────────────────────────────────────────

    def log(self, level: str, msg: str) -> None:
        """Send a structured log message: level is "debug"/"info"/"warn"/"error"."""
        _check(self._lib.wish_log(self._handle, level.encode(), msg.encode()), "log", self.last_error)

    def log_debug(self, msg: str) -> None:
        _check(self._lib.wish_log_debug(self._handle, msg.encode()), "log_debug", self.last_error)

    def log_info(self, msg: str) -> None:
        _check(self._lib.wish_log_info(self._handle, msg.encode()), "log_info", self.last_error)

    def log_warn(self, msg: str) -> None:
        _check(self._lib.wish_log_warn(self._handle, msg.encode()), "log_warn", self.last_error)

    def log_error(self, msg: str) -> None:
        _check(self._lib.wish_log_error(self._handle, msg.encode()), "log_error", self.last_error)

    # ── Automation ────────────────────────────────────────────────────────────
    #
    # Native (ABI-driven) automation: drive/introspect this session's UI over
    # the same connection used to build it -- no browser, no second client,
    # no subprocess -- see src/automation/DESIGN.md's "Native (ABI-based)
    # automation" section. Only available when the server's active renderer
    # implements it (currently only the SDL3 renderer); every method below
    # raises WishError(code=WISH_ERR_NOT_FOUND) otherwise. Mirrors the
    # browser-based wish.automation.AutomationClient's API shape (get_tree,
    # get_widget, click, type_text, drag, screenshot, wait_for, get_logs) so
    # a script can switch renderers with minimal changes.

    def get_tree(self, root: str = "") -> dict:
        """Return a tree/hit-test snapshot: ``{"request_id", "widgets": [...]}``.

        Each widget dict has ``path``, ``class``, ``rect``
        (``{"x0","y0","x1","y1"}`` or ``None`` if never rendered this frame),
        ``hovered``, ``active``, ``visible``, plus whichever of
        ``label``/``text``/``value``/``title``/``checked``/``selected``/``hint``
        the widget has. *root* restricts the snapshot to that dot-path and its
        descendants; empty (the default) returns the whole tree.
        """
        out = ctypes.c_char_p()
        _check(
            self._lib.wish_automation_get_tree(self._handle, root.encode(), ctypes.byref(out)),
            f"get_tree({root!r})",
            self.last_error,
        )
        try:
            return json.loads(out.value.decode("utf-8"))
        finally:
            self._lib.bison_free_string(out)

    def get_widgets(self, root: str = "") -> List[dict]:
        """Convenience: ``get_tree(root)["widgets"]``."""
        return self.get_tree(root)["widgets"]

    def get_widget(self, path: str) -> Optional[dict]:
        """Return one widget's current state by its exact dot-path, or
        ``None`` if no widget in the tree has that path."""
        for w in self.get_widgets(path):
            if w["path"] == path:
                return w
        return None

    def get_logs(self) -> List[dict]:
        """Return the session's buffered automation log entries (bounded, see
        ``logger::recent_logs()``): ``[{"seq", "timestamp", "level", "message"}, ...]``,
        oldest first."""
        out = ctypes.c_char_p()
        _check(self._lib.wish_automation_get_logs(self._handle, ctypes.byref(out)), "get_logs", self.last_error)
        try:
            return json.loads(out.value.decode("utf-8"))["logs"]
        finally:
            self._lib.bison_free_string(out)

    def screenshot(self) -> bytes:
        """Capture the next rendered frame and return it as PNG bytes."""
        out_data = ctypes.c_char_p()
        out_len = ctypes.c_size_t(0)
        _check(
            self._lib.wish_automation_screenshot(self._handle, ctypes.byref(out_data), ctypes.byref(out_len)),
            "screenshot",
            self.last_error,
        )
        try:
            return ctypes.string_at(out_data, out_len.value)
        finally:
            self._lib.bison_free_string(out_data)

    def _widget_center(self, path: str) -> Tuple[float, float]:
        w = self.get_widget(path)
        if w is None:
            raise WishError(_n.WISH_ERR_NOT_FOUND, f"_widget_center({path!r})", f"no widget at path {path!r}")
        rect = w.get("rect")
        if rect is None:
            raise WishError(
                _n.WISH_ERR_NOT_FOUND, f"_widget_center({path!r})", f"widget {path!r} was never rendered (rect is null)"
            )
        return ((rect["x0"] + rect["x1"]) / 2.0, (rect["y0"] + rect["y1"]) / 2.0)

    def mouse_move(self, x: float, y: float) -> None:
        """Inject a synthetic mouse-move event at window-relative (x, y)."""
        _check(self._lib.wish_automation_mouse_move(self._handle, x, y), "mouse_move", self.last_error)

    def mouse_button(self, button: int, down: bool) -> None:
        """Inject a synthetic mouse-button press/release. *button*: 0 = left,
        1 = right, 2 = middle."""
        _check(
            self._lib.wish_automation_mouse_button(self._handle, button, 1 if down else 0),
            "mouse_button",
            self.last_error,
        )

    def key_event(self, keycode: int, down: bool) -> None:
        """Inject a synthetic key press/release. *keycode* is a platform
        keycode (``SDL_Keycode`` for the SDL3 renderer)."""
        _check(
            self._lib.wish_automation_key_event(self._handle, keycode, 1 if down else 0),
            "key_event",
            self.last_error,
        )

    def text_input(self, text: str) -> None:
        """Inject synthetic text input (e.g. for typing into an InputText)."""
        _check(self._lib.wish_automation_text_input(self._handle, text.encode()), "text_input", self.last_error)

    def click(self, path: str, button: int = 0) -> None:
        """Click a widget's center: a real synthetic mouse move+down+up,
        indistinguishable from user input. Raises if *path* doesn't exist or
        was never rendered (``rect`` is ``None``)."""
        x, y = self._widget_center(path)
        self.mouse_move(x, y)
        self.mouse_button(button, True)
        self.mouse_button(button, False)

    def type_text(self, path: str, text: str) -> None:
        """Focus-click *path*, then type *text* as synthetic input."""
        self.click(path)
        self.text_input(text)

    def drag(self, from_path: str, to_path: str, steps: int = 10) -> None:
        """Real press/move/release drag between two widgets' centers -- for
        an element with a ``drag_type`` field dropped onto one with a
        matching ``drop_type`` (see ``docs/ui-elements.md``'s "Drag and
        drop" section)."""
        fx, fy = self._widget_center(from_path)
        tx, ty = self._widget_center(to_path)
        self.mouse_move(fx, fy)
        self.mouse_button(0, True)
        for i in range(1, steps + 1):
            t = i / steps
            self.mouse_move(fx + (tx - fx) * t, fy + (ty - fy) * t)
        self.mouse_button(0, False)

    def wait_for(self, predicate: Callable[["Client"], bool], timeout: float = 5.0, interval: float = 0.05) -> None:
        """Poll ``predicate(self)`` until it returns truthy, or raise
        :class:`TimeoutError` after *timeout* seconds.

        Mirrors the browser automation's ``wait_for()``, adapted for a native
        connection: there's no JS engine to evaluate a string predicate
        against, so *predicate* is a plain Python callable -- typically
        calling :meth:`get_tree`/:meth:`get_widget` and checking a field.
        """
        deadline = time.monotonic() + timeout
        while True:
            if predicate(self):
                return
            if time.monotonic() >= deadline:
                raise TimeoutError(f"wait_for() timed out after {timeout}s")
            time.sleep(interval)
