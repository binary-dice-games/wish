"""Calculator example using the wish Python binding.

Mirrors examples/calculator/main.cpp, minus the in-memory server/renderer
wiring: this script is a *client only*. Start a wish server first (it owns
the window/renderer), then point this script at it -- matching whichever
transport the server was started with (see wish server --help):

    # --transport=tcp (explicit; the server's default is --transport=term):
    build/app/wish server --transport=tcp --port=7070 --renderer=sdl3
    python bindings/python/examples/calculator_example.py --transport=tcp --host=127.0.0.1 --port=7070

    # --transport=term (the server's default): the server spawns its own
    # terminal and expects the client to run *inside* it, wrapping that
    # process's own inherited stdio (see wish_client_term_create()):
    build/app/wish server --renderer=sdl3
    # -- inside the terminal the server just spawned --
    python bindings/python/examples/calculator_example.py --transport=term

Run with:  python bindings/python/examples/calculator_example.py [--transport=tcp|pipe|term] [--host=HOST] [--port=PORT] [--name=PATH] [--theme=dark|light|classic]
"""

import argparse
import math
import os
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from wish import Client

KCALC_DESC = """{
  "type": "Window",
  "title": "Calculator",
  "width": 328,
  "height": 420,
  "closable": true,
  "children": {
    "display": { "type": "Label", "text": "0" },
    "sep":     { "type": "Separator" },
    "row0": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "c":   { "type": "Button", "label": "C",   "width": 72, "height": 52 },
        "div": { "type": "Button", "label": "/",   "width": 72, "height": 52 },
        "mul": { "type": "Button", "label": "*",   "width": 72, "height": 52 },
        "bsp": { "type": "Button", "label": "<-",  "width": 72, "height": 52 }
      }
    },
    "row1": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n7":  { "type": "Button", "label": "7", "width": 72, "height": 52 },
        "n8":  { "type": "Button", "label": "8", "width": 72, "height": 52 },
        "n9":  { "type": "Button", "label": "9", "width": 72, "height": 52 },
        "sub": { "type": "Button", "label": "-", "width": 72, "height": 52 }
      }
    },
    "row2": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n4":  { "type": "Button", "label": "4", "width": 72, "height": 52 },
        "n5":  { "type": "Button", "label": "5", "width": 72, "height": 52 },
        "n6":  { "type": "Button", "label": "6", "width": 72, "height": 52 },
        "add": { "type": "Button", "label": "+", "width": 72, "height": 52 }
      }
    },
    "row3": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n1": { "type": "Button", "label": "1", "width": 72, "height": 52 },
        "n2": { "type": "Button", "label": "2", "width": 72, "height": 52 },
        "n3": { "type": "Button", "label": "3", "width": 72, "height": 52 },
        "eq": { "type": "Button", "label": "=", "width": 72, "height": 52 }
      }
    },
    "row4": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n0":  { "type": "Button", "label": "0",   "width": 72, "height": 52 },
        "dot": { "type": "Button", "label": ".",   "width": 72, "height": 52 },
        "pm":  { "type": "Button", "label": "+/-", "width": 72, "height": 52 },
        "pct": { "type": "Button", "label": "%",   "width": 72, "height": 52 }
      }
    }
  }
}"""


