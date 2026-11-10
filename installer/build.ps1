# Builds the installer: build\P2VR-Installer.exe (Windows x64, WebView2 window, application icon from installer\icon.png).
#   .\installer\build.ps1 [-Version 0.1.0]
# Needs Go. The icon resource is made with go-winres (fetched by "go run" on first use).
param([string]$Version = "dev")
$ErrorActionPreference = "Stop"
Push-Location $PSScriptRoot
try {
$out = Join-Path (Split-Path $PSScriptRoot) "build"
New-Item -ItemType Directory -Force $out | Out-Null
$fileVersion = if ($Version -match '^\d+\.\d+\.\d+$') { "$Version.0" } else { "0.0.0.0" }

go mod tidy
if ($LASTEXITCODE) { throw "go mod tidy failed" }
# the icon and the manifest (per-monitor DPI, modern controls) as a resource the Go linker picks up: rsrc_windows_amd64.syso
go run github.com/tc-hib/go-winres@v0.3.3 simply --arch amd64 --out rsrc --icon icon.png --manifest gui `
   --product-name "Privateer 2 VR Installer" --file-description "Privateer 2 VR Installer" `
   --product-version $fileVersion --file-version $fileVersion --original-filename P2VR-Installer.exe
if ($LASTEXITCODE) { throw "go-winres failed" }
try {
   $env:GOOS = "windows"; $env:GOARCH = "amd64"; $env:CGO_ENABLED = "0"
   go build -trimpath -ldflags "-s -w -H windowsgui -X main.version=$Version" -o (Join-Path $out "P2VR-Installer.exe") .
   if ($LASTEXITCODE) { throw "go build failed" }
} finally {
   Remove-Item rsrc_windows_*.syso -ErrorAction SilentlyContinue
}
Write-Host "built $out\P2VR-Installer.exe ($Version)"
} finally { Pop-Location }
