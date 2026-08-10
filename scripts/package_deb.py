#!/usr/bin/env python3
"""Build wish and produce a .deb package for Debian/Ubuntu (apt install).

Thin orchestrator around the packaging CMake already knows how to do (see
cmake/Packaging.cmake): configure a Release, headless (no SDL3/window
system) build, build it, then run `cpack -G DEB` restricted to the
"runtime" install COMPONENT -- just the CLI binaries, wish_client_dll, its
public C headers, and a copyright file, laid out under Debian's standard
/usr prefix. That's deliberately a smaller set than the release zip's
"wish" component (docs, binding sources, PATH-setup shell scripts) -- none
of that belongs in a system package installed via `apt`/`dpkg`, since apt
already puts `wish` on PATH and libwish_client.so through the dynamic
linker's default search path (via the auto-added ldconfig postinst/postrm
CPack generates for a shared object under /usr/lib).

CPACK_COMPONENTS_ALL and CPACK_PACKAGING_INSTALL_PREFIX are passed as
cpack-invocation-time `-D` overrides rather than baked into
cmake/Packaging.cmake, so the *default* `cmake --build build --target
package` / bare `cpack` path (used for the release zip) is completely
unaffected by this script.

Usage:
    python3 scripts/package_deb.py
    python3 scripts/package_deb.py --version 1.2.3 --build-dir build-release
"""

import argparse
import re
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def run(cmd, cwd=None):
    print(f"+ {' '.join(str(c) for c in cmd)}")
    subprocess.run(cmd, cwd=cwd, check=True)


def configure(build_dir: Path, generator: str, version: str | None):
    args = [
        "cmake", "-S", str(REPO_ROOT), "-B", str(build_dir),
        "-G", generator,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DWISH_ENABLE_SDL3=OFF",
        "-DWISH_ENABLE_WEB=ON",
        "-DWISH_ENABLE_AUTOMATION=OFF",
        "-DWISH_BUILD_SHARED=ON",
        "-DWISH_BUILD_TESTS=OFF",
    ]
    if version:
        args.append(f"-DWISH_PACKAGE_VERSION={version}")
    run(args)


def build(build_dir: Path):
    run(["cmake", "--build", str(build_dir), "--config", "Release",
         "--parallel"])


def cpack_deb(build_dir: Path) -> Path:
    # -D overrides take effect at cpack-run time, layered on top of (and
    # here, replacing) whatever cmake/Packaging.cmake baked into
    # CPackConfig.cmake at configure time -- see that file's "CPack DEB"
    # section comment for why this can't just be set() there instead.
    result = subprocess.run(
        ["cpack", "-G", "DEB", "-C", "Release",
         "-D", "CPACK_COMPONENTS_ALL=runtime",
         "-D", "CPACK_PACKAGING_INSTALL_PREFIX=/usr"],
        cwd=build_dir, check=True, capture_output=True, text=True,
    )
    print(result.stdout)
    match = re.search(r"package: (.+\.deb) generated", result.stdout)
    if not match:
        raise RuntimeError("cpack did not report a generated .deb path")
    return Path(match.group(1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build-release",
                         help="CMake build directory (default: build-release; "
                              "shared with package_release.py's default so a "
                              "release job can build once and package both)")
    parser.add_argument("--generator", default="Ninja")
    parser.add_argument("--output-dir", default="dist")
    parser.add_argument("--version", default=None,
                         help="Release version, e.g. 1.2.3 (default: the "
                              "CMake project version)")
    parser.add_argument("--skip-configure", action="store_true")
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()

    build_dir = (REPO_ROOT / args.build_dir).resolve()
    output_dir = (REPO_ROOT / args.output_dir).resolve()

    if not args.skip_configure:
        configure(build_dir, args.generator, args.version)
    if not args.skip_build:
        build(build_dir)

    deb_path = cpack_deb(build_dir)

    output_dir.mkdir(parents=True, exist_ok=True)
    final_path = output_dir / deb_path.name
    final_path.write_bytes(deb_path.read_bytes())
    print(f"\n.deb package: {final_path}")


if __name__ == "__main__":
    main()
