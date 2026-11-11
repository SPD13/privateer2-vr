# Development notes: cheats, bug reports, panels, transitions (November 2026)

What was learned while building v0.1.1 – v0.1.3 and the README diagrams: game addresses, how p2den and Direct3D 11 behave, how the VR presenter decides what to show, the pitfalls hit on the way, and how the work is done day to day. Read this together with [RE_NOTES.md](RE_NOTES.md), which covers the renderer, the BRender structures, projection, smoothing and the flight controls; it is not repeated here.

Addresses are `DARK.EXE` (Steam 30th Anniversary Edition, image base 0x400000). `game` is the game-state pointer (`0x4f7a98`); "entry" is a ship entry, `entry(i) = game + i*0x3A0`; the player index is `*(int*)(game+0x2785C)`. Offsets below are from the entry unless stated.

## 1. Game data used by the cheats (`src/p2vr.cpp`, `DoCheat`)

**Ship entries**

| Data | Where |
|---|---|
| Ship type | `*(int*)(entry+0x11E6) >> 16` |
| Shields, 4 sides (short) | `+0x12DE`, `+0x12E0`, `+0x12E2`, `+0x12E4` |
| Armour, 4 sides (short) | `+0x12E6` … `+0x12EC` |
| Max shield / max armour per type (short) | `0x4C694E + type*0x48` / `0x4C6952 + type*0x48` |
| "Targetable" flag per type (byte) | `0x4C6918 + type*0x48` |
| Max speed per type (float) | `0x4C6924 + type*0x48` |
| Hull condition (0..255) | `game+0x27896` |
| Dying / exploding | `byte (entry+0x12EE) & 0x20` |
| Last attacker (short, ship index) | `+0x1222` (setting it to the player gives the player the kill, the count and the bounty) |
| Attitude to the player (short) | `+0x12DC` |
| The ship's target | `*(int*)(entry+0x1216) >> 16` (−1 = none) |
| Area (nav point) the ship is in (short) | `+0x13B2`; the player's current area is `*(int*)0x52E2FC` |
| Route destination (short) | `+0x13B4` |
| "Direct jump" flag (short) | `+0x13B6` |

Ship slots for other ships: `0x1D..0x94` (29..148).

**Damage function** `FUN_0044D98C`, Watcom registers: eax = game, edx = ship index, ebx = damage, ecx = side (−1 = all sides).
- It handles shields, armour, the explosion, the kill count and the bounty.
- The player takes no damage while `game+0x27D0C == 0`: that is how the invincible cheat works (keep it at 0, restore the saved value when turned off).
- To destroy a ship: shields to 0, armour to 1, attacker = player, then a big damage call.

**Hostile, as "target nearest hostile" counts it:** attitude ≥ 4, or attitude 3 while `*(int*)0x542288 >= 1`, or the ship targets the player. Only ships in the player's area, of a targetable type, not dying.

**Credits:** `*(int*)0x52E23C`.
- `FUN_004469E0` (eax = game, edx = amount) adds credits and shows the game's message. It needs flight data.
- On the ground the cheat adds to `0x52E23C` directly.

**Jump routing:** the next stop of a jump is `*(uint8_t*)(0x4CDADC + here*0xE2 + dest)`, with here = `+0x13B2` and dest = `+0x13B4`. Setting the direct flag `+0x13B6` makes the next jump go straight to the destination.

**Landing:** the flight loop lands the player as soon as `game+0x1144` (landing location) is not −1. `FUN_00438137` handles the location.
- The game sets it after the approach and comms: `FUN_0043BF47` for a base, `FUN_0044B180` for a planet.
- Nearest base of the area: `game+0x278F4` (location = base + 0x12). Base 0 is allowed only when `*(int*)0x52D2A8 != 0`.
- Planet: `game+0x278FC`.
- The "land now" cheat writes the same value directly.

**Game speed (fast forward):**
- p2den paces its 30 Hz loop on QueryPerformanceCounter. The hooked p2den QPC is scaled (`Smooth_SetSpeed`, `src/smooth.cpp`). It uses a double-buffered `ClockScale` so the scale changes without a time jump.
- The game clock itself is the Miles timer: `AIL_set_timer_frequency` through the import slot `0x570500`, with the timer handle at `*(int*)0x4F04EC`. Normal is 100 Hz; 2x is 200 Hz.
- Both must change for a real 2x.

