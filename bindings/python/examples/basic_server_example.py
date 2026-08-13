"""Basic wish server, runnable purely from Python, using wish_server_dll.

Hosts a real wish session (the same bdg::wish::server implementation the
`wish server` CLI uses -- real per-widget proxies, real events) and renders
it with --renderer=sdl3 (a real window), --renderer=web (a browser tab), or
--renderer=console (a text dump to stdout, no display needed). Any ABI-based
client -- this Python binding's own calculator_example.py, the C++/C#
bindings, or a hand-rolled wish_client_c.h caller -- can connect to it
exactly as it would to the compiled `wish server` binary:

    python bindings/python/examples/basic_server_example.py --transport=tcp --port=7070 --renderer=sdl3
    # in another terminal:
    python bindings/python/examples/calculator_example.py --transport=tcp --host=127.0.0.1 --port=7070

Run with:  python bindings/python/examples/basic_server_example.py [--transport=tcp|pipe] [--host=HOST] [--port=PORT] [--name=PATH] [--renderer=sdl3|web|console] [--title=TITLE] [--width=W] [--height=H] [--web_bind=HOST] [--web_port=PORT] [--verbose]
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from wish import Server


def main():
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("--transport", choices=["tcp", "pipe"], default="tcp")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7070)
    parser.add_argument("--name", default="")
    parser.add_argument("--renderer", choices=["sdl3", "web", "console"], default="sdl3")
    parser.add_argument("--title", default="wish")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--font_size", type=int, default=16)
    parser.add_argument("--web_bind", default="127.0.0.1")
    parser.add_argument("--web_port", type=int, default=8080)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    server = Server.tcp(args.host, args.port) if args.transport == "tcp" else Server.pipe(args.name)
    if args.verbose:
        server.set_verbose(True)

    server.start(
        renderer=args.renderer,
        title=args.title,
        width=args.width,
        height=args.height,
        font_size=args.font_size,
        web_bind=args.web_bind,
        web_port=args.web_port,
    )

    if args.transport == "tcp":
        print(f"[wish] listening on {args.host}:{args.port}")
    else:
        print(f"[wish] listening on pipe {args.name}")
    if args.renderer == "web":
        print(f"[wish] open http://{args.web_bind}:{args.web_port} in a browser")
    print("[wish] Press Enter to stop...")

    input()

    server.stop()
    server.release()
    print("[wish] stopped.")


if __name__ == "__main__":
    main()
