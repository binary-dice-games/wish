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

### Running a server from C++

`wish::binding::server` (`bindings/cpp/include/wish_cpp/server.hpp`, over
`wish_server_dll`/`wish_server_c.h`) hosts a real wish session, the same
protocol implementation (`bdg::wish::server`) the `wish server` CLI uses --
template registration/instantiation gives each widget its own independently
addressable proxy, and events work, unlike a server built from the generic
bison RMI ABI alone. It's a **separate binding target** from `wish_cpp`
(`wish_cpp_server`, added automatically when `wish_server_dll` is available,
i.e. `-DWISH_BUILD_SERVER_SHARED=ON`) over a *separate* shared library from
`wish_client_dll` -- do not link both `wish_client_dll` and `wish_server_dll`
into one binary that also uses `wish::binding::value`; see `server.hpp`'s
file doc comment for why.

```bash
cmake -B build -DWISH_BUILD_SERVER_SHARED=ON
cmake --build build --target wish_server_dll
```

```cpp
#include <wish_cpp/server.hpp>

namespace wish = bdg::wish::binding;

int main() {
  auto server = wish::server::tcp("127.0.0.1", 7070);
  wish::value params;
  params["title"_key] = std::string{"My App"};
  server.start("sdl3", params);
  while (!server.should_quit()) { /* sleep, poll, etc. */ }
  server.stop();
}
```

`renderer` is `"sdl3"` (a real window), `"web"` (pass `web_bind`/`web_port`;
open the printed URL in a browser), or `"console"` (a lightweight text dump
of the widget tree to stdout as it's built/updated -- no display needed,
meant for tests/CI). `server::tls(host, port)` and `server::term(cmd)` are
the TLS and terminal (spawns a child process on a new pseudo-terminal, `--cmd`
CLI-flag equivalent) counterparts of `tcp()`/`pipe()`; TLS material is
supplied via `start()`'s `params`, forwarded unchanged as transport listen
params.

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

Two ways to get `wish`, `import`able either way:

- **`pip install`** (`bindings/python/pyproject.toml`, scikit-build-core):
  compiles `wish_client_dll` and `wish_server_dll` from source and ships
  both inside the installed `wish` package, alongside its `bison-abi`
  dependency (installed automatically):
  ```bash
  git clone --recurse-submodules https://github.com/binary-dice-games/wish.git
  pip install ./wish/bindings/python
  ```
  Needs a C++20 compiler and CMake 3.11+. The configure step also
  initializes `extern/bison` if missing (wish's root `CMakeLists.txt`'s
  `add_subdirectory("extern/bison")`); no other submodule needs manual
  `--recurse-submodules` handling since it's all driven by the same clone.
  Unlike `bison-abi`'s own pip build, this one is not lightweight — because
  `wish_server_dll` exposes real SDL3/web rendering (see
  [wish.Server](#running-a-server-from-python) below), `WISH_ENABLE_IMGUI`/
  `SDL3`/`WEB` stay at their normal `ON` defaults, so the install compiles
  Dear ImGui, SDL3, and civetweb too, same as a normal `wish` C++ build. See
  [bindings/python/README.md](../bindings/python/README.md) for the
  package's own quick-start. `pip install -e ./wish/bindings/python` also
  works, for local development.
- **Import directly, no install** — every handle is RAII-wrapped (`Client`,
  and the reused bison `Proxy`/`Future`); none of the `wish_client_destroy()`
  / `rmi_*_release()` functions need to be called directly outside of
  matching them 1:1 in code that doesn't use a `with` block.

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

### Running a server from Python

`wish.Server` (`bindings/python/wish/server.py`, over `wish_server_dll` /
`wish_server_c.h`) hosts a real wish session from Python, the same protocol
implementation (`bdg::wish::server`) the `wish server` CLI uses -- template
registration/instantiation gives each widget its own independently
addressable proxy, and events work, unlike a server built from the generic
bison RMI ABI alone. It's a *separate* shared library from `wish_client_dll`
(`WISH_BUILD_SERVER_SHARED`, off by default in a plain CMake build, on for
the `wish-abi` pip package) -- import `wish.server` only if you need to host
a session; `wish.Client` never loads it.

```python
from wish import Server

server = Server.tcp("127.0.0.1", 7070)
server.start(renderer="sdl3", title="My App", width=1280, height=720)
input("Press Enter to stop...\n")
server.stop()
```

`renderer` is `"sdl3"` (a real window), `"web"` (pass `web_bind`/`web_port`;
open the printed URL in a browser, same as `wish server --renderer=web`), or
`"console"` (a lightweight text dump of the widget tree to stdout as it's
built/updated -- no display needed, meant for tests/CI, not as a real UI).
`bindings/python/examples/basic_server_example.py` is a runnable CLI wrapper
matching the `wish server` app's own flag names; any ABI-based client (any
language) can connect to it exactly as it would to the compiled `wish
server` binary. `Server.tls(host, port)` and `Server.term(cmd="")` are the
TLS and terminal (spawns a child process on a new pseudo-terminal) counterparts
of `Server.tcp()`/`Server.pipe()`; TLS material (`cert_file`/`cert_pem`,
`key_file`/`key_pem`, `key_password`, and optionally `client_auth`/`ca_file`/
`ca_pem` for mutual TLS) is supplied via `start()`'s `**params`, forwarded
unchanged as transport listen params.

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

### Running a server from C#

`Bdg.Wish.Server` (`bindings/csharp/Wish/Server.cs`, over `wish_server_dll`/
`wish_server_c.h`) hosts a real wish session, the same protocol
implementation (`bdg::wish::server`) the `wish server` CLI uses -- template
registration/instantiation gives each widget its own independently
addressable proxy, and events work, unlike a server built from the generic
bison RMI ABI alone. It's a *separate* shared library from `wish_client` --
set `WISH_SERVER_LIB` the same way `WISH_LIB` locates `wish_client` if it
isn't found automatically (default search is `build/libwish_server.so`/
`.dylib`/`build/Debug/wish_server.dll`). `Server` does not depend on
`Bdg.Bison.Dynamic` (see `ServerNative.cs`'s doc comment for why -- the same
cross-library `bison_handle` hazard `_server_native.py` avoids in the Python
binding), so its `Start()` takes a plain `IReadOnlyDictionary<string, object>`
instead.

