#!/usr/bin/env bash
# Source this file to use this extracted wish release from the current
# shell session only -- no files are modified:
#
#   source ./wish-env.sh
#
# For a setup that persists across new shells, run ./install.sh once
# instead (it appends an equivalent block to your shell rc file).
#
# Linux's dynamic linker (unlike Windows') does not consult PATH when
# resolving shared libraries, so `wish`/`wish-server`/etc. being on PATH is
# not enough on its own for a *separate* program (e.g. your own C/C++/Python
# tool linking wish_client) to find libwish_client.so -- LD_LIBRARY_PATH (or
# WISH_LIB, which bindings/python/wish reads directly) is required too.

if [ -n "${BASH_SOURCE:-}" ]; then
  _wish_src="${BASH_SOURCE[0]}"
elif [ -n "${ZSH_VERSION:-}" ]; then
  _wish_src="${(%):-%N}"
else
  _wish_src="$0"
fi
_wish_root="$(cd "$(dirname "$_wish_src")" && pwd)"
unset _wish_src

export PATH="${_wish_root}/bin:${PATH}"
export LD_LIBRARY_PATH="${_wish_root}/bin:${LD_LIBRARY_PATH:-}"
export WISH_LIB="${_wish_root}/bin/libwish_client.so"
unset _wish_root

echo "wish is on PATH for this shell session (try: wish server --renderer web)"
