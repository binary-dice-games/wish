#!/usr/bin/env python3
# MIT License (c) 2026 Binary Dice Games
#
# Python attach target for tests/test_python_debug_backend.cpp's live cases
# (the counterpart of tests/fixtures/dbg_fixture.cpp for the native
# backends). It opens a `debugpy` debug server on an ephemeral port, prints
# "PORT <n>" so the test can point python_debug_backend's connect mode at
# it, then runs a slow bounded loop with distinct inner_function /
# outer_function frames.
#
# NOTE: the test hard-codes line 28 (the breakpoint / step target, the
# `y = x * 2` line) and line 33 (outer_function's call site). Keep them
# where they are.
import sys
import time

try:
    import debugpy
except ImportError:
    print("NO_DEBUGPY", flush=True)
    sys.exit(1)

_host, _port = debugpy.listen(("127.0.0.1", 0))
print("PORT %d" % _port, flush=True)


def inner_function(x):
    y = x * 2  # line 28: breakpoint / step target
    return y


def outer_function(x):
    r = inner_function(x)  # line 33: step-over call site
    return r + 1


def main():
    counter = 0
    for _ in range(400):
        counter = outer_function(counter)
        time.sleep(0.05)
    return counter


if __name__ == "__main__":
    main()
