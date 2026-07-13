# wish optional modules

An optional module is a self-contained UI tool (server-side form logic
and/or client-side app code and/or embedded assets) that can be compiled
into a wish build without becoming a permanent part of the core framework.
See [src/ui/forms/DESIGN.md](../src/ui/forms/DESIGN.md)'s "Module System"
section for the full design, including why modules are statically compiled
in rather than loaded as runtime plugins.

## Layout

```
modules/<organization>/<collection>/<module>/
  server/<module>.hpp, <module>.cpp   # optional: register_<module>()
  client/<module>.hpp, <module>.cpp   # optional: run_<module>(wish_app_host&)
  resources/embedded/...              # optional: assets embedded into the binary
  README.md
```

A module needs none, some, or all three of `server/`, `client/`,
`resources/embedded/` — there's no assumption that any particular
subdirectory exists. A server-only module registers a class the client
drives directly via `wish_instantiate()`/Python `instantiate()`; a
client-only module builds its UI tree entirely from the client side against
existing wish element classes; a resources-only module just ships assets
(icons, fonts) for other modules or client code to reference.

`<organization>` groups modules by who maintains them (e.g. `bdg` for
wish's own bundled tools); `<collection>` groups related modules within an
organization (e.g. `desktop`, wish's bundled desktop-tool collection). This
exists so a flat `modules/<name>/` namespace doesn't collide as more teams
contribute modules to the main branch.

## Adding an in-tree module

1. Create `modules/<org>/<collection>/<name>/` with whichever of
   `server/`, `client/`, `resources/embedded/` the module needs — see
   [src/ui/forms/DESIGN.md](../src/ui/forms/DESIGN.md#adding-a-new-module)
   for the exact per-file contract.
2. Add a `README.md` describing the module.
3. Register it in the root `CMakeLists.txt`, either individually:
   ```cmake
   wish_add_module(<org>/<collection>/<name>)
   ```
   or by adding it to (or relying on auto-discovery within) a
   `wish_add_collection(<org>/<collection>)` call, which enables every
   module in the collection together via one
   `WISH_COLLECTION_<ORG>_<COLLECTION>` option, while each module's own
   `WISH_MODULE_<ORG>_<COLLECTION>_<NAME>` option can still be individually
   overridden.
4. If the collection's README doesn't already list it, add it there too.

No edits to `registry.cpp` or `app_registry.cpp` are ever required.

## Adding an out-of-tree (3rd-party) module

A project that depends on wish — via `add_subdirectory()` or
`FetchContent`, keeping its module's source in its own repo — can register
its own module without editing anything inside the wish source tree or
forking it:

```cmake
add_subdirectory(extern/wish)
wish_add_module(acme/tools/mymodule MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mymodule)
wish_generate_module_registry()
wish_finalize_app_modules()
wish_generate_embedded_resources()
```

Still declare a full `<org>/<collection>/<name>` path (`MODULE_DIR` points
at wherever the module actually lives on disk) so its CMake option name and
embedded-resource path follow the same convention as wish's own modules and
don't collide with them. See
[src/ui/forms/DESIGN.md](../src/ui/forms/DESIGN.md#out-of-tree-modules-3rd-party-projects)
for the full explanation of why this is source-level (not a runtime
plugin), the three finalize calls above, and why they must run after every
`wish_add_module()`/`wish_add_collection()` call.

**Naming collisions**: the generated server-side registry calls
`register_<name>()` (the module's leaf name only) — this is **not**
automatically qualified by org/collection, so two different orgs shipping a
same-leaf-name module's *server* code in the same build get a real C++ link
error (register_calculator() defined twice). Pick distinctive leaf names,
or fully-qualified `register_<org>_<collection>_<name>()` function names,
to avoid this.

A module's client *app* name (whatever string it passes to `register_app()`
— by convention the same as its leaf name, e.g. `"calculator"`) is a
different story: two modules registering the same app name is expected to
happen and is handled, not just avoided by convention. Both remain fully
visible in `--list`/`wish_list_apps()` (shown qualified, e.g.
`bdg/desktop/calculator` and `microsoft/tools/calculator`), and
`--run=<name>`/`--describe=<name>`/`wish_run_app()` accept either the short
name (when only one module registers it) or the fully-qualified
`organization/collection/name` form (always unambiguous). If the short name
matches more than one registered app, the short form fails with a
clear "ambiguous between: ..." error listing the fully-qualified
candidates, instead of silently running whichever happened to register
first — see `resolve_app()`/`register_app()` in `src/client/app_registry.hpp`.
