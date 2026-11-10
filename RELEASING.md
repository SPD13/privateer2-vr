# Releasing

Versions are numbered `vX.Y.Z` (for example `v0.1.0`): the minor number for a release with new features, the patch number
for fixes only.

## Publish a release

1. Commit and push everything that belongs in the release (`SETTINGS.md` and `dist/p2vr.ini` up to date).
2. Tag and push the tag:
   ```
   git tag v0.1.0
   git push origin v0.1.0
   ```
3. The **release** workflow (`.github/workflows/release.yml`) builds `P2VR.dll` (x86, MSVC) with the version compiled in,
   builds the installer, and publishes the GitHub release **Privateer 2 VR 0.1.0** with:
   - `privateer2-vr-0.1.0.zip`: `P2VR.dll`, `openxr_loader.dll`, `p2vr.ini` (defaults), `README.md`, `SETTINGS.md`, `VERSION`
   - `P2VR-Installer.exe`: the one-click installer

   The release notes list the commits since the previous tag; edit them on GitHub if needed.

The installer reads `https://api.github.com/repos/SPD13/privateer2-vr/releases/latest` and installs the `privateer2-vr-X.Y.Z.zip`
of that release, so every installer already out there installs or updates to the newest release. Users always get the newest
installer from `https://github.com/SPD13/privateer2-vr/releases/latest/download/P2VR-Installer.exe`. The repository must be
**public** for that: GitHub does not show releases of private repositories without signing in.

A test build without publishing: Actions > release > *Run workflow*. The files are in the run's artifacts.

## Build locally

```
src\build.bat                                  (set P2VR_VERSION=0.1.0 first for a versioned DLL)
powershell -File installer\build.ps1 -Version 0.1.0
powershell -File scripts\package.ps1 -Version 0.1.0
```

`build\release\` then holds the same two files as a release. To test the installer with this archive instead of the latest
release: `build\release\P2VR-Installer.exe --archive build\release\privateer2-vr-0.1.0.zip`.

The installer artwork (`installer/ui/hero.jpg`, `installer/ui/favicon.png`, `installer/icon.png`, `installer/logo.png`) is made from the game's Steam
library art by `python scripts/make_installer_art.py`.
