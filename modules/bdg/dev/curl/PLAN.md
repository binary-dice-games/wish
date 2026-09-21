# wish curl Module — Implementation Plan

See [DESIGN.md](DESIGN.md) for the architecture this plan implements, and
the sibling [docker](../docker/DESIGN.md)/[kubectl](../kubectl/DESIGN.md)
modules for the shared client/server-split pattern this module follows.

## Steps

1. **Mock UI** (`curl_mock.json`) — all six windows as tabs in one preview
   window, validated in the `editor` tool. *Done* — layouts mirrored into
   the real per-window JSON in `server/curl.cpp`.
2. **Server form** (`server/curl.hpp`/`.cpp`) — window construction, the
   `kv_table` editable-row plumbing, `*_requested` event emission, every
   `update_*` RMI method. *Done.*
3. **Client subprocess** (`client/curl_process.hpp`/`.cpp`) — unmodified
   copy of `docker_process`'s shape, renamed. *Done.*
4. **Client source** (`client/curl_source.hpp`/`.cpp`) — argv
   construction, `-i`/`-w`-sentinel response parsing, environment
   substitution, the local JSON store. *Done.*
5. **Client wiring** (`client/curl.hpp`/`.cpp`) — `run_curl`, event-to-
   source wiring, app registration. *Done.*
6. **Build wiring** — `modules/bdg/dev/curl/` is auto-discovered by the
   existing `wish_add_collection(bdg/dev)` call in the root
   `CMakeLists.txt` (no edit needed there); `tests/CMakeLists.txt` gained
   `test_curl`/`test_curl_process`/`test_curl_response_parser` entries
   gated by `WISH_MODULE_BDG_DEV_CURL`; `modules/bdg/dev/README.md` and
   `docs/building.md`'s CMake options table each gained a row. *Done.*
7. **Docs** — this file, `DESIGN.md`, `README.md`. *Done.*
8. **Tests** — `tests/test_curl.cpp` (memory_transport, no network),
   `tests/test_curl_process.cpp` (stub binaries, no network),
   `tests/test_curl_response_parser.cpp` (pure string parsing, no
   dependencies — added after the response-parsing bug found in step 9
   below, to give it a fast regression test). *Done, all 28 cases pass.*
9. **Live verification** — built `wish-cli`/`wish-standalone` with
   `-DWISH_MODULE_BDG_DEV_CURL=ON` and drove the real module end-to-end
   via the automation module (`docs/automation.md`) against
   `https://httpbin.org`. Found and fixed one real bug this way (not
   caught by the unit tests, since `parse_curl_output()` had none at the
   time): a JSON response body ending in its own trailing newline,
   combined with the `-w` format string's leading `\n`, produced a
   spurious second blank-line boundary right before the `__WISH_CURL_
   META__` sentinel; scanning for the header/body boundary *before*
   stripping the sentinel misread that as the real split and silently
   dropped the entire response body. Fixed by stripping the sentinel
   first (see DESIGN.md "Command construction & response parsing"), then
   extracted `parse_curl_output()` into its own dependency-free
   `curl_response_parser.hpp/.cpp` with a dedicated regression test.
   *Done* — see Verification below for exactly what was and wasn't
   confirmed this way.

## Verification

- [x] Configure with `-DWISH_MODULE_BDG_DEV_CURL=ON` and build; confirm
      `server/curl.cpp`/`client/curl*.cpp` compile cleanly and `wish
      client --list` shows `bdg/dev/curl`.
- [x] `test_curl`, `test_curl_process`, and `test_curl_response_parser`
      all pass (28/28).
- [x] End-to-end via the automation module against a real GET
      (`https://httpbin.org/get?probe=1`): status/timing/size render
      correctly and colour-coded green for 200, the JSON response body
      renders pretty-printed with syntax highlighting in the Body
      TextEditor, the response Headers table populates correctly, and
      the Console traces the exact `curl` invocation with exit code 0.
      Also directly confirmed: adding/removing a Params row affects only
      that row; the "Save as" flow writes a correctly-shaped entry (id,
      collection, name, and the full request state) into the local
      persistent store file.
- [ ] **Not yet driven through the browser automation path**: reloading a
      saved/history entry back into the builder, deleting a saved
      request/environment through the confirm dialog, and environment
      `{{name}}` substitution actually resolving at send time. These are
      each covered by `test_curl.cpp`'s server-side unit tests (row
      rebuild, `MessageBox` confirm → `delete_*_requested`,
      `update_request_builder` field restoration) and, for substitution,
      by code review of `resolve_environment()`/`substitute_vars()` (plain
      string replacement, structurally similar to already-tested code
      elsewhere in the module) — but not by a live browser round trip,
      because the Response/History/Collections/Environments windows share
      one ImGui dock-tab group and bringing a background tab to front
      turned out not to be reliably drivable through
      `AutomationClient`/Playwright in this environment (see
      docs/automation.md's "docked window that is behind another tab"
      entry, added while investigating this). If you pick this up: either
      find a working way to switch that dock tab through automation, or
      temporarily give Environments/Collections/History their own
      non-tabbed default layout for one debugging session (bump
      `set_default_dock_layout()`'s version arg back afterward).
- [x] `docs/automation.md` updated with what was learned during this pass
      (the dock-tab entry above, and the existing "screenshot missing
      rows is a real bug" entry already covered the render/data-mismatch
      class of issue this bug did *not* turn out to be — the mismatch
      here was in `curl_source`'s parsing, not the renderer).

Status: **implemented and live-verified** for the request/response/
Console/Params-Headers-editing/Save-to-collection paths (see DESIGN.md
§10). The three items above (reload, delete-confirm, substitution) are
implemented and unit-tested but still only "should work by inspection +
unit test" rather than "watched happen in a real browser" — pick those up
first if this module gets touched again.
