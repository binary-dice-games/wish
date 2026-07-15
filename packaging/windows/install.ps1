<#
.SYNOPSIS
  Persistently adds this extracted wish release's bin\ folder to your user
  PATH, so `wish`/`wish-server`/etc. and wish_client.dll are discoverable
  from any new terminal session -- no administrator rights needed (this
  edits the per-user HKCU environment, not the machine-wide one).

.DESCRIPTION
  Run once:
    powershell -ExecutionPolicy Bypass -File .\install.ps1
  Then open a *new* terminal (PATH changes don't apply to already-open
  ones) and run: wish server --renderer web

  For a one-off session instead of a persistent change, dot-source
  wish-env.ps1 instead and skip this script entirely.

.PARAMETER Uninstall
  Removes this release's bin\ folder from your user PATH again.
#>
param([switch]$Uninstall)

$WishRoot = $PSScriptRoot
$BinDir = Join-Path $WishRoot "bin"
$CurrentPath = [Environment]::GetEnvironmentVariable("Path", "User")
$Entries = @($CurrentPath -split ';' | Where-Object { $_ -ne "" })

if ($Uninstall) {
    $NewEntries = $Entries | Where-Object { $_ -ne $BinDir }
    [Environment]::SetEnvironmentVariable("Path", ($NewEntries -join ';'), "User")
    Write-Host "Removed $BinDir from your user PATH."
    Write-Host "Open a new terminal for the change to take effect."
} elseif ($Entries -contains $BinDir) {
    Write-Host "$BinDir is already on your user PATH -- nothing to do."
} else {
    $NewPath = ($Entries + $BinDir) -join ';'
    [Environment]::SetEnvironmentVariable("Path", $NewPath, "User")
    Write-Host "Added $BinDir to your user PATH."
    Write-Host "Open a new terminal for it to take effect (try: wish server --renderer web)"
    Write-Host "To undo: powershell -ExecutionPolicy Bypass -File .\install.ps1 -Uninstall"
}
