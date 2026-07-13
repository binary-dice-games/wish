# Building wish apps and UI with an AI agent

wish ships two [Claude Code](https://claude.com/claude-code) skills —
`wish-module` and `wish-ui` — that let you describe a UI in plain English
and have an agent build it, without you needing to know wish's widget
catalog, JSON template schema, or C++ API up front. They live in
`.claude/skills/` in this repo and are auto-discovered by Claude Code
whenever you have the wish repo open; invoke one explicitly with
`/wish-module` or `/wish-ui`, or just describe what you want and Claude
Code will pick the matching skill from its description.

Both skills share one grounding document,
[`.claude/skills/wish-shared/CONCEPTS.md`](../.claude/skills/wish-shared/CONCEPTS.md)
— the object model, the full widget catalog (by pointing at the source
files that define it, not a copy that can go stale), styling, and the
automation-driven mockup workflow described below. You don't need to read
it yourself; it's what the agent reads before writing anything.

## Which skill do I want?

| | `wish-module` | `wish-ui` |
|---|---|---|
| **Builds** | A new self-contained tool bundled *inside* the wish repo | A UI embedded into *your own, separate* application |
| **Ends up running as** | `wish client --run=<name>` (or `wish standalone --run=<name>`) | Whatever your app's own entry point is — it links `wish_client_dll` (or a Python/C# binding) and connects to a `wish server` you run alongside it |
| **Writes server-side code?** | Yes, when the tool needs its own state/logic (a `bison::dynamic` form class) | Never — purely client-side; all logic lives in your app |
| **Good for** | "Add a calculator/notepad-style tool to wish" | "Add a live status panel / graph / control window to my existing C++, Python, or C# program" |

If you're not sure which one fits, describe what you're building — the
skill descriptions are specific enough that Claude Code will generally
route to the right one, and it will ask if genuinely ambiguous.

### `wish-module`

Scaffolds `modules/<org>/<collection>/<name>/` — `server/` (if the tool has
its own state or logic beyond static layout), `client/` (the runner that
builds/wires the UI), and a `README.md` — then wires it into the build with
one `wish_add_module(...)` line in the root `CMakeLists.txt`. See
[modules/README.md](../modules/README.md) for the on-disk layout this
produces and how to build it once created
(`-DWISH_MODULE_<ORG>_<COLLECTION>_<NAME>=ON`).

Example prompts:
- "Add a module called `timer` that counts down from a duration I set and
  shows a message when it hits zero."
- "Add a notepad-style module that can open and save a text file from the
  client's machine." (the skill will follow the existing `notepad` module's
  file-handshake pattern for this)

### `wish-ui`

Adds a UI to **someone else's application** — one you already have, written
in C++, Python, or C# — purely through the client-side wish API (the C ABI,
or whichever language binding matches your app). The skill never touches
wish's server-side source; your application links against `wish_client_dll`
(or `bindings/python/wish/` / `bindings/csharp/Wish/`) and connects to a
`wish server` process you run separately.

Example prompts:
- "Add a live CPU/memory usage graph window to my existing monitoring tool,
  using wish."
- "Give my Python script a small control panel with a Start/Stop button and
  a status label."

## How the agent validates what it builds, without you knowing widgets

Both skills follow the same loop, so you never have to read or approve raw
JSON to know whether a layout is right:

1. The agent designs the UI tree (a JSON template, or a sequence of
   `instantiate()` calls) from your description.
2. It renders that UI for real — typically via `wish standalone
   --run=<name> --renderer web` (see [docs/cli.md](cli.md)) — using a build
   with `-DWISH_ENABLE_WEB=ON -DWISH_ENABLE_AUTOMATION=ON` (the automation
   module; see [src/automation/DESIGN.md](../src/automation/DESIGN.md)).
3. It takes a real screenshot with `wish.automation.AutomationClient` and
   shows it to **you** with the `Read` tool, before writing any of the real
   application logic behind it.
4. You give feedback in plain English ("make the button bigger", "the graph
   should be on the left") and the agent iterates on the mockup.
5. Once you approve the layout, the agent wires up the real logic/data and
   re-verifies end-to-end — driving clicks and inputs with the same
   automation client and checking the actual resulting state, not just that
   the code compiles.

This is the same automation workflow documented for human contributors in
the root `CLAUDE.md`'s "Automation: debugging and testing a wish UI"
section and [src/automation/DESIGN.md](../src/automation/DESIGN.md) — the
skills just drive it on your behalf.

## Prerequisites

- [Claude Code](https://claude.com/claude-code) (or another agent runtime
  that reads `.claude/skills/`) with this repository open.
- For the visual mockup/verification loop: a build configured with
  `-DWISH_ENABLE_WEB=ON -DWISH_ENABLE_AUTOMATION=ON` (see
  [docs/building.md](building.md#cmake-options)) and `pip install
  playwright && playwright install chromium` — the agent will tell you if a
  rebuild with these flags is needed and do it for you.

## See also

- [docs/cli.md](cli.md) — the `wish server`/`client`/`standalone`/`desktop`
  commands these skills build on top of.
- [docs/tutorial.md](tutorial.md) — the underlying C++ API, if you want to
  understand or modify what the agent writes.
- [modules/README.md](../modules/README.md) — the module system's on-disk
  layout and CMake wiring, for `wish-module` output.
- [DESIGN.md](../DESIGN.md) — overall wish architecture.
