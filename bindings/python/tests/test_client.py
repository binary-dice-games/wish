"""Tests for wish.Client -- the wish Python binding.

Exercises only the parts that don't require a live wish server (there is no
standalone/in-process mode on the client-only wish ABI): handle
construction/destruction, key hashing, and error-code plumbing.

Build wish_client_dll first, then run from the repository root::

    cmake -B build
    cmake --build build --target wish_client_dll
    python -m pytest bindings/python/tests/test_client.py -v
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from wish import Client, WishError, key
from wish.client import _n


class TestKeyHashing(unittest.TestCase):
    def test_key_matches_bison_key(self):
        from bison import key as bison_key

        self.assertEqual(key("clicked"), bison_key("clicked"))

    def test_key_deterministic(self):
        self.assertEqual(key("hello"), key("hello"))
        self.assertNotEqual(key("hello"), key("world"))


class TestClientLifecycle(unittest.TestCase):
    def test_tcp_create_and_destroy(self):
        client = Client.tcp("127.0.0.1", 7070)
        self.assertTrue(client._handle)
        client.destroy()
        self.assertFalse(client._handle)
        # Idempotent.
        client.destroy()

    def test_tls_create_and_destroy(self):
        client = Client.tls("127.0.0.1", 7070)
        self.assertTrue(client._handle)
        client.destroy()
        self.assertFalse(client._handle)
        # Idempotent.
        client.destroy()

    def test_destroy_via_del_does_not_raise(self):
        client = Client.tcp("127.0.0.1", 7070)
        del client

    def test_last_error_empty_before_any_operation(self):
        client = Client.tcp("127.0.0.1", 7070)
        self.assertEqual(client.last_error(), "")
        client.destroy()


class TestErrorMapping(unittest.TestCase):
    def test_wish_error_message(self):
        err = WishError(_n.WISH_ERR_NOT_FOUND, "proxy_get('x')")
        self.assertIn("Named proxy or resource not found", str(err))
        self.assertEqual(err.code, _n.WISH_ERR_NOT_FOUND)


class TestRunWithConnectParams(unittest.TestCase):
    """Exercises Client.run(session_fn, params=...) -> wish_client_run_with_params
    plumbing (see src/auth/DESIGN.md). No live server is spun up here (the
    Python bindings are client-only) -- connecting to a closed port still
    proves params are marshaled through the FFI boundary without crashing;
    the full persistence round-trip through a live server is covered by the
    C++ integration tests in tests/test_auth.cpp.
    """

    def _unreachable_client(self):
        # Port 1 is a reserved/privileged port essentially never listening.
        return Client.tcp("127.0.0.1", 1)

    def test_run_with_dict_params_fails_cleanly_not_a_crash(self):
        client = self._unreachable_client()
        try:
            with self.assertRaises(WishError) as ctx:
                client.run(lambda c: None, params={"username": "alice"})
            self.assertEqual(ctx.exception.code, _n.WISH_ERR_EXCEPTION)
        finally:
            client.destroy()

    def test_run_with_no_params_still_works(self):
        client = self._unreachable_client()
        try:
            with self.assertRaises(WishError) as ctx:
                client.run(lambda c: None)
            self.assertEqual(ctx.exception.code, _n.WISH_ERR_EXCEPTION)
        finally:
            client.destroy()

    def test_run_with_dynamic_params_fails_cleanly_not_a_crash(self):
        from bison import Dynamic

        client = self._unreachable_client()
        params = Dynamic()
        params["username"] = "alice"
        try:
            with self.assertRaises(WishError) as ctx:
                client.run(lambda c: None, params=params)
            self.assertEqual(ctx.exception.code, _n.WISH_ERR_EXCEPTION)
        finally:
            params.release()
            client.destroy()


if __name__ == "__main__":
    unittest.main()
