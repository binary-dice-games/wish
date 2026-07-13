# Language Bindings

## Python (`bindings/python/`)

Thin `ctypes` wrapper (`bindings/python/wish/`) exposing `wish_client_c.h`
through idiomatic Python syntax. The wish ABI depends on the bison ABI (a
`rmi_proxy_handle` is just a bison type), and `libwish_client.so` embeds
`bison_c.h`/`rmi_c.h` alongside `wish_client_c.h` in one shared object (see
the `wish_client_dll` CMake target) — so `wish/_native.py` loads that one
library and reuses `extern/bison/bindings/python/bison`'s own signature
setup for the `bison_*`/`rmi_*` half of the ABI, then layers `wish_*` on
top. Proxies and futures returned by `wish.Client` are plain
`bison.rmi.Proxy` / `bison.rmi.Future` instances — see
[extern/bison/docs/bindings.md](../extern/bison/docs/bindings.md) for their
API (`.get()` / `.set()` / `.call()` / `.on_event()`).

No installation needed — import directly. Every handle is RAII-wrapped
(`Client`, and the reused bison `Proxy`/`Future`); none of the
`wish_client_destroy()` / `rmi_*_release()` functions need to be called
directly outside of matching them 1:1 in code that doesn't use a `with`
block.

**Requirements:** Python 3.x. Build `wish_client_dll` first:

```bash
cmake -B build
cmake --build build --target wish_client_dll
```

Set `WISH_LIB` to the full path of the shared library if it is not found
automatically (default search is `build/libwish_client.so` /
`.dylib` / `build/Debug/wish_client.dll`):

```bash
# Linux:
export WISH_LIB=$(pwd)/build/libwish_client.so
# macOS:
export WISH_LIB=$(pwd)/build/libwish_client.dylib
# Windows:
set WISH_LIB=%cd%\build\Debug\wish_client.dll
```

### Running the calculator example

Start a wish server (it owns the window/renderer), matching whichever
transport you want the client to use — `wish server`'s own default is
`--transport=term`, not TCP:

```bash
# --transport=tcp, then in another terminal:
build/app/wish server --transport=tcp --port=7070 --renderer=sdl3
python bindings/python/examples/calculator_example.py --transport=tcp --host=127.0.0.1 --port=7070

# --transport=term (the default): the server spawns its own terminal and
# expects the client to run *inside* it, wrapping that process's own
# inherited stdio (wish_client_term_create()) — no separate terminal, no
# --host/--port:
build/app/wish server --renderer=sdl3
# -- inside the terminal the server just spawned --
python bindings/python/examples/calculator_example.py --transport=term
```

This is a line-for-line port of [examples/calculator/main.cpp](../examples/calculator/main.cpp)
minus the in-memory server/renderer setup — the C++ example is a
single-binary demo, but the Python binding only implements the client side
(`wish_client_c.h` has no server API), so it connects to a separately
running `wish server` instead.

Quick-start snippet:

```python
from wish import Client

def session(client):
    client.set_style_preset("dark")
    client.register_template("ui", '{"type": "Window", "title": "Hi"}')
    root = client.instantiate_template("ui", "ui")
    print(root["title"])   # "Hi"
    client.wait()          # blocks until an event handler calls client.quit()

Client.tcp("127.0.0.1", 7070).run(session)
```

---

## C# (`bindings/csharp/`)

Source-generated `[LibraryImport]` P/Invoke wrapper (`bindings/csharp/Wish/`)
over `wish_client_c.h`, layered directly on top of the Bison C# binding at
[extern/bison/bindings/csharp](../extern/bison/bindings/csharp) the same way
the Python binding layers on `extern/bison/bindings/python`: proxies and
futures returned by `Bdg.Wish.Client` are plain `Bdg.Bison.Rmi.Proxy` /
`Bdg.Bison.Rmi.Future` instances -- see
[extern/bison/docs/bindings.md](../extern/bison/docs/bindings.md) for their
API (`.Get()` / `.Set()` / `.Call()` / `.OnEvent()`, plus `dynamic`
attribute-style access). `Bdg.Wish.Client` is `IDisposable` with a finalizer
safety net, so `using`/`Dispose()` is enough -- `wish_client_destroy()` /
`rmi_*_release()` never need to be called directly.

**Requirements:** .NET 8 SDK. Build `wish_client_dll` first:

```bash
cmake -B build
cmake --build build --target wish_client_dll
```

Set `WISH_LIB` to the full path of the shared library if it is not found
automatically (default search is `build/libwish_client.so` /
`.dylib` / `build/Debug/wish_client.dll`), same convention as the Python
binding's `WISH_LIB`:

```bash
# Linux:
export WISH_LIB=$(pwd)/build/libwish_client.so
# macOS:
export WISH_LIB=$(pwd)/build/libwish_client.dylib
# Windows:
set WISH_LIB=%cd%\build\Debug\wish_client.dll
```

### Running the calculator example

```bash
# --transport=tcp, then in another terminal:
build/app/wish server --transport=tcp --port=7070 --renderer=sdl3
dotnet run --project bindings/csharp/examples/CalculatorExample -- --transport=tcp --host=127.0.0.1 --port=7070

# --transport=term (the default): the server spawns its own terminal and
# expects the client to run *inside* it, wrapping that process's own
# inherited stdio (Client.Term()) -- no separate terminal, no --host/--port:
build/app/wish server --renderer=sdl3
# -- inside the terminal the server just spawned --
dotnet run --project bindings/csharp/examples/CalculatorExample -- --transport=term
```

`bindings/csharp/examples/NotepadExample` is the C# port of
[bindings/python/examples/notepad_example.py](../bindings/python/examples/notepad_example.py)
-- run it the same way, with an optional trailing file path to open at
startup.

Run the tests with:

```bash
dotnet test bindings/csharp/Wish.Tests
```

Quick-start snippet:

```csharp
using Bdg.Wish;

using var client = Client.Tcp("127.0.0.1", 7070);
client.Run(c =>
{
    c.SetStylePreset("dark");
    c.RegisterTemplate("ui", """{"type": "Window", "title": "Hi"}""");
    using var root = c.InstantiateTemplate("ui", "ui");
    Console.WriteLine(root["title"]);   // "Hi"
    c.Wait();                           // blocks until an event handler calls c.Quit()
});
```

### Automation (`bindings/python/wish/automation.py`)

A separate, standalone module for driving a `wish server --renderer web`
instance the way Playwright drives a browser — screenshots, click/type
input, and a widget-tree query API for an AI agent debugging a UI, or a
pytest-based e2e suite (`wish.automation_testing`). Unlike `wish.Client`
above, it does **not** use `wish_client_dll`/`ctypes` at all — no native
build step needed on the Python side, only `pip install playwright`. See
[building.md](building.md#running-automation) for a runnable example and
[src/automation/DESIGN.md](../src/automation/DESIGN.md) for the protocol,
and `CLAUDE.md`'s "Automation: debugging and testing a wish UI" section for
the agent-facing workflow.
