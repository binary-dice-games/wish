"""Notepad example using the wish Python binding.

Port of modules/notepad/client/notepad.cpp: the Notepad form (server-side)
never touches this process's local filesystem -- it only edits files already
sitting in its session sandbox. This script is the bridge: it reacts to the
form's high-level events by driving its own local files through
Client.upload_file() / Client.download_file().

Start a wish server first (it owns the window/renderer), then point this
script at it -- matching whichever transport the server was started with
(see wish server --help):

    build/app/wish server --transport=tcp --port=7070 --renderer=sdl3
    python bindings/python/examples/notepad_example.py --transport=tcp --host=127.0.0.1 --port=7070 -- path/to/file

Run with:  python bindings/python/examples/notepad_example.py [--transport=tcp|pipe|term] [--host=HOST] [--port=PORT] [--name=PATH] [--theme=dark|light|classic] [-- path/to/file]
"""

import argparse
import json
import os
import sys
import threading
from pathlib import Path
from typing import Optional

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from wish import Client  # noqa: E402 -- sets up bison's sys.path as a side effect
from bison import Dynamic  # noqa: E402


def read_local_file(path: Path) -> bytes:
    return path.read_bytes()


def write_local_file(path: Path, data: bytes) -> None:
    path.write_bytes(data)


def list_directory(directory: Path) -> list:
    """Build the `files` list expected by FileDialog from a directory listing.

    Mirrors list_directory() in modules/notepad/client/notepad.cpp.
    """
    files = []
    if directory.parent != directory:
        files.append({"name": "..", "type": "dir"})
    try:
        for entry in sorted(directory.iterdir()):
            files.append({"name": entry.name, "type": "dir" if entry.is_dir() else "file"})
    except OSError:
        pass
    return files


class SandboxFiles:
    """Tracks the sandbox name <-> local path mapping for files this client

    has uploaded, and picks a sandbox name that does not collide with one
    already in use (e.g. two different directories each containing a
    "notes.txt"). Re-opening the exact same local path twice is not
    deduplicated here -- the server already no-ops a duplicate open_file
    call for a given sandbox path, so at worst this produces two
    independently-edited tabs backed by two sandbox copies of one file.
    """

    def __init__(self):
        self.local_path_by_sandbox_name: dict[str, Path] = {}

    def reserve_name(self, local_path: Path) -> str:
        candidate = local_path.name
        suffix = 1
        while candidate in self.local_path_by_sandbox_name:
            candidate = f"{local_path.stem}_{suffix}{local_path.suffix}"
            suffix += 1
        return candidate


def upload_and_open(client: Client, notepad, files: SandboxFiles, local_path: Path) -> None:
    """Upload a local file's current contents into the sandbox under a fresh

    name and register it as a new Notepad tab. Shared by the FileDialog-driven
    Open/New flows and by opening a file passed on the command line.
    """
    data = read_local_file(local_path)
    sandbox_name = files.reserve_name(local_path)
    client.upload_file(sandbox_name, data)
    files.local_path_by_sandbox_name[sandbox_name] = local_path

    notepad.call("open_file", {"path": sandbox_name, "title": local_path.name})


def json_array_dynamic(items: list) -> Dynamic:
    """Build a Dynamic array of objects from a list of JSON-serializable dicts.

    The C ABI has no way to set an object/array element at an array index
    (bison_set_object_at() does not exist), so nested arrays of objects --
    like FileDialog's `files` listing -- are built by round-tripping through
    JSON instead. bison_from_json() only accepts a top-level object, so the
    list is wrapped in one and the "items" field (itself array-like) is
    projected back out.
    """
    from bison import from_json

    return from_json(json.dumps({"items": items}))["items"]


def browse_and_open(
    client: Client,
    notepad,
    files: SandboxFiles,
    title: str,
    confirm_label: str,
    create_if_missing: bool,
) -> None:
    """Shared by "Open" and "New": show a FileDialog populated from a local

    directory listing, then upload the chosen file and register it via
    open_file(). `create_if_missing` is set for "New", where the chosen path
    need not already exist locally.
    """
    cur_dir = [Path.cwd()]

    def make_init(directory: Path) -> Dynamic:
        init = Dynamic()
        init.title = title
        init.confirm_label = confirm_label
        init.path = str(directory)
        init.files = json_array_dynamic(list_directory(directory))
        return init

    dlg = client.instantiate("FileDialog", "wish")
    dlg.set(make_init(cur_dir[0]))

    def on_navigate(payload):
        name = payload.name
        kind = payload.type
        if kind == "path":
            cur_dir[0] = Path(name)
        else:
            cur_dir[0] = cur_dir[0].parent if name == ".." else (cur_dir[0] / name)
        dlg.set(make_init(cur_dir[0]))

    def on_open(payload):
        name = payload.path
        local_path = Path(name)
        if not local_path.is_absolute():
            local_path = cur_dir[0] / local_path

        # "New": the user typed/picked a path that may not exist yet -- create
        # it empty. If it already exists (e.g. they picked an existing file
        # by mistake), leave its content alone rather than truncating it.
        if create_if_missing and not local_path.exists():
            write_local_file(local_path, b"")

        upload_and_open(client, notepad, files, local_path)

    def on_cancel(_payload):
        pass

    dlg.on_event("on_navigate", on_navigate)
    dlg.on_event("on_open", on_open)
    dlg.on_event("on_cancel", on_cancel)


