// MIT License © 2025 Binary Dice Games
//! Build script that locates and links against the precompiled
//! `wish_server` shared library.
//!
//! Identical in shape to `../build.rs` (the client crate's), only the
//! library name and its environment override differ:
//!
//!   1. `WISH_SERVER_LIB` environment variable -- a full path to
//!      `libwish_server.{so,dylib}` / `wish_server.dll`. Both the
//!      link-search directory and the library name are derived from it.
//!   2. A `build/` directory next to `bindings/rust/` (the repo's own
//!      `cmake -B build` output), trying `Release`/`Debug` subdirectories
//!      on Windows -- matching every other wish binding's search order.
//!   3. Ordinary system library search (`-lwish_server`, no explicit `-L`).
//!
//! On Linux an rpath is also emitted for the resolved library directory (in
//! cases 1 and 2) so built binaries find the `.so` at run time without
//! requiring `LD_LIBRARY_PATH` -- there is no install step for this binding.
//!
//! `wish_server` must be built explicitly -- it is gated behind a
//! non-default CMake option:
//!
//! ```sh
//! cmake -B build -DWISH_BUILD_SERVER_SHARED=ON
//! cmake --build build --target wish_server_dll
//! ```

use std::env;
use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-env-changed=WISH_SERVER_LIB");

    if let Ok(env_path) = env::var("WISH_SERVER_LIB") {
        if !env_path.is_empty() {
            let path = PathBuf::from(&env_path);
            let dir = path
                .parent()
                .map(Path::to_path_buf)
                .unwrap_or_else(|| PathBuf::from("."));
            let lib_name = library_name_from_path(&path);

            println!("cargo:rustc-link-search=native={}", dir.display());
            println!("cargo:rustc-link-lib=dylib={lib_name}");
            emit_rpath(&dir);
            return;
        }
    }

    if let Some(dir) = find_build_dir() {
        println!("cargo:rustc-link-search=native={}", dir.display());
        println!("cargo:rustc-link-lib=dylib=wish_server");
        emit_rpath(&dir);
        return;
    }

    // Fall back to the OS's normal library search path (no -L directive).
    println!("cargo:rustc-link-lib=dylib=wish_server");
}

/// Extracts the link-time library name from a full path, stripping the
/// platform-specific `lib`/`.so`/`.dylib`/`.dll` decoration the way a
/// linker's `-l` flag expects (e.g. `/path/libwish_server.so` ->
/// `wish_server`).
fn library_name_from_path(path: &Path) -> String {
    let stem = path
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("wish_server");
    stem.strip_prefix("lib").unwrap_or(stem).to_string()
}

/// Looks for `wish_server`'s shared library in `<repo_root>/build/`, the
/// same conventional location every other wish binding checks -- `Release`
/// before `Debug` on Windows. `bindings/rust/wish-server/build.rs` lives
/// three directories below the repo root.
fn find_build_dir() -> Option<PathBuf> {
    let here = env::var("CARGO_MANIFEST_DIR").ok()?;
    let repo_root = Path::new(&here).parent()?.parent()?.parent()?;
    let build_dir = repo_root.join("build");

    let candidates = [
        build_dir.join("libwish_server.so"),
        build_dir.join("libwish_server.dylib"),
        build_dir.join("Release").join("wish_server.dll"),
        build_dir.join("Debug").join("wish_server.dll"),
        build_dir.join("wish_server.dll"),
    ];

    for candidate in &candidates {
        if candidate.is_file() {
            return Some(candidate.parent().unwrap().to_path_buf());
        }
    }
    None
}

fn emit_rpath(dir: &Path) {
    if cfg!(target_os = "linux") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", dir.display());
    }
}
