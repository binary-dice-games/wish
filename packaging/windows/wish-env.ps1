# Dot-source this file to use this extracted wish release in the current
# PowerShell session only -- no persistent changes are made:
#
#   . .\wish-env.ps1
#
# For a setup that persists across new sessions, run .\install.ps1 once
# instead.
#
# Windows' DLL search order includes every directory on PATH, so putting
# bin\ on PATH is enough for wish_client.dll to be found both by the `wish`
# executables themselves and by a separate program (e.g. a C# app
# P/Invoking into it, or Python's ctypes) -- unlike Linux, no separate
# "library path" variable is needed. WISH_LIB is set anyway since
# bindings/python/wish reads it directly and skips its own search entirely
# when present.

$WishRoot = $PSScriptRoot
$env:PATH = "$WishRoot\bin;$env:PATH"
$env:WISH_LIB = "$WishRoot\bin\wish_client.dll"

Write-Host "wish is on PATH for this session (try: wish server --renderer web)"
