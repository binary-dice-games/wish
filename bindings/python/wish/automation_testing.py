"""pytest fixtures for "Jest-like" wish e2e tests, built on
:mod:`wish.automation` -- the "test harness" audience described in
``src/automation/DESIGN.md``: a test that drives an app deterministically
and asserts on widget state or a screenshot.

Two ways to use this module:

1. **Zero-config** -- set ``WISH_AUTOMATION_SERVER_CMD`` (a shell-quoted
   command line) in the test environment and use the ready-made ``wish_ui``
   fixture::

       # WISH_AUTOMATION_SERVER_CMD="wish server --renderer web"
       def test_dialog_ok(wish_ui):
           wish_ui.click("dialog.ok")
           assert wish_ui.get_widget("status.label")["text"] == "Saved"

2. **Custom launch command per project** -- build your own fixture with
   :func:`make_wish_ui_fixture`, e.g. in a ``conftest.py``::

       from wish.automation_testing import make_wish_ui_fixture

       wish_ui = make_wish_ui_fixture(lambda: ["build/wish", "server", "--renderer", "web"])

Assertions are plain ``assert`` on :meth:`~wish.automation.AutomationClient.get_tree`
/ :meth:`~wish.automation.AutomationClient.get_widget` results;
:meth:`~wish.automation.AutomationClient.screenshot` can be attached to a
failed test's report by any existing pytest reporting plugin (e.g.
``pytest-html``) -- no custom assertion DSL is introduced here.
"""

from __future__ import annotations

import os
import shlex
from typing import Callable, Iterator, Sequence

import pytest

from .automation import AutomationClient

__all__ = ["make_wish_ui_fixture", "wish_ui"]


def make_wish_ui_fixture(
    server_cmd_factory: Callable[[], Sequence[str]], *, scope: str = "function", headless: bool = True
):
    """Build a pytest fixture that launches a fresh wish server + headless
    Chromium tab per @p scope, yielding a connected `AutomationClient`.

    @param server_cmd_factory  Called once per fixture instantiation to get
                                 the `wish server --renderer web ...` argv --
                                 a callable (not a plain list) so each
                                 instantiation can vary the command (e.g. a
                                 distinct RMI port per test) if needed.
    @param scope   Standard pytest fixture scope (`"function"`, `"module"`,
                    `"session"`, ...). Defaults to a fresh server per test.
    @param headless  Forwarded to `AutomationClient.launch()`.
    """

    @pytest.fixture(scope=scope)
    def _fixture() -> Iterator[AutomationClient]:
        with AutomationClient.launch(server_cmd=list(server_cmd_factory()), headless=headless) as ui:
            yield ui

    return _fixture


@pytest.fixture
def wish_ui() -> Iterator[AutomationClient]:
    """Ready-made per-test fixture: reads `WISH_AUTOMATION_SERVER_CMD` (a
    shell-quoted command line, e.g. `"wish server --renderer web"`) from the
    environment. Skips the test (rather than failing) when unset, so a suite
    using this fixture is opt-in per environment -- e.g. a CI job that has
    built `wish` sets the variable; a plain `pytest` run elsewhere skips
    cleanly instead of erroring on a missing binary.
    """
    cmd_str = os.environ.get("WISH_AUTOMATION_SERVER_CMD")
    if not cmd_str:
        pytest.skip("set WISH_AUTOMATION_SERVER_CMD to run wish automation e2e tests")
    with AutomationClient.launch(server_cmd=shlex.split(cmd_str)) as ui:
        yield ui
