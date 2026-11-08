# Privateer 2 VR

A stereo VR mod for **Privateer 2: The Darkening**, Steam 30th Anniversary Edition, played through SteamVR/OpenXR. It was developed against the Steam Frame and should work with any SteamVR headset.

The game's own Argonaut BRender engine (statically linked into `DARK.EXE`) is driven from an injected DLL:
- **3D view:** each eye is re-rendered at headset resolution from the tracked head pose.
- **Stars, space dust and lens flare:** drawn per eye.
- **HUD:** a floating panel.
- **Crosshair and target markers:** placed on the ship's boresight plane, so they line up with the lasers and targets.
- **Menus, base screens and videos:** shown on a virtual screen.

No game files are included. You need your own copy of the game.

## How it hooks in
- `DARKVR.EXE` is a copy of `DARK.EXE` whose import `P2DEN.dll` is renamed to `P2VR.dll` (`scripts/make_darkvr.py`).
- `p2vr.dll` re-exports `DirectDrawCreate` from Matt Wells' Enhancement Patch (`p2den.dll`), which keeps working unchanged.
- `p2vr.dll` does three things:
  - It detours BRender scene rendering, the starfield and the HUD-marker call (`src/p2vr.cpp`).
  - It captures p2den's D3D11 output and palette, and composites the monitor view (`src/capture.cpp`).
  - It runs an OpenXR presenter on its own thread with its own D3D11 device (`src/vr.cpp`).

## Build
These steps assume Windows, Visual Studio 2026 (x86 toolset) and Python 3.
1. Download the OpenXR SDK loader release `openxr_loader_windows-1.1.x`. Extract it to `ref/openxr/` so that `ref/openxr/include` and `ref/openxr/Win32` exist.
2. Run `src/build.bat`. The output goes to `build/p2vr.dll` and `build/openxr_loader.dll`.

## Install (into a copy of the game, never the Steam folder itself)
1. Copy the contents of `<Steam>/steamapps/common/Privateer 2 The Darkening/Windows` to a working folder (e.g. `game/`). You can link the large FMV folders with directory junctions instead of copying them.
2. Run `python scripts/make_darkvr.py game/DARK.EXE game/DARKVR.EXE`.
3. Copy `build/p2vr.dll`, `build/openxr_loader.dll` and `dist/p2vr.ini` into the folder.
4. Start SteamVR, then run `DARKVR.EXE`.

## Controls (in game)
| Keys | Action |
|---|---|
| Ctrl+F12 | Recenter the view |
| Ctrl+F9 | Dump eye images to BMP (debug) |
| Ctrl+F5/F6/F7 | Spike test modes (when VR is inactive) |

All tuning lives in `p2vr.ini`: world scale, HUD placement, reticle distance, stars, dust and flare.

## Docs
- `SPIKE_REPORT.md`: the feasibility spike and its go/no-go result.
- `notes/RE_NOTES.md`: addresses, structures and algorithms found by reverse engineering.
