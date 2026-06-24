#!/usr/bin/env python3
# MIT License © 2025 Binary Dice Games
"""Generate embedded_resources.cpp from a directory of binary assets."""

import argparse
import sys
from pathlib import Path

LICENSE_HEADER = """\
// MIT License © 2025 Binary Dice Games
// GENERATED — do not edit. Re-run CMake to regenerate.\
"""

INCLUDES = """\
#include "resource_entry.hpp"\
"""


def path_to_symbol(rel_path: str) -> str:
    """Convert 'icons/folder.png' to 'res_icons_folder_png'."""
    result = "res_"
    for ch in rel_path:
        result += ch if ch.isalnum() else "_"
    return result


def generate_cpp(resource_dir: Path) -> str:
    lines = [LICENSE_HEADER, "", INCLUDES, ""]

    if resource_dir.is_dir():
        all_files = sorted(
            (p for p in resource_dir.rglob("*") if p.is_file()),
            key=lambda p: str(p.relative_to(resource_dir)).replace("\\", "/"),
        )
    else:
        all_files = []

    entries = []  # list of (rel_path, symbol_name)

    for fpath in all_files:
        rel = str(fpath.relative_to(resource_dir)).replace("\\", "/")
        sym = path_to_symbol(rel)
        data = fpath.read_bytes()

        lines.append(f"// {rel}")
        lines.append(f"static const unsigned char {sym}[] = {{")
        for i in range(0, len(data), 16):
            chunk = data[i : i + 16]
            lines.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
        lines.append("};\n")

        entries.append((rel, sym))

    # External-linkage table — no static, so resource_store.cpp can link against it.
    lines.append(
        "// No static — external linkage required so resource_store.cpp can reference it."
    )
    lines.append("namespace bdg::wish {")
    lines.append("")

    if entries:
        # extern overrides C++'s default internal linkage for const at namespace scope.
        lines.append("extern const resource_entry g_resource_table[] = {")
        for rel, sym in entries:
            lines.append(f'  {{ "{rel}", {sym}, sizeof({sym}) }},')
        lines.append("};\n")
        lines.append(
            "extern const std::size_t g_resource_count =\n"
            "    sizeof(g_resource_table) / sizeof(g_resource_table[0]);\n"
        )
    else:
        # Zero-entry case: C++ forbids zero-length arrays, so use a sentinel entry
        # and set count to 0.  find() iterates only g_resource_count times so the
        # sentinel is never matched.
        lines.append("extern const resource_entry g_resource_table[1] = {")
        lines.append("  { nullptr, nullptr, 0 },")
        lines.append("};\n")
        lines.append("extern const std::size_t g_resource_count = 0;\n")

    lines.append("} // namespace bdg::wish")

    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Embed binary resources into a C++ translation unit."
    )
    parser.add_argument("--input", required=True, help="Source asset directory")
    parser.add_argument("--output", required=True, help="Output .cpp file path")
    args = parser.parse_args()

    resource_dir = Path(args.input)
    out_path = Path(args.output)

    if not resource_dir.exists():
        print(
            f"WARNING: Resource directory '{resource_dir}' not found; "
            "generating empty table.",
            file=sys.stderr,
        )

    new_content = generate_cpp(resource_dir)

    if out_path.exists() and out_path.read_text(encoding="utf-8") == new_content:
        sys.exit(0)  # unchanged — preserve mtime, skip recompile

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(new_content, encoding="utf-8")


if __name__ == "__main__":
    main()