**Calling Watcom functions from C++:** inline asm with `pushad`, load eax/edx/ebx/ecx, `mov esi, addr` / `call esi`, `popad` (see `GameDamage`, `GameAddCredits`).

## 2. Cheat input and messages

**Inputs:**
- **Keys:** Ctrl+1..8 (`src/macro.cpp` swallows them so the game never sees them; `Macro_TakeCheatKeys`).
- **SteamVR actions:** `cheat_*`. They are entries of `kVrCommands` (`src/commands.h`) with scancode 0, which means "no game key, handled by p2vr".
- **Joystick:** `CHEAT_<NAME>_BUTTON` in `p2vr.ini`.
- p2den's own Alt+J joystick tool was not patched.

**Where cheats run:**
- In flight, from `PostEnd` (`RunCheats`).
- `IdleTick` is not called on the ground, so the ground cheats (cash, speed), the ground Ctrl+F9 capture and the `msgtest` file trigger run on their own thread: `CreateThread`, `Sleep(20)` loop, `GroundCheats()`.

**Red confirmation message (`CheatMessage`, `DrawCheatOverlay`)**, drawn into the game's 8-bit frame so it shows on the monitor and in the headset:
- **In flight** there is no hooked frame upload. The message is drawn into `g_lastCol` at the end of `PostHud`.
- **On the ground** it is drawn through the capture overlay callback, on a copy of p2den's 8-bit frame upload.
- **Colour:** pick the palette index with the best "red" score (`2R−G−B+R/2−1.5|G−B|`), on a black box (index 0) with an 8 px margin.
- **Palette orientation:**
  - The flight HUD layer is displayed with red and blue swapped (see §4).
  - Ground screens vary.
  - The swap is detected per screen by comparing frame indices with p2den's RGB readback (`Capture_GetHudRGB`), every frame, before the "is a message showing" check.

## 3. Direct3D 11 / p2den facts (`src/capture.cpp`)

**Context hooks:**
- The device context vtable is **per object**, and d3d11.dll **replaces the context's function table after startup**. Hooks installed at device creation are lost.
- Fix: `HookContext()` runs on every Present ("presenting device"). It keeps the original functions per vtable (`CtxOrig`). When it finds its entries replaced, it chains to the new functions and patches again.
- A thread-local re-entry guard (`tDepth`) sends nested calls (another hook calling back into ours) to the first originals, which avoids loops.
- Slots used: Map 14, Unmap 15, UpdateSubresource 48, UpdateSubresource1 116. Slot 116 is hooked only when `ID3D11DeviceContext1` shares the same vtable.
- `ID3D11DeviceContext2+` needs `<d3d11_4.h>`.

**p2den's frame:**
- On the ground, p2den uploads its 8-bit frame by **Map of a 640×480 R8 texture** (staging/dynamic) on its own device. `IsP2denRes` accepts p2den's device or the device that presents (`gFrameDevice`).
- The overlay is drawn on a copy at Unmap / UpdateSubresource.
- In flight, p2den's upload is not hooked: anything that must appear in the game image is drawn into the game frame buffer in `PostHud`.

**Screen frames:** `Vr_SetScreenSource(handle, w, h, frame)` is called from the p2den Present hook (capture.cpp). The shared texture is p2den's window back buffer, at the window size, with the 4:3 image centred: `s_uScale` / `s_uOff` crop.

## 4. Palette facts

- p2den displays the **flight HUD layer with red and blue swapped** relative to `GetPalRGBX`. RE_NOTES says the same for the HUD graphics.
- **Markers** (target brackets, missile lock, direction arrows, the snowflake) are drawn by p2vr as separate layers. Their palette (`mkPal`) must have red and blue swapped before `Vr_SubmitMarkers`, or red markers show blue in VR.
- **Captures:**
  - `hud_NN.bmp` (Ctrl+F9) is written with the raw palette, so its colours are wrong: radar and crosshair brown instead of blue, missiles and guns blue instead of red.
  - `hudraw_NN.bin` = 640×480 indices + 1024-byte RGBX palette + 640×480×3 **RGB as p2den displays it**. Use that RGB part, or swap R/B of the bmp, for documentation images.

## 5. VR presenter: flight vs flat (`src/vr.cpp`)

