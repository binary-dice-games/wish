"""Pythonic ``ctypes`` bindings for the wish client C ABI (``wish_client_c.h``).

Layered on bison's own Python bindings (vendored at
``extern/bison/bindings/python``): wish's shared library bundles the bison
and RMI C ABIs together with wish's own client entry points, so proxies and
futures returned from :class:`Client` are plain ``bison.rmi.Proxy`` /
``bison.rmi.Future`` instances -- see :mod:`bison.rmi` for their API.

Quick start::

    from wish import Client

    def session(client):
        client.set_style_preset("dark")
        client.register_template("ui", '{"type": "Window", "title": "Hi"}')
        root = client.instantiate_template("ui", "ui")
        print(root["title"])   # "Hi"
        client.wait()          # blocks until an event handler calls client.quit()

    Client.tcp("127.0.0.1", 7070).run(session)
"""

from .client import Client, WishError, key

__all__ = ["Client", "WishError", "key"]
