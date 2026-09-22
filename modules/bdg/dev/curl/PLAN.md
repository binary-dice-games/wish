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
   dependencies — added after the first response-parsing bug found in
   step 9 below, to give it a fast regression test). *Done, all 29 cases
   pass.*
9. **Live verification, round 1** — built `wish-cli`/`wish-standalone`
   with `-DWISH_MODULE_BDG_DEV_CURL=ON` and drove the real module
   end-to-end via the automation module (`docs/automation.md`) against
   `https://httpbin.org/get`, then `https://www.google.com`. Found and
   fixed two real response-parsing bugs (see DESIGN.md "Command
   construction & response parsing" §6 for the full explanation of each):
   a JSON body ending in its own trailing newline was silently dropped;
   a large HTML body containing its own coincidental blank-line sequence
   was truncated and partly misparsed as headers. `parse_curl_output()`
   was extracted into its own dependency-free `curl_response_parser.hpp/
   .cpp` with dedicated regression tests for both. *Done.*
10. **Live verification, round 2** — requested separately by the user
    ("validate that all the curl commands work as expected"): cycled all
    seven HTTP methods (GET/POST/PUT/PATCH/DELETE/HEAD/OPTIONS) against
    `https://httpbin.org/anything` in one browser session, plus a fresh
    session each for a `-L` redirect chain and 404/500 status colouring.
    Found and fixed two more real bugs (see DESIGN.md §6): `HEAD`
    requests failed with curl exit 18 (missing `--head`) even though they
    actually succeeded; the Response Body pane silently kept showing the
    *first* response's content for every request after it within the
    same session, because every response reused the same sandbox
    filename (`curl_response.txt`) and the `TextEditor`'s `file_path`
    field therefore never visibly changed. Also, while investigating why
    the Environments/Collections/History dock tabs seemed unswitchable
    through automation, found that was a **testing-technique mistake**,
    not a real limitation — see docs/automation.md's corrected entry;
    `docs/automation.md` was updated accordingly (it previously
    documented the wrong conclusion from round 1's own investigation).
    *Done.*

## Verification

- [x] Configure with `-DWISH_MODULE_BDG_DEV_CURL=ON` and build; confirm
      `server/curl.cpp`/`client/curl*.cpp` compile cleanly and `wish
      client --list` shows `bdg/dev/curl`.
- [x] `test_curl`, `test_curl_process`, and `test_curl_response_parser`
      all pass (29/29).
- [x] End-to-end via the automation module against real endpoints:
      - `GET https://httpbin.org/get?probe=1`, `GET
        https://www.google.com` — status/timing/size render correctly,
        colour-coded green for 200; the response body renders
        pretty-printed/syntax-highlighted (JSON and, for google.com, a
        large HTML/JS document) in the Body `TextEditor` with no
        truncation; the Headers table populates correctly; the Console
        traces the exact `curl` invocation with exit code 0.
      - All seven methods against `https://httpbin.org/anything` in one
        session — each gets 200 with the correct method echoed back in
        the response body (HEAD/OPTIONS correctly show a 0 B body, not a
        failure), and — critically — the Body pane visibly updates to
        the *current* response every time, not the first one.
      - `GET https://httpbin.org/redirect/2` — followed through both
        hops to the final `200`, body/headers reflect the final
        destination only, not an intermediate hop.
      - `GET https://httpbin.org/status/404` / `/status/500` — status
        line renders in red for both, body correctly empty.
      - Adding/removing a Params row affects only that row; the "Save
        as" flow writes a correctly-shaped entry (id, collection, name,
        and the full request state) into the local persistent store
        file.
- [ ] **Not re-exercised live in this pass** (implemented and covered by
      `test_curl.cpp`'s server-side unit tests — row rebuild,
      `MessageBox` confirm → `delete_*_requested`,
      `update_request_builder` field restoration — and, for
      substitution, by code review of `resolve_environment()`/
      `substitute_vars()`): reloading a saved/history entry back into the
      builder, deleting a saved request/environment through the confirm
      dialog, and environment `{{name}}` substitution actually resolving
      at send time. Switching the Environments/Collections/History dock
      tabs to drive these live is now known to work fine through
      automation (see docs/automation.md) — this is a "didn't get to it
      yet" gap, not a blocked one. Pick these up first if this module
      gets touched again.
- [x] `docs/automation.md` updated with what was learned across both
      rounds: the two response-parsing pitfalls (cross-referenced to
      DESIGN.md, not duplicated there), and — importantly — a correction
      to round 1's own dock-tab entry, which had wrongly concluded dock
      tabs couldn't be switched through automation at all; round 2 found
      the real cause (missing `delay=60` on a raw `page.mouse.click()`,
      and verifying with a fixed `time.sleep()` instead of `wait_for()`
      polling) and confirmed dock tabs, `Combo` popups, and `TabItem`s all
      switch reliably once both are right.

Status: **implemented and live-verified** (DESIGN.md §10) across the
request/response parsing (all HTTP methods, redirects, error status
colouring, large/binary-ish/embedded-blank-line bodies), Console trace,
Params/Headers editing, and Save-to-collection paths — four real bugs
found and fixed across two rounds of live testing. Reload/delete-confirm/
substitution remain "unit-tested + known-drivable, not yet re-watched
live" — the next natural follow-up, not a known problem area.