class Calculator:
    """Port of calc_client from examples/calculator/main.cpp."""

    def __init__(self, client: Client, verbose: bool, theme: str):
        self.client = client
        self.verbose = verbose
        self.theme = theme
        self.display = "0"
        self.operand = 0.0
        self.pending_op = None
        self.fresh = True
        # Every proxy handed to on_event() must be kept alive for the whole
        # session: Proxy.__del__ releases the handle *and* drops the ctypes
        # callback trampoline it holds, so a proxy_get(...).on_event(...)
        # one-liner with no surviving reference gets garbage-collected right
        # after registering -- the next server-side click then calls into a
        # freed trampoline. This dict is the Python analogue of the proxy
        # map (`pm`) the C++ example keeps alive via instantiate_template().
        self.buttons = {}

    def vlog(self, msg: str) -> None:
        if self.verbose:
            print(f"[calc] {msg}")

    def update_display(self, disp) -> None:
        self.vlog(f'update_display -> "{self.display}"')
        disp["text"] = self.display

    def button(self, client: Client, path: str):
        """Resolve + cache the proxy for "row.name", keyed under self.buttons."""
        proxy = client.proxy_get(f"calc.{path}")
        self.buttons[path] = proxy
        return proxy

    def run_session(self, client: Client) -> None:
        self.vlog(f"applying {self.theme} theme")
        client.set_style_preset(self.theme)

        self.vlog("registering template 'calc'")
        client.register_template("calc", KCALC_DESC)

        self.vlog("instantiating template 'calc'")
        root = client.instantiate_template("calc", "calc")

        def on_closed(_params):
            self.vlog("window closed -- quitting")
            client.quit()

        root.on_event("closed", on_closed)

        disp = client.proxy_get("calc.display")

        def digit_handler(ch: str):
            def handler(_params):
                self.vlog(f"digit '{ch}' clicked")
                if self.fresh:
                    self.display = ch
                    self.fresh = False
                else:
                    self.display += ch
                self.update_display(disp)

            return handler

        def op_handler(op: str):
            def handler(_params):
                self.vlog(f"op '{op}' clicked")
                self.operand = float(self.display)
                self.pending_op = op
                self.fresh = True
                self.update_display(disp)

            return handler

        self.vlog("registering button handlers")

        def on_clear(_params):
            self.vlog("C (clear) clicked")
            self.display = "0"
            self.operand = 0.0
            self.pending_op = None
            self.fresh = True
            self.update_display(disp)

        self.button(client, "row0.c").on_event("clicked", on_clear)
        self.button(client, "row0.div").on_event("clicked", op_handler("/"))
        self.button(client, "row0.mul").on_event("clicked", op_handler("*"))

        def on_backspace(_params):
            self.vlog("<- (backspace) clicked")
            if len(self.display) > 1:
                self.display = self.display[:-1]
            else:
                self.display = "0"
            self.update_display(disp)

        self.button(client, "row0.bsp").on_event("clicked", on_backspace)

        self.button(client, "row1.n7").on_event("clicked", digit_handler("7"))
        self.button(client, "row1.n8").on_event("clicked", digit_handler("8"))
        self.button(client, "row1.n9").on_event("clicked", digit_handler("9"))
        self.button(client, "row1.sub").on_event("clicked", op_handler("-"))

        self.button(client, "row2.n4").on_event("clicked", digit_handler("4"))
        self.button(client, "row2.n5").on_event("clicked", digit_handler("5"))
        self.button(client, "row2.n6").on_event("clicked", digit_handler("6"))
        self.button(client, "row2.add").on_event("clicked", op_handler("+"))

        self.button(client, "row3.n1").on_event("clicked", digit_handler("1"))
        self.button(client, "row3.n2").on_event("clicked", digit_handler("2"))
        self.button(client, "row3.n3").on_event("clicked", digit_handler("3"))

        def on_equals(_params):
            self.vlog("= (equals) clicked")
            rhs = float(self.display)
            if self.pending_op == "+":
                result = self.operand + rhs
            elif self.pending_op == "-":
                result = self.operand - rhs
            elif self.pending_op == "*":
                result = self.operand * rhs
            elif self.pending_op == "/":
                result = (self.operand / rhs) if rhs != 0.0 else 0.0
            else:
                result = rhs

            if result == math.floor(result) and abs(result) < 1e12:
                self.display = str(int(result))
            else:
                self.display = repr(result)
            self.pending_op = None
            self.fresh = True
            self.vlog(f'result: "{self.display}"')
            self.update_display(disp)

        self.button(client, "row3.eq").on_event("clicked", on_equals)

        self.button(client, "row4.n0").on_event("clicked", digit_handler("0"))

        def on_dot(_params):
            self.vlog(". (dot) clicked")
            if "." not in self.display:
                self.display += "."
            self.fresh = False
            self.update_display(disp)

        self.button(client, "row4.dot").on_event("clicked", on_dot)

        def on_plus_minus(_params):
            self.vlog("+/- clicked")
            if self.display and self.display != "0":
                if self.display[0] == "-":
                    self.display = self.display[1:]
                else:
                    self.display = "-" + self.display
            self.update_display(disp)

        self.button(client, "row4.pm").on_event("clicked", on_plus_minus)

        def on_percent(_params):
            self.vlog("% clicked")
            self.display = repr(float(self.display) / 100.0)
            self.update_display(disp)

        self.button(client, "row4.pct").on_event("clicked", on_percent)

        self.vlog("ready -- waiting for quit()")
        client.wait()
        self.vlog("session ending")

        client.release("calc")
        root.release()
        disp.release()
        for proxy in self.buttons.values():
            proxy.release()
        self.buttons.clear()


def main():
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("--transport", choices=["tcp", "pipe", "term"], default="tcp")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7070)
    parser.add_argument("--name", default="", help="Named-pipe / Unix-socket path (--transport=pipe)")
    parser.add_argument("--theme", choices=["dark", "light", "classic"], default="dark")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    if args.transport == "tcp":
        print(f"[Client] connecting to {args.host}:{args.port} ...")
        client = Client.tcp(args.host, args.port)
    elif args.transport == "pipe":
        print(f"[Client] connecting to pipe {args.name} ...")
        client = Client.pipe(args.name)
    else:
        print("[Client] connecting via inherited stdio (--transport=term) ...")
        client = Client.term()

    calc = Calculator(client, args.verbose, args.theme)

    # client.run() calls wish_client_run(), which invokes run_session() on
    # the library's own RMI worker thread and blocks *this* call until it
    # returns -- an uninterruptible native wait from Python's point of view.
    # Run it on a background thread instead, so the main thread is free to
    # sit in a short join() loop: that's what lets CPython's signal
    # dispatcher actually deliver a pending Ctrl+C as KeyboardInterrupt (it
    # only runs between bytecode instructions, never while blocked inside a
    # foreign C call). On KeyboardInterrupt, quit() unblocks run_session()'s
    # wait() the same way clicking the window's close button (X) does.
    errors: list = []

    def worker():
        try:
            client.run(calc.run_session)
        except Exception as exc:
            errors.append(exc)

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()
    try:
        while thread.is_alive():
            thread.join(timeout=0.2)
    except KeyboardInterrupt:
        print("\n[Client] Ctrl+C -- quitting ...")
        client.quit()
        thread.join()

    if errors:
        raise errors[0]
    print("[Client] done.")


if __name__ == "__main__":
    main()
