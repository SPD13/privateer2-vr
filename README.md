# Privateer 2 VR

A stereo VR mod for **Privateer 2: The Darkening**, Steam 30th Anniversary Edition, played through SteamVR/OpenXR. It was developed against the Steam Frame and should work with any SteamVR headset.

The game's own Argonaut BRender engine (statically linked into `DARK.EXE`) is driven from an injected DLL:
- **3D view:** each eye is re-rendered at headset resolution from the tracked head pose.
- **Stars, space dust and lens flare:** drawn per eye.
- **HUD:** a floating panel.
- **Resolution:** full SteamVR recommended size per eye, with BRender's 2048 px check lifted and each eye frustum centred on its own field of view.
- **Shots:** they appear to leave from the wings (VR only; hits are unchanged).
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

## Install
1. Download **[P2VR-Installer.exe](https://github.com/SPD13/privateer2-vr/releases/latest/download/P2VR-Installer.exe)** and run it.
   - Windows SmartScreen may warn on first launch, because the installer is not signed. Click *More info* > *Run anyway*.
2. The installer finds the game in your Steam libraries (or GOG), or you choose its folder.
   - It shows whether the mod is installed and its version, and checks GitHub for the latest release.
3. Press **Install** (or **Update**).
   - It adds `DARKVR.EXE` (made from your own `DARK.EXE`), `P2VR.dll`, `openxr_loader.dll` and `p2vr.ini` next to the game, and a *Privateer 2 VR* shortcut. No file of the game is changed.
   - On update your `p2vr.ini` is kept; settings added in the new version are appended with their defaults.
4. Start SteamVR, then the **Privateer 2 VR** shortcut. Starting the game from Steam still plays it flat.

Run the installer again at any time to update, repair (e.g. after Steam updated the game) or uninstall.

### Manual install (developers)
1. Copy the contents of `<Steam>/steamapps/common/Privateer 2 The Darkening/Windows` to a working folder (e.g. `game/`). You can link the large FMV folders with directory junctions instead of copying them.
2. Run `python scripts/make_darkvr.py game/DARK.EXE game/DARKVR.EXE`.
3. Copy `build/p2vr.dll`, `build/openxr_loader.dll` and `dist/p2vr.ini` into the folder.
4. Start SteamVR, then run `DARKVR.EXE`.

Releases: see [RELEASING.md](RELEASING.md).

## VR controllers (SteamVR; every action is remappable in SteamVR > Controller bindings)
| Control | In flight | In menus / bases / films |
|---|---|---|
| Right stick | Pitch / yaw | - |
| Left stick | Roll (x), throttle up/down (y); click = speed zero | - |
| Right trigger | Fire guns | Left click (laser pointer) |
| Right grip | Afterburner | Right click |
| Left trigger / grip | Fire missile / match target speed | - |
| A / B | Cycle targets / nearest hostile | - |
| X / Y (left A / B) | Cycle weapons / jump | - |
| Right stick click | Recenter view | Recenter view |

Axes are written directly into the game's flight controls. Buttons are sent as the game's own keys (configurable scancodes in `p2vr.ini`).

## Controls (in game)
| Keys | Action |
|---|---|
| Ctrl+F12 | Recenter the view |
| Ctrl+F2 | Start / stop recording an input macro (`macro.txt`) |
| Ctrl+F3 | Replay the macro / abort a replay (`AUTOPLAY=1` replays at game start) |
| Ctrl+F9 | Dump eye images to BMP (debug) |
| Ctrl+F5/F6/F7 | Spike test modes (when VR is inactive) |

All tuning lives in `p2vr.ini`; every setting, its effect and default is documented in [SETTINGS.md](SETTINGS.md).

## Docs
- `SPIKE_REPORT.md`: the feasibility spike and its go/no-go result.
- `notes/RE_NOTES.md`: addresses, structures and algorithms found by reverse engineering.
