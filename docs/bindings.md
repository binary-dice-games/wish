# Language Bindings

## C++ (`bindings/cpp/`)

Header-only wrapper (`bindings/cpp/include/wish_cpp/`) around
`wish_client_c.h` (plus `bison_c.h`/`rmi_c.h`), for C++ applications that
only want to connect to a running wish server -- no bison/wish source needs
to be compiled in, just `#include <wish_cpp/wish.hpp>` and link the prebuilt
`wish_client_dll` shared library. This is distinct from the native,
statically-linked `bdg::wish::client` (`src/client/client.hpp`) that
single-binary demos like `examples/calculator` use with an in-memory
transport; use this binding instead when the client is its own separate
process/executable, the same situation the Python and C# bindings address
for their languages.

Since C++ (unlike Python/C#) has `constexpr`, `wish_cpp::key_t`'s
`const char*` constructor reimplements the FNV-1a hash used by
`wish_key()`/`bison_key()`/`"name"_key` as a `constexpr` function instead of
calling into the shared library: a `"name"_key` literal is evaluated
entirely at compile time and costs nothing at runtime, with no
hashing-related library call at all. Only names known solely at runtime
(e.g. a dot-path string) pay for hashing, via that same function, at the
point they're used. See `bindings/cpp/include/wish_cpp/key.hpp`.

There is no header-only equivalent of `bison::dynamic` (that class is
compiled/linked library code), so this binding ships its own thin
`wish_cpp::value` -- a header-only RAII wrapper directly over `bison_handle`
(`bindings/cpp/include/wish_cpp/value.hpp`) -- for proxy `set()`/`get()`/
`call()` payloads, event parameters, and connect params.

**Requirements:** A C++17 compiler and CMake. Build `wish_client_dll` first
(same shared library the Python/C# bindings load):

```bash
cmake -B build -DWISH_BUILD_SHARED=ON
cmake --build build --target wish_client_dll
```

The `wish_cpp` CMake target (`bindings/cpp/CMakeLists.txt`, added
automatically when `WISH_BUILD_SHARED=ON`) is an `INTERFACE` library that
sets up the include paths and links `wish_client_dll` -- link your own
target against it the same way `calculator_cpp_binding`/`notepad_cpp_binding`
do.

### Running the calculator example

Start a wish server (it owns the window/renderer), matching whichever
transport you want the client to use:

```bash
# --transport=tcp, then in another terminal:
build/app/wish server --transport=tcp --port=7070 --renderer=sdl3
cmake --build build --target calculator_cpp_binding
build/bindings/cpp/calculator_cpp_binding --transport=tcp --host=127.0.0.1 --port=7070

# --transport=term (the server's default): the server spawns its own
# terminal and expects the client to run *inside* it, wrapping that
# process's own inherited stdio (wish::binding::client::term()):
build/app/wish server --renderer=sdl3
# -- inside the terminal the server just spawned --
build/bindings/cpp/calculator_cpp_binding --transport=term
```

`bindings/cpp/examples/notepad.cpp` (`notepad_cpp_binding`) is the C++ port
of [bindings/python/examples/notepad_example.py](../bindings/python/examples/notepad_example.py)
-- run it the same way, with an optional trailing file path to open at
startup.

Quick-start snippet:

```cpp
#include <wish_cpp/wish.hpp>

namespace wish = bdg::wish::binding;
using namespace bdg::wish::binding;  // for the "_key" literal operator

int main() {
  auto client = wish::client::tcp("127.0.0.1", 7070);
  client.run([](wish::client& c) {
    c.set_style_preset("dark");
    c.register_template("ui", R"({"type": "Window", "title": "Hi"})");
    auto root = c.instantiate_template("ui", "ui");
    std::cout << *root.get().get_string("title"_key) << "\n";  // "Hi"
    c.wait();                                                  // blocks until an event handler calls c.quit()
  });
}
```

### File transfer

`client::upload_file(name, data)` / `download_file(name) -> std::string`
move a whole file in one call. `upload_file_from_path`/
`download_file_to_path` stream a local file's content in chunks instead of
buffering it in memory (mirroring the C ABI's own streaming functions), and
`upload_package(dest_path, local_zip_path)` uploads a local zip archive and
has the server unpack it into `dest_path` inside the sandbox. See
[DESIGN.md](../DESIGN.md#bdgwishfile_service) for the chunked-transfer
protocol these build on.

### TLS

`wish::binding::client::tls(host, port)` is the TLS counterpart of `tcp()` --
trust/identity material (`ca_file`/`ca_pem`, `insecure_skip_verify`,
`cert_file`/`cert_pem`, `key_file`/`key_pem`, `key_password`, `server_name`)
is supplied via `run()`'s `connect_params`:

```cpp
auto client = wish::client::tls("127.0.0.1", 8443);
wish::value params;
params["ca_pem"_key] = ca_cert_pem;
client.run([](wish::client& c) { /* ... */ }, params);
```

See [cli.md](cli.md#tls-flags---transport-tls) for the matching `--transport=tls`
CLI flags and bison's [TLS-Secured Transport](https://github.com/binary-dice-games/bison/blob/main/docs/tls.md)
doc for the full parameter reference.

---

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
# Windows (cmd):
set WISH_LIB=%cd%\build\Debug\wish_client.dll
# Windows (powershell):
$env:WISH_LIB = "$PWD\build\Debug\wish_client.dll"
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

### File transfer

`Client.upload_file(name, data: bytes)` / `download_file(name) -> bytes` move
a whole file in one call, same as the C++ `std::string` overloads. For large
files, `Client.upload_file_from_path(name, local_path)` and
`download_file_to_path(name, local_path)` stream the content in chunks
to/from a local file on disk instead of buffering it in memory (the C ABI has
no `istream`/`ostream`, so these take a path and open it internally).
`Client.upload_package(dest_path, local_zip_path)` uploads a local zip
archive and has the server unpack it into `dest_path` inside the sandbox.
See [DESIGN.md](../DESIGN.md#bdgwishfile_service) for the chunked-transfer
protocol these build on.

### TLS

`Client.tls(host, port)` is the TLS counterpart of `Client.tcp()` -- trust/
identity material (`ca_file`/`ca_pem`, `insecure_skip_verify`,
`cert_file`/`cert_pem`, `key_file`/`key_pem`, `key_password`, `server_name`)
is supplied via `run()`'s `params`:

```python
client = Client.tls("127.0.0.1", 8443)
client.run(session, params={"ca_pem": ca_cert_pem})
```

See [cli.md](cli.md#tls-flags---transport-tls) for the matching `--transport=tls`
CLI flags and bison's [TLS-Secured Transport](https://github.com/binary-dice-games/bison/blob/main/docs/tls.md)
doc for the full parameter reference.

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
# Windows (cmd):
set WISH_LIB=%cd%\build\Debug\wish_client.dll
# Windows (powershell):
$env:WISH_LIB = "$PWD\build\Debug\wish_client.dll"
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

### TLS

`Client.Tls(host, port)` is the TLS counterpart of `Client.Tcp()` -- trust/
identity material (`ca_file`/`ca_pem`, `insecure_skip_verify`,
`cert_file`/`cert_pem`, `key_file`/`key_pem`, `key_password`, `server_name`)
is supplied via `Run()`'s `parameters`:

```csharp
using var client = Client.Tls("127.0.0.1", 8443);
client.Run(c => { /* ... */ }, new Dictionary<string, object> { ["ca_pem"] = caCertPem });
```

See [cli.md](cli.md#tls-flags---transport-tls) for the matching `--transport=tls`
CLI flags and bison's [TLS-Secured Transport](https://github.com/binary-dice-games/bison/blob/main/docs/tls.md)
doc for the full parameter reference.

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

### Native automation (`wish.Client`, for `--renderer sdl3`)

The SDL3 renderer has no browser tab for Playwright to drive, so it exposes
the same capability set — `get_tree`/`get_widgets`/`get_widget`/`click`/
`type_text`/`drag`/`screenshot`/`get_logs`/`wait_for` — directly as methods
on `wish.Client` (`bindings/python/wish/client.py`), riding the same
`wish_client_dll`/`ctypes` connection already used to build the UI. No
Playwright, no browser, no second client: one `Client.tcp(...)` connection
does both. Raises `WishError(code=WISH_ERR_NOT_FOUND)` if the connected
server's active renderer doesn't support it. See
[src/automation/DESIGN.md](../src/automation/DESIGN.md)'s "Native
(ABI-based) automation" section for the architecture.
