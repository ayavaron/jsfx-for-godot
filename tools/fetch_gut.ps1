# Installs GUT into demo/addons/gut (gitignored).
# GUT 9.7.x needs Godot 4.5+; pass -Tag v9.3.0 for Godot 4.3/4.4.
param(
  [string]$Tag = "v9.7.1"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "demo/addons/gut"
$temp = Join-Path $env:TEMP ("jsfx_gut_" + [guid]::NewGuid().ToString("N"))

try {
  # git reports progress on stderr, which Windows PowerShell 5.1 turns into a
  # terminating error under "Stop"; judge success by the exit code instead.
  $ErrorActionPreference = "Continue"
  git clone --quiet --depth 1 --branch $Tag https://github.com/bitwes/Gut.git $temp 2>&1 | Out-Null
  $ErrorActionPreference = "Stop"
  if ($LASTEXITCODE -ne 0) { throw "git clone of GUT $Tag failed" }

  $newDest = Join-Path $root "demo/addons"
  New-Item -ItemType Directory -Force -Path $newDest | Out-Null
  if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
  Copy-Item -Recurse (Join-Path $temp "addons/gut") $dest
  Write-Host "GUT $Tag installed at demo/addons/gut"
} finally {
  if (Test-Path $temp) { Remove-Item -Recurse -Force $temp }
}