**Layers:**
- OpenXR on SteamVR allows **16 composition layers**.
- **Flight:** projection + marker quads + HUD quad (crosshair on the glass) + panel quads (only panels with something drawn: `s_panelMask`) + the fade quad. The marker budget is what is left.
- **Flat:** the screen quad (`SCREEN_DIST` 2.5 m, `SCREEN_WIDTH` 3.2 m) + the pointer beam + its tip.

**Flight tag for each screen frame** (`Vr_SetScreenSource`). A screen frame is flight only if all three hold since the previous screen frame:
1. **The game's own once-per-cycle eye render happened** (`Vr_NoteFlightFrame`, called from `RenderVrEyes`). The extra smoothing renders (`IdleTick` → `RenderVrCore`) do **not** count: they keep running between game cycles, and counting them tagged the first nav map frame as flight.
2. **The flight HUD was finished** (`Vr_NoteHudDone` from `PostHud`).
3. **Fewer than 3 flat-screen sprites were drawn after the HUD** (`Vr_NoteFlatDraw`).

**Why check 3 is needed:** opening the nav map (Alt+N) happens *inside* a flight cycle. The cycle renders the 3D view and draws the HUD, then the map is drawn over the same frame, and that frame is presented. Checks 1 and 2 both pass for it, so it went onto the HUD quad at 1 m for one game frame, then jumped to the flat screen at 2.5 m. Users see "the map drawn close, then pushed away" or "left/right out of sync".
- The sprite hooks (`SpriteFilter`, `SpriteDetect` → `NotePostHudDraw`) count only sprites drawn after `PostHud` (`g_frameDone`), outside our own eye renders (`g_inEye2`), and only from the call sites of the flat screens that open from flight, **0x45A000..0x460000**.
- The flight HUD itself draws ~13 sprites after `PostHud` from **0x44C265**. Counting every call site turned all of flight into "flat" (no stars, no 3D).
- Call sites seen, by screen:
  - nav map (Alt+N): 0x45AE73, 0x45ADB5, 0x45AFA3, 0x45AE46, 0x45AF6E, 0x45B0D5; cursor 0x45B6D8;
  - diary (Alt+D), options (Alt+O) and hotkeys (Alt+H): three blocks, 0x45C592..0x45CC90, 0x45D08A..0x45D526 and 0x45F400..0x45F622.
- To find a new screen's call sites, log each distinct `ret` in `NotePostHudDraw` once, then open the screen from flight.
- When the diary opens, the game runs one more plain flight cycle (only 0x44C265) between two diary frames. With the fade this is just a short dim, not a wrong frame.
- 0x42207F (cursor 0x422626) draws after a switch to flat, so it is not a first-frame case.

The shown mode is `flight = targetDone && eyes rendered < 300 ms ago && screenFlight`.

**Fade through black** (`TRANSITION_FADE`, `TRANSITION_FADE_MS`, default on, 500 ms):
- While the shown mode differs from the game's, the old view stays (last images, `UploadEyes` / `UpdateScreen` skipped: `fadeHold`) and darkens over 40% of the time.
- At full black the shown mode switches, and the new view fades in over 60%.
- The HUD and panels are hidden while fading out of flight, because they may already hold the flat frame.
- The veil is a head-locked quad (`viewSpace`, 4×4 m at 0.5 m) taking one cell of a 32-step texture: premultiplied black with alpha 0..1, written once at start-up. `imageRect` picks the cell's centre 2×2 texels.
- **Pitfall:** clearing an 8×8 swapchain image with `ClearRenderTargetView` every frame did not work: the layer was never visible. Prefer static textures plus `imageRect` selection.

**Transition diagnostics:** a per-frame log around the switches showed:
- 90 Hz frame times, 9–16 ms, around switches;
- no dropped frames;
- a clean single-frame switch between 5–6 layers (flight) and 2 (flat).

So when a user reports a glitch, first suspect *content* (which frame is shown where), then eye refocus (3D scene at infinity → flat screen at 2.5 m). Timing was never the cause. Saving frames as BMPs inside the frame loop costs ~30 ms each and causes stutters itself: never leave it on while judging smoothness.

## 6. HUD panels (`src/vr.cpp`, `PANEL_*`)

