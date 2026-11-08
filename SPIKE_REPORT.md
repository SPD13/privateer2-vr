# Privateer 2 VR — reverse-engineering spike report

**Date:** 2026-11-08
**Verdict:** **GO**. A true stereo VR mod is technically feasible, and cheaper to run than expected.

## Summary
- `DARK.EXE` (Win95 build, 1997) renders its 3D scene with a statically linked **Argonaut BRender** (float build, Watcom register calling convention). BRender is public (MIT), so the camera, actor and pixelmap structures are known.
- An injected DLL (`p2vr.dll`) re-renders the scene a second time per frame from a shifted and rotated camera, into its own buffer, at **1440×1440**. The extra eye costs **about 0.2–1.0 ms** (max 2 ms) on the Ryzen 7 7800X3D.
- The result is correct stereo. Disparity scales with depth: 37 px for a near capital ship and 7–9 px for distant objects, at 640 px width and IPD = 1 unit.
- Matt Wells' `p2den.dll` (DirectDraw→D3D11 patch) keeps working unchanged. Our DLL sits in front of it.

## Go/no-go criteria — results
| Criterion | Result |
|---|---|
| PoC A: camera control without input | **Pass.** Yaw sweep visibly moved the planet in flight (user-confirmed). |
| PoC B: second eye fuses into correct depth | **Pass.** Same-frame re-render. Depth-dependent parallax goes the right way (`captures/stereo_ship_0{7,8}_*.png`). |
| Two renders ≤ ~11 ms at usable resolution | **Pass, by a wide margin.** About 1 ms per extra eye at 1440×1440. |
| Wide FOV has no unfixable culling | **Partly tested.** FOV ×1.6 renders cleanly with a planet. It was not tested with ships near the screen edge. |
| BRender pixelmap size limit | 2048×2048 per buffer, which is enough for PSVR2 (about 2000×2040 per eye at 100%). |

## How it works (verified facts)
- **Scene-render functions** (Watcom ABI, args in eax/edx/ebx/ecx):
  - `BrZbSceneRenderBegin` `0x46D517`
  - `BrZbSceneRenderAdd` `0x46D6F1`
  - `BrZbSceneRenderEnd` `0x46D70B`
- **Two passes per flight frame** (`0x439A3D`, and the twin at `0x43BBF4`):
  1. **Background world** `[0x51F9C8]`, camera at `game+0x26860`. Contains planets. Its camera holds orientation only and sits at the origin.
  2. **Foreground world** `[0x51F9C4]`, camera at `game+0x24428`. Contains ships and stations. The camera's local transform is identity, and its parent actor (the player ship) carries position and orientation.
- **Structures:**
  - `br_actor.t.type` is at +0x28 and the transform matrix (float[4][3]) at +0x2C. `br_actor.type_data` (the `br_camera`) is at +0x5C.
  - Camera fields: FOV (u16 angle) at +6, `hither` +8, `yon` +12, `aspect` +16. The game uses FOV 55°, hither 1, yon 1024, aspect 1.33.
- **Buffers:** the game's colour and depth buffers are fixed at **640×480**, as 8-bit palettized colour plus a 16-bit Z buffer. p2den only scales up the final image.
- **Hooking:** `DARKVR.EXE` is a copy of `DARK.EXE` with its import `P2DEN.dll` renamed to `P2VR.dll`. `p2vr.dll` re-exports `DirectDrawCreate` from `p2den.dll`. The original Steam install is untouched.

## Known gaps — work for the full mod
1. **Stars and other effects drawn outside BRender.**
   - The starfield, and probably space dust, lasers and particles, are drawn by game code between the two passes (`0x44C38C`, `0x43ED76`). That code writes straight into the 640×480 buffer.
   - It is missing from the re-rendered eye.
   - Fix: reimplement it in our renderer, or retarget the game's buffer and call it per eye. Stars at infinity are trivial.
2. **Palette not captured.** The dumps are greyscale. Our `DirectDrawCreate` wrapper is never called, probably because p2den rewrites the import entry. Fix: find the game's palette in memory, or hook p2den's palette object.
3. **HUD and target brackets.**
   - The HUD is drawn in 2D at 640×480 after the 3D passes.
   - Plan: show it as a floating panel in the headset.
   - The brackets are projected for the mono camera and would need reprojecting or redrawing per eye.
4. **Game-level culling with a wider FOV.** Not yet tested with ships at the edges of the view.
5. **Frame-rate decoupling.**
   - The game logic runs at 30 Hz.
   - Rendering is so cheap that we can **re-render the scene at 90–120 Hz with the latest head pose**. That gives correct rotation and head movement with no reprojection artefacts.
   - It needs a render thread synchronized with the game thread, because BRender is not thread-safe.
6. **World-scale calibration.** Find the real meaning of 1 game unit to set the eye separation (IPD); 1.0 currently gives reasonable depth.
7. **2D screens** (base, booth, FMVs, menus) stay on a virtual screen in the headset.

## Proposed next phase and rough effort (part-time, uncertain)
| Milestone | Content | Estimate |
|---|---|---|
| M1 OpenXR presenter | Use p2den's D3D11 device, or our own. Convert the 8-bit image to RGBA with a palette shader. Feed a stereo swapchain to SteamVR/PSVR2, apply the head pose to the cameras, show the HUD as a floating panel, and put menus/FMV on a virtual screen. | 2–4 weeks |
| M2 Decoupled 90 Hz rendering | Render thread plus game-thread synchronization; per-frame head pose. | 1–2 weeks |
| M3 Completeness | Stars and effects per eye, palette, target brackets, culling fixes, IPD/scale tuning, controls. | 2–4 weeks |

## Artifacts
- Source:
  - `src/p2vr.cpp`
  - `src/build.bat` (MSVC x86)
  - `scripts/make_darkvr.py`
  - `scripts/Decomp.java` (Ghidra headless)
- Working copy of the game: `game/`. Run `DARKVR.EXE`; settings are in `p2vr.ini`.
  - Hotkeys: Ctrl+F5/F6/F7 select mode 0/1/2, Ctrl+F8 toggles wide FOV, Ctrl+F9 dumps images, Ctrl+F10/F11 change IPD.
- Evidence: `captures/` (`stereo_ship_*_parallel.png`, `stereo_ship_*_crosseyed.png`, `right_eye_1440_*.png`, plus raw logs and dumps in `run1/` and `run2/`).
- RE notes: `notes/decomp_frame.c` (Ghidra decompilation of the frame loop and helper functions).