```csharp
using Bdg.Wish;

using var server = Server.Tcp("127.0.0.1", 7070);
server.Start("sdl3", new Dictionary<string, object> { ["title"] = "My App", ["width"] = 1280, ["height"] = 720 });
Console.ReadLine();
server.Stop();
```

`renderer` is `"sdl3"` (a real window), `"web"` (pass `web_bind`/`web_port`;
open the printed URL in a browser), or `"console"` (a lightweight text dump
of the widget tree to stdout as it's built/updated -- no display needed,
meant for tests/CI). `Server.Tls(host, port)` and `Server.Term(cmd = "")`
are the TLS and terminal (spawns a child process on a new pseudo-terminal)
counterparts of `Server.Tcp()`/`Server.Pipe()`; TLS material is supplied via
`Start()`'s `parameters`, forwarded unchanged as transport listen params.

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

---

## Android (Java / Kotlin) (`bindings/android/`)

Like Android apps generally, this binding ships its own native libraries
inside the APK instead of loading a precompiled shared library at run time
(`ctypes.CDLL`/`[LibraryImport]`/link-time, as the other bindings do). It's
two pieces: `bindings/android/jni/` is JNI glue (the `wish_jni` CMake
target, built straight from this repo's own root `CMakeLists.txt` -- see
[docs/building.md](building.md#building-for-android)) linked against
`wish_client_dll`, and `bindings/android/wish-lib/` is the Java package
(`com.bdg.wish`) that calls it -- `Client c = Client.tcp(host, port);`
instead of `wish_client_tcp_create(host, port)`. Both `libwish_client.so`
and `libwish_jni.so` end up in the app's `jniLibs/<abi>/`;
`NativeLibrary.ensureLoaded()` (called from every public class's static
initializer) loads both, `wish_client` first.

Unlike the C#/Python bindings, this one does **not** depend on bison's own
Android binding (`extern/bison/bindings/android`) -- `com.bdg.wish.Dynamic`/
`Key` are a self-contained reimplementation of the `bison_c.h` surface
wish needs, backed by the same single `wish_jni.so` as everything else.
Loading bison's separately-built `bison_abi.so`/`bison_jni.so` alongside it
would mean two independent copies of the bison/RMI C ABI in one process --
harmless in isolation, but any `bison_handle`/`rmi_proxy_handle` this
binding hands to Java is only ever valid against the exact `wish_client_dll`
instance that created it, so mixing the two would be a footgun for no
benefit. (The C#/Python bindings avoid the same footgun differently, by
redirecting bison's own native-library resolution at `wish_client`'s shared
object at run time -- see `Native.cs`'s doc comment -- a trick Java's
`System.loadLibrary` has no equivalent for, hence the different fix here.)

`Client` is `AutoCloseable`, matching the C# binding's `IDisposable` choice
-- use a try-with-resources block, or call `close()` directly:

```java
import com.bdg.wish.Client;

try (Client client = Client.tcp("127.0.0.1", 7070)) {
    client.run(c -> {
        c.setStylePreset("dark");
        c.registerTemplate("ui", "{\"type\": \"Window\", \"title\": \"Hi\"}");
        try (var root = c.instantiateTemplate("ui", "ui")) {
            root.onEvent("closed", params -> c.quit());
            c.waitForQuit();   // blocks until an event handler calls c.quit()
        }
    });
}
```

`Client.run()` blocks the calling thread until the session callback
returns, and the callback itself runs on the library's internal RMI worker
thread (not the calling thread) -- run it from a background thread if the
caller (e.g. an Android `Activity`) needs to stay responsive, the same
pattern the C# example's `Thread` wrapper uses. See
`bindings/android/examples/WishExample/.../MainActivity.kt` for a complete
worked example, including posting results back to the UI thread with
`runOnUiThread`.

**Requirements:** Android NDK r26+, `compileSdk`/`targetSdk` 34, `minSdk` 24
(see [docs/building.md](building.md#building-for-android) for why 24, not
21). No separate `wish_client_dll` build step to run by hand -- Gradle's
`externalNativeBuild` drives it:

```bash
cd bindings/android
./gradlew assembleDebug                     # builds :wish-lib and :examples:WishExample
./gradlew :wish-lib:connectedAndroidTest    # runs the binding's instrumented tests on a device/emulator
```

See [docs/examples.md](examples.md#android-example-emulator) for running
the example app on an emulator, alongside a `wish server` to connect to.

### Gaps versus the C#/Python bindings

- **Only TCP and TLS transports** are bound (`Client.tcp`/`Client.tls`) --
  named-pipe and terminal (`--transport=term`) aren't meaningful for an
  Android app process, matching the same choice bison's own Android binding
  makes for `com.bdg.bison.rmi.Client`.
- **Only the synchronous `rmi_proxy_*`/`wish_*` calls** are bound -- no
  `rmi_future_handle`/`_async` variants.
- **File transfer** is byte-array only (`uploadFile`/`downloadFile`) -- the
  local-path streaming variants (`wish_upload_file_from_path`/
  `wish_download_file_to_path`/`wish_upload_package_from_path`) aren't
  bound; Android's scoped-storage model makes a raw filesystem path a poor
  fit for this binding's API compared to a `byte[]`/`Uri` the caller already
  has in hand.
- **Native automation** (`wish_automation_*`) isn't bound -- it's desktop
  dev/test tooling for the SDL3 renderer (see the Python binding's "Native
  automation" section above), not something a deployed Android client
  needs.
- **`Dynamic`** covers named-field scalar/vector access, serialization, and
  JSON -- indexed (numeric) field access, class/method registration
  (`bison_add_class`/`bison_add_method`), and YAML text interop aren't
  exposed, matching bison's own Android binding's documented gaps. Unlike
  bison's binding, though, **`Proxy.onEvent` is exposed** here -- driving a
  UI template's button clicks and other server-pushed events is this
  binding's whole point.

None of these are architectural dead ends -- each is a straightforward
extension of the same JNI-glue-plus-Java-wrapper shape already in place for
the rest of the surface (see `bindings/android/jni/wish_jni.cpp` and
`wish_rmi_jni.cpp`).
