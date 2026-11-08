# Privateer 2: The Darkening — reverse-engineering notes

Target binary: `DARK.EXE` from the Steam 30th Anniversary Edition. It is the Windows 95 build dated 1997-10-15 (x86, Watcom C/C++), image base 0x400000. Matt Wells' Enhancement Patch (`p2den.dll`) emulates DirectDraw on Direct3D 11 and is closed source.

## Renderer
- **Engine:** Argonaut BRender (zbrendr.c 1.36, 1995), statically linked, float build.
- **Calling convention:** Watcom register ABI, with arguments in eax, edx, ebx, ecx. Callees preserve every register except eax.
- **Scene-render functions:**

  | Function | Address |
  |---|---|
  | `BrZbSceneRenderBegin(world, camera_actor, colour_pm, depth_pm)` | 0x46D517. Prologue `56 57 C8 34 00 00`. |
  | `BrZbSceneRenderAdd(world)` | 0x46D6F1 |
  | `BrZbSceneRenderEnd()` | 0x46D70B. Body: `mov [0x55E640],0 ; ret`. |

- **Flight frame:** the main path calls the scene functions at 0x439A3D..0x439ADB; a twin path is at 0x43BBF4.
  1. Background pass. World `[0x51F9C8]`, camera `*(game+0x26860)`. Contains planets. The camera holds orientation only; its parent is the world.
  2. Dirty-rect depth clears: 0x4599C0, 0x459B10, 0x459988.
  3. Space dust (0x44C38C), then effects and actor update (0x43ED76).
  4. Foreground pass. World `[0x51F9C4]`, camera `*(game+0x24428)`. The camera's local transform is identity, and its parent is the player-ship actor.
  5. HUD markers 0x453B00 (single call site 0x439AC3), lens flare 0x445DF8, a values helper 0x454AB0, static HUD 0x454B5C.
- **Starfield:** 0x44C18C is called before the background pass, with eax = game state. Prologue `53 51 52 56 57 55`.
- **Buffers:** colour pixelmap struct at 0x51F9D0 (8-bit, 640×480), depth pixelmap at 0x51FA1C (16-bit). Row pointer table 0x521E88; centre row 0x522248.

## BRender structures (offsets verified at runtime)
- **br_actor:**

  | Field | Offset |
  |---|---|
  | next | +0x00 |
  | prev | +0x04 |
  | children | +0x08 |
  | parent | +0x0C |
  | depth (u16) | +0x10 |
  | type (u8) | +0x12 |
  | t.type (u16) | +0x28 |
  | t.mat (float[4][3], row vectors: rows 0..2 = axes, row 3 = translation) | +0x2C |
  | type_data (br_camera*) | +0x5C |

- **br_camera:**

  | Field | Offset | Value in game |
  |---|---|---|
  | type (u8) | +4 | — |
  | field_of_view (u16 br_angle, vertical) | +6 | 55° |
  | hither | +8 | 1 |
  | yon | +12 | 1024 |
  | aspect | +16 | 1.33 |

- **br_pixelmap:**

  | Field | Offset |
  |---|---|
  | pixels | +0x04 |
  | row_bytes (s16) | +0x10 |
  | type | +0x12 |
  | base_x / base_y | +0x14 / +0x16 |
  | width / height | +0x18 / +0x1A |
  | origin | +0x1C / +0x1E |

  BrZbSceneRenderBegin caps pixelmaps at 2048×2048.

## Game state (eax of the frame and star functions)
- `+0x279DC`: stars. 650 entries of 16 bytes: `float dir[3]`, u8 colour at +0xC, s16 sprite at +0xE (0xFFFF = point star, 0x28 = sun).
- `+0x279E0`: dust particles, 10 × float4.
- `+0x27858`: player ship index. Ship entries are at `game + 0x1210 + i*0x3A0`; the entry holds a pointer to the ship actor, whose matrix is at +0x2C. The ship position pointer is at entry `+0x127C - 0x1210`, i.e. `game + i*0x3A0 + 0x127C`.
- `+0x26868`: sun screen offsets (x-320, y-240) as s16 pairs; the count is at `+0x26890`.

## Projection and 2D drawing
- **Mono projection:** K = 461.0357, stored at 0x4A0F90 and 0x4A0F98. `px = 320 + x*K/-z`, `py = 240 - y*K/-z`.
- **Sprite blit:** `FUN_0041305A(view*, shapes*, id, x, y)`, cdecl.
- **Translucent blit:** `FUN_004144E0`, same signature. It blends through a 64 KB lookup table at 0x4A5C90.
- **Blit structs:**
  - `view` = `{bitmap*, x0, y0, x1, y1}` (global 0x4F1C60).
  - `bitmap` = `{pixels, w-1, h-1}` (global 0x4F1BFC).
  - Shape table pointer: `[0x51F9F4]`.
- **Dust:** 27 cell offsets (vec3) at 0x4BA1BC. The ship position is wrapped to ±32. Colour = `0xCF - |z|/8`. Points are 1 px; 2×2 when the colour index is above 0xCB.
- **Lens flare:** enabled by the byte at 0x52E061. The sun must still be colour 0xFE at its centre pixel (not occluded). Sprites and their factors along `centre + (sun - centre)*f`:

  | Sprite | f |
  |---|---|
  | 0x24 | +0.5 |
  | 0x23 | +0.33 |
  | 0x25 | +0.2 |
  | 0x24 | -0.2 |
  | 0x25 | -0.6 |
  | 0x26 | -0.5 |

## Palette
- `SetPaletteEntry(index, rgb6*)` is at 0x46B45C (cdecl). It copies 6-bit RGB into 0x4F095C and fills an 8-bit RGBX table at 0x55CEC0 plus a 16-bit lookup table at 0x4B5C90.
- p2den diverts this routine, so 0x55CEC0 stays zero. The palette p2den actually displays with is a 256×1 B8G8R8A8 texture on its own device; p2vr reads it back on the GPU.
- The game sets a 16-bit DirectDraw mode (`SetDisplayMode(640,480,16)`).
