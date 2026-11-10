# Privateer 2 VR settings (`p2vr.ini`)

## How to configure

- All settings live in the `[VR]` section of `p2vr.ini`, next to `DARKVR.EXE` in the game folder.
- The repo ships a commented template in `dist/p2vr.ini`; copy it into the game folder on install.
- Settings are read **once at game start**. Edit the file, then restart `DARKVR.EXE`.
- Any setting missing from the file uses its default (the "Default" column below).
- Lines starting with `;` are comments.
- Booleans are `0` = off and `1` = on. Distances are in metres unless stated otherwise.
- A log of what was loaded and hooked is written to `p2vr.log` in the game folder. The previous run's log is kept as `p2vr_prev.log`.

> **Keep this file up to date:** when a setting is added, removed or its meaning changes, update this document, `dist/p2vr.ini` and the code in the same commit.

---

## VR presentation

| Setting | Default | Effect |
|---|---|---|
| `VR_ENABLE` | `1` | Show the game in the headset through OpenXR (SteamVR). `0` = flat game only. |
| `WORLD_SCALE` | `10` | Game units per metre. Scales head movement and eye separation. Higher = stronger depth / smaller-feeling world. |
| `EYE_SCALE` | `1.0` | Multiplier on SteamVR's recommended per-eye resolution. |
| `EYE_MAX` | `2048` | Largest per-eye render size in pixels (clamped 512..8192). BRender's original 2048 limit is lifted in the engine, so higher values work. The template uses `4096`. |
| `WINDOW_MAXIMIZE` | `1` | Maximise the game window at start. p2den renders the 2D layer (HUD, menus, films) at the window's size and p2vr shows that image in VR, so a bigger window makes them sharper. On a 3440×1440 monitor this gives 1825×1369 for the game's 4:3 picture instead of ~1060×797. Fullscreen (`WINDOWED=0` in p2den's settings) does not help: p2den then renders at 640×480. |
| `SPACE_PROMPT` | `1` | When a mission starts (launch into space), the game is held on its first flight frame and the headset shows "Entering Space in VR. Press the fire button to continue" (world-fixed, 1.5 m ahead). The fire button continues, on release, so the press does not also fire: the VR `fire_guns` action (right trigger by default), the mouse button(s) p2den maps to fire in space (`[MOUSE_SPACE]` `BUTTON_0n=119` in `p2_dark_en_ingame.ini`, left button by default), or the joystick button(s) mapped to *fire guns* in p2den's Alt+J tool (code `0x72` in `controllers\<vid><pid>_*.joy`; without such a mapping, joystick button 1 as in the original game). Not shown when returning from the nav map, options or other screens opened during flight. Only while the VR session is running. `0` = start flying immediately. |
| `SMOOTH` | `1` | Frame smoothing between the game's ~30 Hz simulation steps and the headset's refresh rate. `0` = off (judder at 30 Hz). `1` = interpolate (smooth, about one game frame of delay). `2` = extrapolate (no delay, may overshoot). |
| `OBSERVER_BACK` | `1.0` | The eyes sit this far behind the ship's centre, so no object can come between you and the HUD plane. |
| `SHOW_OWN_SHIP` | `0` | Draw your own ship's model (visible from behind once `OBSERVER_BACK` > 0). |

## HUD panel

The game's 2D HUD (radar, gauges, VDUs) is shown on a flat panel in front of you. In flight, black pixels are keyed out so the HUD floats over space.

| Setting | Default | Effect |
|---|---|---|
| `HUD_DIST` | `1.0` | Distance of the HUD panel. |
| `HUD_WIDTH` | `0` | Panel width. `0` = exactly the game's field of view at `HUD_DIST`, so HUD markers line up with the 3D world. |
| `HUD_Y` | `0` | Vertical offset of the panel (positive = up). |
| `HUD_CLEAR` | `1` | Clear the game's own 3D image after it is captured, so the HUD layer contains only HUD graphics. Keep at `1`. |
| `HUD_KEY_THRESHOLD` | `0.03` | Brightness below which HUD pixels become transparent. |
| `HUD_KEY_GAIN` | `40` | Steepness of the transparency edge (higher = harder edge, lower = softer/more see-through HUD). |

## Crosshair and target markers

| Setting | Default | Effect |
|---|---|---|
| `RETICLE_GLASS` | `1` | Draw the gun crosshair on the HUD glass (the HUD panel plane) with `RETICLE_GLASS_ALPHA` opacity. Takes precedence over `RETICLE_IN_EYES`. |
| `RETICLE_GLASS_ALPHA` | `0.7` | Crosshair opacity on the glass (0..1). |
| `HUD_ARROW_LAYER` | `1` | The yellow target-direction arrow (the triangle the HUD draws around the crosshair, pointing to the target) is not drawn into the HUD image but redrawn by p2vr as its own layer on the HUD glass, together with the other direction arrows. It is never cut by the HUD panels (`HUD_PANELS`) or the screen edge and passes behind the panels. It follows one game frame (1/30 s) later than the HUD. `0` = leave it in the HUD image. |
| `RETICLE_IN_EYES` | `1` | Used when `RETICLE_GLASS=0`. `1` = draw the crosshair into the 3D image at infinity, so nearer ships cover it. `0` = separate layer on top. |
| `RETICLE_DIST` | `30` | Crosshair distance on the ship's boresight when it is not on the glass. `500` ≈ optical infinity, like a real HUD. `0` = keep it in the HUD panel. The template uses `500`. |
| `RETICLE_MIN_DIST` | `1.5` | Closest the crosshair/marker plane may come. |
| `RETICLE_DYNAMIC` | `0` | Experimental: move the crosshair to the depth of whatever is under it. |
| `RETICLE_SIZE` | `0.20` | Size of the crosshair region as a fraction of the HUD width. It must contain the whole crosshair cluster, including the pitch/yaw ladders. |

The target bracket (octagon) and the lead indicator are always placed at the 3D position and depth of the targeted object. Direction arrows (including the HUD's target-direction arrow, see `HUD_ARROW_LAYER`) are separate layers on the HUD plane, drawn behind the HUD panels. The markers are never clipped at the edge of the original screen.

## HUD panels

With `HUD_PANELS=1`, parts of the HUD are cut out of the flat HUD and shown as separate panels on a sphere around your head, like cockpit instruments. Every panel automatically faces you, so looking at any of them feels natural. Anything not covered by a panel (crosshair, messages, other pop-ups) stays on the flat HUD.

| Setting | Default | Effect |
|---|---|---|
| `HUD_PANELS` | `1` | `1` = show the panels below; `0` = one flat HUD, as before. |
| `PANEL_RADIUS` | `0.8` | Radius of the sphere in metres, i.e. how far the panels are from your head. |
| `PANEL_<NAME>` | see below | Defines one panel. `PANEL_<NAME>=0` removes it. |

Each `PANEL_<NAME>` value is 7 (or 8) comma-separated numbers:

```
PANEL_TARGET=2,342,120,478, -44,-22, 0.28
             └─ x0,y0,x1,y1 ─┘ └az,el┘ └width┘ [,radius]
```

| Part | Meaning |
|---|---|
| `x0,y0,x1,y1` | The rectangle of the original 640×480 HUD shown on this panel (top-left inclusive, bottom-right exclusive). Use a Ctrl+F9 HUD dump (`hud_*.bmp` in the game folder) to find coordinates. |
| `az` | Azimuth in degrees: `0` = straight ahead, positive = to your right, negative = to your left. |
| `el` | Elevation in degrees: `0` = eye level, positive = up, negative = down. |
| `width` | Panel width in metres; the height follows the rectangle's aspect ratio. |
| `radius` | Optional: this panel's own distance, overriding `PANEL_RADIUS`. |

At 0.8 m, 0.1 m of width covers about 7° of view; keep neighbouring panels' azimuths far enough apart so they don't overlap.

Keep rectangles tight around their window: the HUD's direction arrows circle the crosshair at about 100 px (roughly x 220–420, y 140–340), and any part of an arrow inside a panel rectangle is shown on that panel, splitting the arrow between two places.

Default panels (names are fixed). A panel only takes a compositor layer while something is drawn in it, e.g. the guns window only while it is open, so closed windows leave the layers to the 3D markers:

| Panel | HUD content | Default value |
|---|---|---|
| `PANEL_VELOCITY` | Velocity readout and bars (top centre), and the camera-view labels above them ("OBSERVER VIEW"…) | `236,0,406,58, 0,24, 0.36` |
| `PANEL_COMMS` | Comms window (top left) | `0,0,124,140, -34,22, 0.24` |
| `PANEL_TARGET` | Target VDU (bottom left) | `2,342,120,478, -44,-22, 0.28` |
| `PANEL_TARGET_SHIELDS` | Target's shield/armour ring (left of the radar) | `138,382,254,476, -22,-26, 0.24` |
| `PANEL_RADAR` | Radar and the bars below it | `254,346,388,478, 0,-28, 0.30` |
| `PANEL_SHIELDS` | Your own ship's shield/armour ring (right of the radar) | `388,382,504,476, 22,-26, 0.24` |
| `PANEL_RIGHT` | Right VDU area (bottom right) | `504,342,640,478, 44,-22, 0.28` |
| `PANEL_MISSILES` | Top-right window | `516,0,640,124, 34,22, 0.24` |
| `PANEL_GUNS` | Guns window (right, middle) | `520,124,640,264, 40,4, 0.24` |
| `PANEL_MINES` | Mines / decoys window (right, below the guns) | `520,264,640,342, 40,-11, 0.24` |

A malformed value is ignored and reported in `p2vr.log` (`[vr] PANEL_... ignored`). Each panel is one OpenXR composition layer; if the runtime's layer limit (16 on SteamVR) is reached, target markers are dropped before panels.

## Space visuals

| Setting | Default | Effect |
|---|---|---|
| `STAR_SIZE` | `2` | Star size in pixels. |
| `STAR_SPRITES` | `1` | Draw stars as sprites at infinity in each eye. |
| `DUST` | `1` | Space dust (speed cue) in VR. |
| `DUST_SCALE` | `1.0` | Dust particle size multiplier. |
| `LENS_FLARE` | `1` | Sun lens flare, with correct depth and ring transparency. |
| `LASER_WIDEN` | `3.0` | VR only: shots appear to leave from wider gun points (multiplier on the sideways offset), so the ship doesn't feel narrow. `1` = original positions. |
| `LASER_WIDEN_DIST` | `60` | Distance (game units) over which the widening fades back to the true path, so shots still converge on the crosshair. |

## Menus, films and the PAD (2D screens)

Outside flight, the game screen is shown on a virtual screen. The right controller casts a beam that moves the mouse:
- **Right trigger:** left click.
- **Right grip:** right click (cycles hotspots).
- **Left trigger:** Esc (skips films, leaves screens).
- An aim dot marks where the beam hits.

| Setting | Default | Effect |
|---|---|---|
| `SCREEN_DIST` | `2.5` | Distance of the virtual screen. |
| `SCREEN_WIDTH` | `3.2` | Width of the virtual screen. |
| `CURSOR_FIX` | `1` | Screens that draw the mouse cursor offset from the mouse position, such as the PAD (by its screen origin, 60,23) and the nav map: the VR pointer moves the mouse so that the drawn cursor lands exactly where the beam points. The drawn cursor is what the game uses for clicks. The offset is detected automatically from where the game draws its cursor. `0` = the mouse goes exactly to the beam point (the cursor then shows offset). |

## VR controllers

Every game command is a SteamVR action, remappable in **SteamVR > Settings > Controllers > Manage controller bindings > Privateer 2 VR**. Default bindings are provided for Steam Frame, Index and Touch controllers; see `src/commands.h` for the full list and defaults. The README lists the Steam Frame layout.

| Setting | Default | Effect |
|---|---|---|
| `VR_CONTROLS` | `1` | Fly with the VR controllers: right stick pitch/yaw, left stick roll/throttle. Buttons press the game's keys. |
| `STICK_DEADZONE` | `0.15` | Thumbstick deadzone (0..1). Past it, the response is rescaled with a gentle curve for fine aiming. |
| `INVERT_PITCH` | `0` | `1` = stick back pitches down (inverted). |
| `INVERT_ROLL` | `0` | Invert the roll direction. |
| `ROLL_GAIN` | `2.5` | Roll rate multiplier. |
| `THROTTLE_RATE` | `0.6` | Throttle change per second at full left-stick deflection (0..1 of max speed per second). The throttle follows the game's own setting until you move the stick. |
| `JOYSTICK_PASSTHROUGH` | `1` | Use a joystick / HOTAS together with the VR controllers. The joystick's axes and buttons are configured with p2den's in-game tool (**Alt+J**). Each axis (pitch, yaw, roll) is taken from the VR thumbsticks only while they are deflected; otherwise the joystick's value is used. The throttle follows the joystick unless the VR throttle stick is moved. For the joystick's Alt+J mapping (axes and buttons) to apply, set the game's control method to *Joystick* (or *Joystick + throttle*) in the game options. With *Mouse*, the joystick passes through only while its stick (X, Y or twist axis) is out of centre. `0` = VR sticks always override. |
| `KEY_<COMMAND>` | per command | Override the game key a command presses, as a DirectInput scancode in decimal. `<COMMAND>` is the action name in upper case, e.g. `KEY_FIRE_GUNS=57` (Space), `KEY_JUMP=36` (J). Commands flagged with Alt in `commands.h` keep their Alt modifier. |

## Controller rumble

| Setting | Default | Effect |
|---|---|---|
| `RUMBLE` | `1` | Controller vibration on/off. |
| `RUMBLE_GUNS` | `0.35` | Strength (0..1) of the short right-hand pulse for each gun shot leaving your ship. `0` = none. |
| `RUMBLE_HIT` | `0.9` | Strength (0..1) of the both-hands pulse when your ship takes damage (shields or armour drop); bigger hits rumble harder and longer. `0` = none. |

The rumble outputs are also SteamVR actions (`rumble_left`, `rumble_right`), so they can be reassigned in the SteamVR binding screen.

## Testing automation

| Setting | Default | Effect |
|---|---|---|
| `AUTOPLAY` | `0` | Replay `macro.txt` at game start (e.g. menus → into space). Real input is blocked during the replay, including the VR controllers (pointer, clicks, buttons, sticks). Pressing **Esc** (keyboard, or the VR Esc button) stops the replay: it means the sequence went wrong. The replay pauses while the game window is not focused. |
| `AUTORECORD` | `0` | Start recording an input macro at game start. Ctrl+F2 stops and saves to `macro.txt`. |

## Diagnostics and spike tools

These are for development; leave them at their defaults for play.

| Setting | Default | Effect |
|---|---|---|
| `ACTOR_LOG` | `0` | Log 3D objects appearing near the ship. |
| `CURSOR_LOG` | `0` | Every second, log the sprite and line draws that moved, with their view origin and the mouse position, to find how a screen draws its cursor (`CURSOR_FIX`). |
| `SPRITE_LOG` | `0` | Log 2D sprite draws (id, shape table, position, bounds, pointer position) outside flight. Used to find the PAD cursor. |
| `PAD_CURSOR_ID`, `PAD_CURSOR_DX`, `PAD_CURSOR_DY` | `-1`, `0`, `0` | Manual nudge: move every draw of sprite `PAD_CURSOR_ID` by (DX, DY) game pixels outside flight. `-1` = off. Not needed with `CURSOR_FIX`. |
| `MODE` | `0` | Spike test mode when VR is not active: `0` normal, `1` camera sweep, `2` side-by-side stereo. |
| `IPD`, `SWEEP_DEG`, `FOV_SCALE`, `EYE_W`, `EYE_H` | — | Parameters of the spike test modes (eye separation, sweep angle, FOV multiplier, dump size). |

## Hotkeys (keyboard, in game)

| Keys | Action |
|---|---|
| Ctrl+F12 | Recenter the VR view (also the right thumbstick click). |
| Ctrl+F2 | Start / stop recording an input macro (`macro.txt`). |
| Ctrl+F3 | Replay the macro / abort a replay. |
| Ctrl+F9 | Dump eye images to BMP (debug screenshots). |
| Ctrl+F5/F6/F7 | Spike test modes 0/1/2 (when VR is inactive). |
| Ctrl+F8 | Toggle wide FOV (spike). |
| Ctrl+F10/F11 | Halve / double the spike IPD. |