**Geometry:**
- Panels sit on a sphere, `PANEL_RADIUS` 0.8 m, centred on the head.
- `PANEL_<NAME>=x0,y0,x1,y1, azimuth, elevation, width [, radius]`.
- Position: `r*(sin az cos el, sin el, −cos az cos el)`.
- Orientation: yaw = −az, pitch = −el, so each panel faces the centre.
- `MAX_PANELS` is 12. `dist/p2vr.ini` still says "at most 8": stale comment.

**Defaults:**

| Panel | Rectangle | Az | El | Width |
|---|---|---|---|---|
| VELOCITY | 236,0,406,58 | 0 | 24 | 0.36 |
| COMMS | 0,0,124,190 | −34 | 18 | 0.24 |
| TARGET | 2,342,120,478 | −44 | −22 | 0.28 |
| TARGET_SHIELDS | 138,382,254,476 | −22 | −26 | 0.24 |
| RADAR | 254,338,388,478 | 0 | −28 | 0.30 |
| SHIELDS | 388,382,504,476 | 22 | −26 | 0.24 |
| RIGHT | 504,342,640,478 | 44 | −22 | 0.28 |
| MISSILES | 516,0,640,124 | 34 | 22 | 0.24 |
| GUNS | 520,124,640,264 | 40 | 4 | 0.24 |
| MINES | 520,264,640,342 | 40 | −11 | 0.24 |

- The VELOCITY rectangle starts at y 0 so it includes the camera-view labels ("OBSERVER VIEW"). COMMS reaches y 190 for the reply options. RADAR starts at 338 for the "REWARD n CR" line.
- Text that crosses a panel edge gets split between two distances. `GrowPanelsAroundText` grows a panel's rectangle around such text each frame, and `PanelLayout` runs in `PostHud`.
- A panel rectangle that overlaps the top of a later one is cut at that panel's top. This fixes old `PANEL_MISSILES` values reaching into GUNS.

**Own ship flames:** in the VR eyes, the actors at the player ship's origin (its engine flames, e.g. with the afterburner) are hidden for the eye renders and restored afterwards (`hiddenParts` in `RenderVrCore`), so they no longer show inside the cockpit view.

## 7. Bug reports (Ctrl+F9) and the launcher