def run_notepad(client: Client, startup_path: Optional[Path]) -> None:
    notepad = client.instantiate("Notepad", "wish")
    files = SandboxFiles()

    # "Open" clicked: the server has no view of the client's local files, so
    # it asks us to present our own picker.
    def on_request_open(_payload):
        browse_and_open(client, notepad, files, "Open File", "Open", create_if_missing=False)

    # "New" clicked: same handshake, but the chosen path need not exist yet.
    def on_request_new(_payload):
        browse_and_open(client, notepad, files, "New File", "New", create_if_missing=True)

    # A tab (or the whole window) closed: download this file one last time
    # and forget our local bookkeeping for it.
    def on_file_closed(payload):
        path = payload.path
        local_path = files.local_path_by_sandbox_name.pop(path, None)
        if local_path is None:
            return
        write_local_file(local_path, client.download_file(path))

    # Ctrl+S inside a tab: download that one file, keep it open.
    def on_file_saved(payload):
        path = payload.path
        local_path = files.local_path_by_sandbox_name.get(path)
        if local_path is not None:
            write_local_file(local_path, client.download_file(path))

    # "Sync" clicked: force-download every currently open file.
    def on_sync_requested(payload):
        paths = payload.paths
        for path in paths:
            local_path = files.local_path_by_sandbox_name.get(path)
            if local_path is not None:
                write_local_file(local_path, client.download_file(path))

    def on_closed(_payload):
        client.quit()

    notepad.on_event("on_request_open", on_request_open)
    notepad.on_event("on_request_new", on_request_new)
    notepad.on_event("on_file_closed", on_file_closed)
    notepad.on_event("on_file_saved", on_file_saved)
    notepad.on_event("on_sync_requested", on_sync_requested)
    notepad.on_event("closed", on_closed)

    # A file to open at startup may be passed after `--` on the command line.
    if startup_path is not None:
        if startup_path.exists():
            upload_and_open(client, notepad, files, startup_path)
        else:
            print(f"[notepad] no such file: {startup_path}", file=sys.stderr)

    client.wait()
    notepad.release()


def main():
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("--transport", choices=["tcp", "pipe", "term"], default="tcp")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7070)
    parser.add_argument("--name", default="", help="Named-pipe / Unix-socket path (--transport=pipe)")
    parser.add_argument("--theme", choices=["dark", "light", "classic"], default="dark")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("file", nargs="?", default=None, help="Path to a file to open at startup (optional)")
    args = parser.parse_args()

    startup_path = Path(args.file).resolve() if args.file else None

    if args.transport == "tcp":
        print(f"[Client] connecting to {args.host}:{args.port} ...")
        client = Client.tcp(args.host, args.port)
    elif args.transport == "pipe":
        print(f"[Client] connecting to pipe {args.name} ...")
        client = Client.pipe(args.name)
    else:
        print("[Client] connecting via inherited stdio (--transport=term) ...")
        client = Client.term()

    # client.run() calls wish_client_run(), which invokes run_session() on the
    # library's own RMI worker thread and blocks *this* call until it returns
    # -- an uninterruptible native wait from Python's point of view. Run it on
    # a background thread instead, so the main thread is free to sit in a
    # short join() loop: that's what lets CPython's signal dispatcher actually
    # deliver a pending Ctrl+C as KeyboardInterrupt (it only runs between
    # bytecode instructions, never while blocked inside a foreign C call). On
    # KeyboardInterrupt, quit() unblocks run_session()'s wait() the same way
    # clicking the window's close button (X) does.
    errors: list = []

    def worker():
        try:
            def session(c: Client):
                c.set_style_preset(args.theme)
                run_notepad(c, startup_path)

            client.run(session)
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
