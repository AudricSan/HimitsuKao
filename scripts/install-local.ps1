<#
.SYNOPSIS
  Installs the locally built HimitsuKao plugin into the system-wide OBS plugin folder.
.DESCRIPTION
  Copies build_x64\RelWithDebInfo\himitsukao.dll and the data\ folder (locale, effects, models)
  to C:\ProgramData\obs-studio\plugins\himitsukao\. Refuses to run while OBS is running.
#>
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $root 'build_x64\RelWithDebInfo\himitsukao.dll'
$pdb = Join-Path $root 'build_x64\RelWithDebInfo\himitsukao.pdb'
$data = Join-Path $root 'data'
$dest = 'C:\ProgramData\obs-studio\plugins\himitsukao'

if (Get-Process -Name 'obs64' -ErrorAction SilentlyContinue) {
    throw 'OBS (obs64) is running. Close it first, then run this script again.'
}
if (-not (Test-Path $dll)) {
    throw "Plugin not built: $dll not found. Run: cmake --build --preset windows-x64"
}

$binDir = Join-Path $dest 'bin\64bit'
$dataDir = Join-Path $dest 'data'
New-Item -ItemType Directory -Force -Path $binDir | Out-Null
New-Item -ItemType Directory -Force -Path $dataDir | Out-Null

Copy-Item -Force $dll $binDir
if (Test-Path $pdb) { Copy-Item -Force $pdb $binDir }
Copy-Item -Recurse -Force (Join-Path $data '*') $dataDir

Write-Host "Installed to $dest"
