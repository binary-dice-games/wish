# Publishing the wish binaries to a GitHub Release

This document is the release runbook for the **downloadable binary zips**
attached to each GitHub Release (`wish-<version>-<System>-<arch>.zip`). It
covers what the automation does and the per-release steps.

For the packaging mechanics (what goes in the zip, how to build one
locally, PATH-setup scripts), see
[docs/building.md](building.md#packaging-a-release). For the `.deb`, see
[docs/building.md](building.md#building-a-deb-package). For the `wish-abi`
PyPI package, see [docs/publishing-python.md](publishing-python.md).

## How it works

Two workflows fire on `release: published`:

| Workflow | Produces | Runner(s) |
|---|---|---|
| [`.github/workflows/release-binaries.yml`](../.github/workflows/release-binaries.yml) | `wish-<version>-Linux-x86_64.zip`, `-Linux-aarch64.zip`, `-Darwin-arm64.zip`, `-Windows-AMD64.zip` | `ubuntu-latest`, `ubuntu-24.04-arm`, `macos-14`, `windows-latest` |
| [`.github/workflows/release-deb.yml`](../.github/workflows/release-deb.yml) | `wish_<version>_amd64.deb` | `ubuntu-latest` |

Each matrix job runs
[`scripts/package_release.py`](../scripts/package_release.py) with the
**Ninja** generator (on Windows, MSVC `cl.exe` is put on `PATH` first by
`ilammy/msvc-dev-cmd` — see the Windows note below), then
`gh release upload <tag> dist/*.zip --clobber` attaches the result to the
release that triggered the run. `--clobber` makes re-runs idempotent.

The zip version comes from the git tag with the leading `v` stripped
(`v1.2.3` -> `1.2.3`), passed as `package_release.py --version`. Keep tags
in the `v<version>` form — `release-deb.yml` derives its version the same
way.

### Platform notes

- **Windows** zips are MSVC (`cl.exe`) builds via the Ninja generator. The
  build step runs under `bash`, which strips the `ProgramFiles(x86)` env
  var that CMake's "Visual Studio" generator relies on to find a VS
  instance, so the workflow sets up the compiler explicitly with
  `ilammy/msvc-dev-cmd` and builds with Ninja instead. The zip does **not**
  bundle the mingw runtime DLLs (that path in `package_release.py` needs
  `ldd`/`/mingw64`, i.e. MSYS2), so end users need the
  [Microsoft Visual C++ redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe).
  An MSYS2 zip can still be produced by hand (`package_release.py` from an
  MSYS2 shell) if a self-contained mingw build is wanted.
- **macOS** builds arm64 only (`macos-14`). Intel Macs are not covered by a
  prebuilt zip; those users build from source.
- **Linux** zips are dynamically linked against the system X11/Wayland/GL
  libraries (same runtime requirement as any SDL3 app — see
  [docs/publishing-python.md](publishing-python.md#the-one-runtime-caveat-sdl3-display-libraries)).
- **C# binding DLLs** are bundled only when the `dotnet` SDK is present on
  the runner (`setup-dotnet` is `continue-on-error`). If a job's log shows
  `dotnet not found ... skipping C# DLL bundle`, that zip ships the C#
  binding as source only.

## One-time setup

None. The workflows use the automatic `GITHUB_TOKEN` (`permissions:
contents: write`) — no PyPI-style Trusted Publisher, environment, or
secret. `ubuntu-24.04-arm` runners are free for public repos; if `wish`
goes private without org-enabled arm64 runners, that matrix entry queues
forever — drop it or swap in QEMU.

## Each release

1. Land everything for the release on `main`.
2. *(Optional pre-flight)* **Actions → Build and publish release binaries →
   Run workflow** on `main`. The full matrix builds and uploads the zips as
   workflow artifacts; nothing touches a release. Confirms all four runners
   are green before you tag.
3. Draft the release on GitHub (**Releases → Draft a new release**), create
   a new tag `v<version>` targeting `main`, write the notes.
4. **Publish** the release. Both workflows start.
5. Watch **Actions**. When they finish, the release has five assets: four
   `.zip`s and one `.deb`. Re-running a failed job re-attaches its asset
   (`--clobber`).

## Verify

Download a zip from the release page and, on a clean machine of that
platform:

```sh
unzip wish-<version>-Linux-x86_64.zip -d wish && cd wish
source ./wish-env.sh          # . .\wish-env.ps1 on Windows
wish --version
wish-server --help
```

## Build a zip locally

```sh
git submodule update --init --recursive
python3 scripts/package_release.py --version <version>
# -> dist/wish-<version>-<System>-<arch>.zip
```

Run once per platform. See
[docs/building.md](building.md#packaging-a-release) for `--build-dir` /
`--generator` / `--skip-configure` details.

## Gotchas

- **Tag format**: must be `v<version>`. A tag without the `v` still builds,
  but the zip version string will include whatever prefix you used.
- **`Darwin` vs `macOS`**: the zip name uses `platform.system()`, which is
  `Darwin` on macOS — `wish-<version>-Darwin-arm64.zip`.
- **`AMD64` vs `x86_64`**: Windows `platform.machine()` returns `AMD64`;
  Linux returns `x86_64`. Expected, not a bug.
- **SDL3 runtime libs on Linux** are not vendored into the zip (SDL3
  `dlopen`s them lazily). A headless CI check of `wish-server --renderer
  web` works without them; a real SDL3 window needs `libgl1 libx11-6
  libwayland-client0 libxkbcommon0`.
- **A missing `dotnet` SDK** on a runner silently downgrades that one zip
  to C#-source-only. Check each job's "Build and package" log if the C#
  DLLs matter for a release.
