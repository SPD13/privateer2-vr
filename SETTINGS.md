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
| `RETICLE_GLASS_ALPHA` | `0.5` | Crosshair opacity on the glass (0..1). |
| `RETICLE_IN_EYES` | `1` | Used when `RETICLE_GLASS=0`. `1` = draw the crosshair into the 3D image at infinity, so nearer ships cover it. `0` = separate layer on top. |
| `RETICLE_DIST` | `30` | Crosshair distance on the ship's boresight when it is not on the glass. `500` ≈ optical infinity, like a real HUD. `0` = keep it in the HUD panel. The template uses `500`. |
| `RETICLE_MIN_DIST` | `1.5` | Closest the crosshair/marker plane may come. |
| `RETICLE_DYNAMIC` | `0` | Experimental: move the crosshair to the depth of whatever is under it. |
| `RETICLE_SIZE` | `0.20` | Size of the crosshair region as a fraction of the HUD width. It must contain the whole crosshair cluster, including the pitch/yaw ladders. |

The target bracket (octagon) and the lead indicator are always placed at the 3D position and depth of the targeted object. Direction arrows stay on the HUD plane. The markers are never clipped at the edge of the original screen.

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
| `CURSOR_FIX` | `1` | PAD screen: draw the mouse cursor, its shadow and its background erase exactly under the pointer. The original game shifts all three by the PAD screen's origin (60,23). `0` = original behaviour. |

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
| `KEY_<COMMAND>` | per command | Override the game key a command presses, as a DirectInput scancode in decimal. `<COMMAND>` is the action name in upper case, e.g. `KEY_FIRE_GUNS=57` (Space), `KEY_JUMP=36` (J). Commands flagged with Alt in `commands.h` keep their Alt modifier. |

## Testing automation

| Setting | Default | Effect |
|---|---|---|
| `AUTOPLAY` | `0` | Replay `macro.txt` at game start (e.g. menus → into space). Real input is blocked during the replay. The replay pauses while the game window is not focused. |
| `AUTORECORD` | `0` | Start recording an input macro at game start. Ctrl+F2 stops and saves to `macro.txt`. |

## Diagnostics and spike tools

These are for development; leave them at their defaults for play.

| Setting | Default | Effect |
|---|---|---|
| `ACTOR_LOG` | `0` | Log 3D objects appearing near the ship. |
| `SPRITE_LOG` | `0` | Log 2D sprite draws (id, shape table, position, bounds, pointer position) outside flight. Used to find the PAD cursor. |
| `PAD_CURSOR_ID`, `PAD_CURSOR_DX`, `PAD_CURSOR_DY` | `-1`, `0`, `0` | Manual nudge: move every draw of sprite `PAD_CURSOR_ID` by (DX, DY) game pixels outside flight. `-1` = off. Superseded by `CURSOR_FIX`. |
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