**Capture folder:** `<game>\p2vr-reports\<session start>\capture-NN\`. It holds:
- `context.txt`: phase, credits, ship, target, nav point, area, speed, cheats;
- `p2vr.ini`, `p2vr.log` at the time;
- **in flight:** `vr_NN_L/R.bmp` (eyes), `view.bmp` (left eye at 1/4, preview for the launcher), `hud_NN.bmp`, `hudraw_NN.bin`, `markers_NN.bmp`;
- **on the ground:** `screen.bmp`.

A capture finishes in `PostHud` (flight) or after a 2 s fallback, then shows "CAPTURE N SAVED".

**Launcher** (Go + WebView2, `installer/`):
- `reports.go` lists sessions and captures and builds the zip (`Downloads\P2VR-bug-report_<session>_<capture>.zip`: capture + `installer.log` + `system.txt`).
- Routes: `/api/reports`, `/api/report-file`, `/api/report-zip`, `/api/report-reveal`.
- The "Report a bug" card is always open and reloads on window focus.
- The issue template is `.github/ISSUE_TEMPLATE/bug_report.md`.

## 8. Input macros: autoplay and recorder (`src/macro.cpp`)

- `AUTOPLAY=1` replays `macro.txt` at start (the dev copy uses it to fly into space in ~15 s). `AUTORECORD=1` records from start; Ctrl+F2 stops and saves.
- **VR input is injected** (SendInput), so the recorder used to ignore it. All of p2vr's own input carries `dwExtraInfo = P2VR_INPUT_TAG` (`0x50325652`, `src/vr.h`). The recorder accepts real input or tagged input.
- `Macro_Shutdown()` (DLL_PROCESS_DETACH) saves a recording that is still running. `Save()` drops the final Alt+X (from the Alt press, scancode 0x38, before the `X`) and everything after, so a recording that ends by quitting does not replay the quit.
- **Replay aborts on Esc.** The replay's own Esc presses echo back through the hook, so an Esc within 4 s of the replay's own Esc does not abort (`s_macroEscTick`).
- During a replay, the VR pointer is not updated, and the VR Esc button aborts.

## 9. Releases

- Push a `vX.Y.Z` tag. The release workflow builds and publishes, and the notes list the commit titles since the previous tag.
- Commit titles are not player-friendly. After the workflow, rewrite the notes with `gh release edit vX.Y.Z --notes-file …`: keep the "## Install" paragraph and write a player-facing "## Changes" list. Skip dev-only items like the macro recorder and diagnostics.
- Watch the run with `gh run watch <id> --exit-status`.
- Released this session: v0.1.1 (panel fixes), v0.1.2 (cheats), v0.1.3 (bug reports, fast forward, messages, marker colours, fade, nav map fix).

## 10. Documentation images

- `scripts/make_hud_diagram.py` → `docs/hud-positioning.png`. It is a 3D projection of the default panels on the sphere, a seated mannequin (no headset, at the user's request), and top and side views.
  - Panel contents come from `docs/hud-capture.png`, a composite 640×480 HUD capture in displayed colours.
  - The missiles, mines and guns windows and the DAMAGE window (RIGHT panel) were pasted from captures where they were open; they only show when selected.
  - Uses PIL perspective warps (`Image.transform(PERSPECTIVE)`, coefficients solved from the 4 corners) and 2× supersampling.
- `scripts/make_controller_diagram.py` → `docs/steam-frame-controls.png`. It draws arrows on a photo of the Steam Frame controllers (`../steam-frame-controllers.webp`, outside the repo) with the Frame defaults of `src/commands.h`; trigger, grip and bumper go in boxes (on the back).
  - The photo is probably Valve's product image: the user was told and chose to publish it.
- PIL multiline text accepts only the vertical anchors `a`/`m`/`d` (not `t`/`s`/`b`).

## 11. Working practices that held up

**Repository and environment:**
- Never modify the Steam install. The dev copy is `privateer2-vr\game\` (DARKVR.EXE = DARK.EXE with its `P2DEN.dll` import renamed to `P2VR.dll`; p2vr forwards to p2den). `game\p2vr.ini` is local, not in the repo: it has `AUTOPLAY=1`, `CURSOR_LOG=1` and others.
- **Build:** `cmd /c src\build.bat` (finds vcvars32 itself) → `build\p2vr.dll`. **Deploy:** copy to `game\P2VR.dll`, only when DARKVR is not running. **Start:** `Start-Process game\DARKVR.EXE -WorkingDirectory game`.
- **Line endings:** `src/vr.cpp` and `SETTINGS.md` are CRLF in HEAD, and one SETTINGS.md line (the `AUTORECORD` row) is LF-only. Python rewrites flip them, so check `git diff --stat`: a whole-file diff means line endings changed. Normalise to HEAD before committing.
- **Python edits through bash heredocs:** `\n` inside a replacement string gets mangled, and an `assert` that fails leaves the file unchanged (silently, if you don't read the error). For multi-line or escaped edits, use the Edit tool or a script file written with Write.

**Settings and git:**
- Every `p2vr.ini` setting change must update `SETTINGS.md` and `dist/p2vr.ini` together.
- Commit with `-c user.email=20162978+SPD13@users.noreply.github.com`: GitHub blocks pushes that would expose the private email. Commit and push only when the user asks.

**Testing with the user:**
- Do not change `game\p2vr.ini` test settings (e.g. AUTOPLAY) without telling the user: they tested a "broken" start sequence that was only disabled.
- Diagnose from the log before guessing. Add a targeted log, let the user reproduce, read it, and remove it afterwards (a separate commit is fine).

**Session-local, not in the repo:**
- `game\macro.txt` is the current 14.6 s "into space" recording; backups: `macro_space_20261110.txt`, `macro_backup_20261110.txt`.
- `game\controllers\` holds the copied Steam joystick mapping.
- `game\p2vr-reports\transition\` holds diagnostic frame dumps and can be deleted.

## 12. Open items

- **Missiles not visible in VR** (they are in the flat game): unresolved. A Ctrl+F9 capture with a missile in flight was requested and not yet analysed.
- **Ground message colour on bases / PAD screens:** per-screen swap detection is in place, but the user hasn't confirmed it on every screen.
- **Flat screens opened from flight other than the nav map, diary, options and hotkeys** may still show their first frame on the HUD glass for one game frame.
- **`dist/p2vr.ini` comment** "at most 8 panels" should say 12.
