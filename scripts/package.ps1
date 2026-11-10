# Packs the mod for a release: build\release\privateer2-vr-<version>.zip (what the installer downloads) and
# build\release\P2VR-Installer.exe. Build first: src\build.bat (with P2VR_VERSION set) and installer\build.ps1.
#   .\scripts\package.ps1 -Version 0.1.0
param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = "Stop"
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version must be X.Y.Z, e.g. 0.1.0" }
$root = Split-Path $PSScriptRoot
$build = Join-Path $root "build"
$rel = Join-Path $build "release"
$stage = Join-Path $rel "privateer2-vr-$Version"
Remove-Item $rel -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item (Join-Path $build "p2vr.dll") (Join-Path $stage "P2VR.dll")
Copy-Item (Join-Path $build "openxr_loader.dll") $stage
Copy-Item (Join-Path $root "dist\p2vr.ini") $stage
Copy-Item (Join-Path $root "README.md") $stage
Copy-Item (Join-Path $root "SETTINGS.md") $stage
Set-Content (Join-Path $stage "VERSION") $Version -NoNewline
@"
Privateer 2 VR $Version

Install with P2VR-Installer.exe (from the same GitHub release): it finds the game, makes DARKVR.EXE from your own
DARK.EXE and copies these files next to it. See README.md.
"@ | Set-Content (Join-Path $stage "INSTALL.txt")

$zip = Join-Path $rel "privateer2-vr-$Version.zip"
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Remove-Item $stage -Recurse -Force
Copy-Item (Join-Path $build "P2VR-Installer.exe") $rel
Get-ChildItem $rel | Format-Table Name, Length
