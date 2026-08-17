"""Pythonic ``ctypes`` wrapper around ``wish_server_c.h`` -- host and render
a real wish session from Python.

Unlike a server built from bison's generic RMI primitives
(``bison.rmi.Server`` + ``bison.add_class``), this wraps the actual
``bdg::wish::server`` C++ implementation (the same one the ``wish server``
CLI uses): template registration/instantiation gives each widget its own
independently addressable proxy, and events work.

Quick start::

    from wish import Server

    server = Server.tcp("127.0.0.1", 7070)
    server.start(renderer="sdl3")   # opens a real SDL3 window
    input("Press Enter to stop...\\n")
    server.stop()
"""

from typing import Any

from . import _server_native as _n

__all__ = ["Server"]


class Server:
    """RAII wrapper around a ``wish_server_handle``.

    Construct via :meth:`tcp` or :meth:`pipe`, then :meth:`start` to build
    the requested renderer and begin accepting client connections. Use as a
    context manager to auto-:meth:`stop` on exit.
    """

    __slots__ = ("_lib", "_handle", "_started")

    def __init__(self, handle: int):
        self._lib = _n.get_lib()
        self._handle = _n.ServerHandle(handle)
        self._started = False

    @classmethod
    def tcp(cls, host: str, port: int) -> "Server":
        h = _n.get_lib().wish_server_tcp_create(host.encode(), port)
        if not h:
            raise MemoryError("wish_server_tcp_create failed")
        return cls(h)

    @classmethod
    def pipe(cls, path: str) -> "Server":
        h = _n.get_lib().wish_server_pipe_create(path.encode())
        if not h:
            raise MemoryError("wish_server_pipe_create failed")
        return cls(h)

    @classmethod
    def tls(cls, host: str, port: int) -> "Server":
        """Create a TLS-secured TCP server (not yet listening).

        TLS material (``cert_file``/``cert_pem``, ``key_file``/``key_pem``,
        ``key_password``, and optionally ``client_auth``/``ca_file``/
        ``ca_pem`` for mutual TLS) is supplied via :meth:`start`'s
        ``**params``.
        """
        h = _n.get_lib().wish_server_tls_create(host.encode(), port)
        if not h:
            raise MemoryError("wish_server_tls_create failed")
        return cls(h)

    @classmethod
    def term(cls, cmd: str = "") -> "Server":
        """Create a terminal (OSC-99 framed) server by spawning a child
        process attached to a new pseudo-terminal.

        The spawned child is expected to be a wish client process using
        ``wish.Client.term()`` (or an equivalent term-transport client) over
        its own inherited stdio. :meth:`should_quit` also returns ``True``
        once the spawned child exits, in addition to any renderer close
        signal.

        :param cmd: Command to exec in the child. Empty (default) spawns the
            operator's ``$SHELL``/``cmd.exe``.
        """
        h = _n.get_lib().wish_server_term_create(cmd.encode() if cmd else None)
        if not h:
            raise MemoryError("wish_server_term_create failed")
        return cls(h)

    def set_verbose(self, verbose: bool = True) -> "Server":
        """Enable trace logging of RMI dispatch to stdout. Must be called
        before :meth:`start`."""
        self._check(self._lib.wish_server_set_verbose(self._handle, 1 if verbose else 0), "set_verbose")
        return self

    def start(self, renderer: str = "sdl3", **params: Any) -> "Server":
        """Build *renderer* and begin accepting client connections.

        :param renderer: ``"sdl3"`` (a real window), ``"web"`` (pass
            ``web_bind``/``web_port``; open the printed URL in a browser),
            or ``"console"`` (a lightweight text dump of the widget tree to
            stdout -- no display needed, meant for tests/CI).
        :param params: Renderer-specific keyword arguments, all optional:
            ``title``, ``width``, ``height``, ``font_size`` for
            ``"sdl3"``/``"web"``; ``web_bind``, ``web_port`` for ``"web"``
            only. Matches the ``wish server`` CLI's own flags/defaults. Also
            forwarded unchanged to the transport's own listen params -- e.g.
            ``cert_file``/``key_file``/etc. for a server created with
            :meth:`tls`; ignored by every other transport.
        """
        ph = _n.build_params(self._lib, params) if params else None
        try:
            self._check(self._lib.wish_server_start(self._handle, renderer.encode(), ph), "start")
        finally:
            _n.release_params(self._lib, ph)
        self._started = True
        return self

    def stop(self) -> None:
        if self._started:
            self._check(self._lib.wish_server_stop(self._handle), "stop")
            self._started = False

    def should_quit(self) -> bool:
        """Returns True once the renderer signals it should close (e.g. the
        SDL3 window was closed). The web/console renderers never set this on
        their own."""
        return bool(self._lib.wish_server_should_quit(self._handle))

    def release(self) -> None:
        if self._handle:
            self._lib.wish_server_destroy(self._handle)
            self._handle = _n.ServerHandle(0)

    def __enter__(self) -> "Server":
        return self

    def __exit__(self, *_exc) -> None:
        self.stop()
        self.release()

    def __del__(self):
        try:
            self.release()
        except Exception:
            pass

    def _check(self, rc: int, context: str = "") -> None:
        if rc != _n.WISH_SERVER_OK:
            detail = self._lib.wish_server_last_error(self._handle)
            detail_str = detail.decode("utf-8", "replace") if detail else ""
            msg = _n.WISH_SERVER_ERROR_MESSAGES.get(rc, f"Unknown error {rc}")
            if detail_str:
                msg = f"{msg} ({detail_str})"
            raise RuntimeError(f"{context}: {msg}" if context else msg)
