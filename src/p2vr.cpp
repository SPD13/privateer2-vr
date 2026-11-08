// p2vr.dll - Privateer 2 VR spike injector (x86 only).
// Loaded by DARKVR.EXE in place of P2DEN.dll; DirectDrawCreate is forwarded to the real p2den.dll
// (see p2vr.def). Detours BRender's BrZbSceneRenderBegin inside DARK.EXE (Watcom register ABI:
// eax=world, edx=camera actor, ebx=colour pixelmap, ecx=depth pixelmap).
//
// Modes (p2vr.ini [VR] MODE=):
//   0 = log only (structures, timing)
//   1 = PoC A: yaw sine sweep applied to the render cameras
//   2 = PoC B: stereo - main frame rendered as left eye, right eye re-rendered in the same frame
//       into private buffers; F9 dumps both eyes as BMP.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <share.h>
#include <vector>
#include "vr.h"
void Capture_Install(LogFn log);

// ---------------------------------------------------------------- addresses (DARK.EXE 1997-10-15)
static const uintptr_t ADDR_BEGIN = 0x46d517;   // BrZbSceneRenderBegin
static const uintptr_t ADDR_ADD   = 0x46d6f1;   // BrZbSceneRenderAdd(world)
static const uintptr_t ADDR_END   = 0x46d70b;   // BrZbSceneRenderEnd
static const uintptr_t ADDR_WORLD_BG = 0x51f9c8;  // background world actor *
static const uintptr_t ADDR_WORLD_FG = 0x51f9c4;  // foreground world actor *
// Palette: 256 x {r,g,b} 6-bit VGA values. (0x55cec0 holds an 8-bit copy, but p2den bypasses the routine that fills it.)
static const uintptr_t ADDR_PALETTE6 = 0x4f095c;
// Starfield (FUN_0044c18c, eax = game state). Entries at *(game+0x279dc): float dir[3]; u8 colour @+0xC; s16 sprite @+0xE
static const uintptr_t ADDR_STARS_FN  = 0x44c18c;
static const uintptr_t ADDR_SPRITE_FN = 0x41305a;   // cdecl (view*, shapes*, id, x, y)
static const uintptr_t ADDR_SHAPES    = 0x51f9f4;   // shape table pointer used for star sprites
// Lens flare (FUN_00445df8): sprites via FUN_004144e0 along sun->centre line; enabled flag byte at 0x52e061
static const uintptr_t ADDR_BLIT2     = 0x4144e0;   // cdecl (view*, shapes*, id, x, y), used for flare sprites
static const uintptr_t ADDR_FLARE_ON  = 0x52e061;
// Space dust (FUN_0044c38c): 27 cell offsets (vec3) at 0x4ba1bc, 10 particles (float4) at *(game+0x279e0)
static const uintptr_t ADDR_DUST_CELLS = 0x4ba1bc;
static const int GAME_DUST = 0x279e0, SHIP_POSPTR = 0x127c;
// HUD markers (target brackets, lead indicator, nav markers): FUN_00453b00, called once from the flight frame
static const uintptr_t ADDR_MARKERS_FN   = 0x453b00;
static const uintptr_t ADDR_MARKERS_CALL = 0x439ac3;   // E8 rel32 call site
static const int GAME_STARS = 0x279dc, GAME_SHIPIDX = 0x27858, GAME_SHIPS = 0x1210, SHIP_STRIDE = 0x3a0, NUM_STARS = 0x28a;
static const uint8_t   BEGIN_PROLOGUE[6] = {0x56, 0x57, 0xC8, 0x34, 0x00, 0x00}; // push esi; push edi; enter 0x34,0

// ---------------------------------------------------------------- BRender 1.x structures (verified offsets)
#pragma pack(push, 1)
struct br_pixelmap {           // only fields we touch
    char*    identifier;  // 0x00
    uint8_t* pixels;      // 0x04
    uint32_t _q;          // 0x08
    void*    map;         // 0x0C
    int16_t  row_bytes;   // 0x10
    uint8_t  type;        // 0x12
    uint8_t  flags;       // 0x13
    uint16_t base_x;      // 0x14
    uint16_t base_y;      // 0x16
    uint16_t width;       // 0x18
    uint16_t height;      // 0x1A
    int16_t  origin_x;    // 0x1C
    int16_t  origin_y;    // 0x1E
};
#pragma pack(pop)
static const int ACTOR_T_TYPE = 0x28;   // br_transform.type (u16)
static const int ACTOR_T_MAT  = 0x2C;   // br_matrix34 float[4][3]
static const int ACTOR_TYPE_DATA = 0x5C; // br_camera *

// ---------------------------------------------------------------- config / log
static FILE* g_log;
static int   g_mode = 0;
static float g_ipd = 0.5f;          // eye separation in game units (calibrate)
static float g_sweepDeg = 25.f;
static float g_fovScale = 1.0f;
static int   g_eyeW = 0, g_eyeH = 0;   // 0 = same as game buffer
static bool  g_vrEnable = true;
static float g_worldScale = 10.f;    // game units per metre (head motion / IPD)
static bool  g_hudClear = true;      // clear the game buffer after 3D so p2den shows only the HUD
static int   g_starSize = 2;         // VR star size in pixels
static bool  g_starSprites = true;   // draw star sprites (sun, nebulae) in VR
static uint8_t* volatile g_game;     // game state pointer captured from the starfield call
static bool  g_dust = true, g_flare = true;
static float g_dustScale = 1.0f;     // dust speck size multiplier
void Capture_SetMirror(const uint8_t* scene, int w, int h, const uint8_t* palRGBX);
bool Capture_GetPalette(uint8_t out[1024]);
static char  g_dir[MAX_PATH];

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

// ---------------------------------------------------------------- calling game BRender (Watcom ABI)
static void* g_tramp;   // executes original prologue then jumps to ADDR_BEGIN+6

static void CallBegin(void* world, void* cam, void* col, void* dep) {
    void* t = g_tramp;
    __asm {
        pushad
        mov eax, world
        mov edx, cam
        mov ebx, col
        mov ecx, dep
        call t
        popad
    }
}
static void CallAdd(void* world) {
    uintptr_t f = ADDR_ADD;
    __asm { pushad
            mov eax, world
            call f
            popad }
}
static void CallEnd() {
    uintptr_t f = ADDR_END;
    __asm { pushad
            call f
            popad }
}

// ---------------------------------------------------------------- camera helpers
struct SavedCam { uint8_t* actor; float mat[12]; uint16_t fov; float aspect; bool valid; };

static void SaveCam(SavedCam& s, uint8_t* actor) {
    s.actor = actor; memcpy(s.mat, actor + ACTOR_T_MAT, 48);
    uint8_t* cam = *(uint8_t**)(actor + ACTOR_TYPE_DATA);
    s.fov = cam ? *(uint16_t*)(cam + 6) : 0; s.aspect = cam ? *(float*)(cam + 16) : 0; s.valid = true;
}
static void RestoreCam(SavedCam& s) {
    if (!s.valid) return;
    memcpy(s.actor + ACTOR_T_MAT, s.mat, 48);
    uint8_t* cam = *(uint8_t**)(s.actor + ACTOR_TYPE_DATA);
    if (cam) { *(uint16_t*)(cam + 6) = s.fov; *(float*)(cam + 16) = s.aspect; }
    s.valid = false;
}
// Rotate camera about its local Y (yaw) and shift along its local X. Row-vector convention:
// rows 0..2 = local axes in parent space, row 3 = translation.
static void ApplyEye(uint8_t* actor, float yawRad, float xOffset) {
    float* m = (float*)(actor + ACTOR_T_MAT);
    float c = cosf(yawRad), s = sinf(yawRad);
    float x[3] = {m[0], m[1], m[2]}, z[3] = {m[6], m[7], m[8]};
    for (int i = 0; i < 3; i++) { m[i] = c * x[i] - s * z[i]; m[6 + i] = s * x[i] + c * z[i]; }
    for (int i = 0; i < 3; i++) m[9 + i] += xOffset * m[i];
}

static void ScaleFov(uint8_t* actor) {
    if (g_fovScale == 1.0f) return;
    uint8_t* c = *(uint8_t**)(actor + ACTOR_TYPE_DATA);
    if (c) *(uint16_t*)(c + 6) = (uint16_t)(*(uint16_t*)(c + 6) * g_fovScale);
}

// ---------------------------------------------------------------- state
static LARGE_INTEGER g_qpf;
static int      g_frames, g_loggedDetail;
static double   g_accMs[2], g_accEyeMs, g_maxEyeMs;
static int      g_accN[2], g_accEyeN;
static DWORD    g_lastReport;
static SavedCam g_saved[2];
static bool     g_dumpRequested;
static int      g_dumpCount;

// private right-eye buffers
static br_pixelmap g_pmC, g_pmD;
static uint8_t *g_bufC, *g_bufD;
static size_t   g_bufCSize, g_bufDSize;
static void*    g_lastCol; static void* g_lastDep;
static void*    g_cam[2];       // [0]=bg camera, [1]=fg camera of current frame
static bool     g_inEye2;

static LONGLONG Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static double Ms(LONGLONG d) { return d * 1000.0 / g_qpf.QuadPart; }

// ---------------------------------------------------------------- palette (for BMP dumps)
static IUnknown* g_palette;   // IDirectDrawPalette*
typedef HRESULT (WINAPI *PFN_DDCreate)(GUID*, void**, IUnknown*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreatePalette)(void*, DWORD, PALETTEENTRY*, void**, IUnknown*);
static PFN_CreatePalette g_origCreatePalette;
static HRESULT STDMETHODCALLTYPE HookCreatePalette(void* self, DWORD f, PALETTEENTRY* e, void** out, IUnknown* o) {
    HRESULT hr = g_origCreatePalette(self, f, e, out, o);
    if (SUCCEEDED(hr) && out && *out) { g_palette = (IUnknown*)*out; Log("CreatePalette -> %p", *out); }
    return hr;
}
static uint8_t Six2Eight(uint8_t v) { v &= 63; return (uint8_t)((v << 2) | (v >> 4)); }
static void GetPalRGBX(uint8_t out[1024]) {
    if (Capture_GetPalette(out)) return;          // what p2den actually displays with
    const uint8_t* p = (const uint8_t*)ADDR_PALETTE6;
    for (int i = 0; i < 256; i++) { out[i*4] = Six2Eight(p[i*3]); out[i*4+1] = Six2Eight(p[i*3+1]); out[i*4+2] = Six2Eight(p[i*3+2]); out[i*4+3] = 255; }
}
static void GetPal(PALETTEENTRY pe[256]) {
    uint8_t p[1024]; GetPalRGBX(p);
    for (int i = 0; i < 256; i++) { pe[i].peRed = p[i*4]; pe[i].peGreen = p[i*4+1]; pe[i].peBlue = p[i*4+2]; pe[i].peFlags = 0; }
}

static void WriteBmp(const char* name, const br_pixelmap* pm, const PALETTEENTRY* pal) {
    char path[MAX_PATH]; sprintf_s(path, "%s\\%s", g_dir, name);
    FILE* f; if (fopen_s(&f, path, "wb")) return;
    int w = pm->width, h = pm->height, stride = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh = {0x4D42, (DWORD)(54 + stride * h), 0, 0, 54};
    BITMAPINFOHEADER ih = {40, w, h, 1, 24, BI_RGB, (DWORD)(stride * h)};
    fwrite(&fh, 14, 1, f); fwrite(&ih, 40, 1, f);
    uint8_t* row = (uint8_t*)calloc(stride, 1);
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t* src = pm->pixels + (pm->base_y + y) * pm->row_bytes + pm->base_x;
        for (int x = 0; x < w; x++) { const PALETTEENTRY& p = pal[src[x]]; row[x*3] = p.peBlue; row[x*3+1] = p.peGreen; row[x*3+2] = p.peRed; }
        fwrite(row, stride, 1, f);
    }
    free(row); fclose(f); Log("wrote %s", path);
}

// ---------------------------------------------------------------- logging of structures
static void LogActor(const char* tag, uint8_t* a) {
    uint16_t tt = *(uint16_t*)(a + ACTOR_T_TYPE);
    float* m = (float*)(a + ACTOR_T_MAT);
    Log("%s actor=%p type=%u parent=%p ttype=%u", tag, a, a[0x12], *(void**)(a + 0xC), tt);
    Log("   m: [%.3f %.3f %.3f] [%.3f %.3f %.3f] [%.3f %.3f %.3f] T[%.2f %.2f %.2f]",
        m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
    uint8_t* c = *(uint8_t**)(a + ACTOR_TYPE_DATA);
    if (c) Log("   camera=%p type=%u fov=0x%04x (%.1f deg) hither=%.3f yon=%.1f aspect=%.3f",
               c, c[4], *(uint16_t*)(c + 6), *(uint16_t*)(c + 6) * 360.0 / 65536.0,
               *(float*)(c + 8), *(float*)(c + 12), *(float*)(c + 16));
    for (uint8_t* p = *(uint8_t**)(a + 0xC); p; p = *(uint8_t**)(p + 0xC)) {
        float* pm = (float*)(p + ACTOR_T_MAT);
        Log("   parent %p type=%u ttype=%u T[%.2f %.2f %.2f]", p, p[0x12], *(uint16_t*)(p + ACTOR_T_TYPE), pm[9], pm[10], pm[11]);
    }
}
static void LogPm(const char* tag, br_pixelmap* p) {
    Log("%s pm=%p pixels=%p row=%d type=%u base=(%u,%u) size=%ux%u origin=(%d,%d)", tag, p, p->pixels, p->row_bytes,
        p->type, p->base_x, p->base_y, p->width, p->height, p->origin_x, p->origin_y);
}

// ---------------------------------------------------------------- right-eye render (PoC B)
static void EnsureEyeBuffers(br_pixelmap* c0, br_pixelmap* d0) {
    static br_pixelmap c1, d1; c1 = *c0; d1 = *d0;
    br_pixelmap *c = &c1, *d = &d1;
    if (g_eyeW > 0) {
        c->base_x = c->base_y = d->base_x = d->base_y = 0;
        c->width = d->width = (uint16_t)g_eyeW; c->height = d->height = (uint16_t)g_eyeH;
        c->row_bytes = (int16_t)g_eyeW; d->row_bytes = (int16_t)(g_eyeW * 2);
        c->origin_x = d->origin_x = (int16_t)(c0->origin_x * g_eyeW / (c0->width ? c0->width : 1));
        c->origin_y = d->origin_y = (int16_t)(c0->origin_y * g_eyeH / (c0->height ? c0->height : 1));
    }
    size_t cs = (size_t)abs(c->row_bytes) * (c->base_y + c->height + 1);
    size_t ds = (size_t)abs(d->row_bytes) * (d->base_y + d->height + 1);
    if (cs > g_bufCSize) { free(g_bufC); g_bufC = (uint8_t*)malloc(cs); g_bufCSize = cs; }
    if (ds > g_bufDSize) { free(g_bufD); g_bufD = (uint8_t*)malloc(ds); g_bufDSize = ds; }
    g_pmC = *c; g_pmC.pixels = g_bufC;
    g_pmD = *d; g_pmD.pixels = g_bufD;
}
static void RenderRightEye() {
    void* wbg = *(void**)ADDR_WORLD_BG; void* wfg = *(void**)ADDR_WORLD_FG;
    if (!wbg || !wfg || !g_cam[0] || !g_cam[1] || !g_lastCol) return;
    EnsureEyeBuffers((br_pixelmap*)g_lastCol, (br_pixelmap*)g_lastDep);
    LONGLONG t0 = Now();
    memset(g_bufC, 0, g_bufCSize);
    memset(g_bufD, 0xFF, g_bufDSize);
    g_inEye2 = true;
    SavedCam s0, s1;
    SaveCam(s0, (uint8_t*)g_cam[0]); SaveCam(s1, (uint8_t*)g_cam[1]);
    // background: rotation only (infinitely far), foreground: +ipd/2
    ScaleFov((uint8_t*)g_cam[0]); ScaleFov((uint8_t*)g_cam[1]);
    if (g_eyeW > 0) for (int k = 0; k < 2; k++) {
        uint8_t* c = *(uint8_t**)((uint8_t*)g_cam[k] + ACTOR_TYPE_DATA);
        if (c) *(float*)(c + 16) = (float)g_eyeW / g_eyeH;
    }
    CallBegin(wbg, g_cam[0], &g_pmC, &g_pmD); CallAdd(wbg); CallEnd();
    memset(g_bufD, 0xFF, g_bufDSize);
    ApplyEye((uint8_t*)g_cam[1], 0, +g_ipd * 0.5f);
    CallBegin(wfg, g_cam[1], &g_pmC, &g_pmD); CallAdd(wfg); CallEnd();
    RestoreCam(s0); RestoreCam(s1);
    g_inEye2 = false;
    double ms = Ms(Now() - t0); g_accEyeMs += ms; g_accEyeN++; if (ms > g_maxEyeMs) g_maxEyeMs = ms;
}



// ---------------------------------------------------------------- VR starfield
struct GameBitmap { uint8_t* pixels; int32_t wm1, hm1; };
struct GameView   { GameBitmap* bmp; int32_t x0, y0, x1, y1; };

static void CallBlit(uintptr_t f, GameView* view, void* shapes, int id, int x, int y) {
    __asm {
        pushad
        push y
        push x
        push id
        push shapes
        push view
        call f
        add esp, 20
        popad
    }
}

static void CallSprite(GameView* view, void* shapes, int id, int x, int y) { CallBlit(ADDR_SPRITE_FN, view, shapes, id, x, y); }

struct SunHit { int x, y; float dir[3]; };
static SunHit g_suns[8]; static int g_numSuns;

// Draw the game's starfield into an eye buffer. h = eye rows (eye axes in ship space).
static void DrawStarsVr(uint8_t* buf, int W, int H, float tanH, float tanV, const float h[9]) {
    uint8_t* game = g_game;
    if (!game) return;
    const uint8_t* stars = *(uint8_t**)(game + GAME_STARS);
    uint8_t* ship = *(uint8_t**)(game + GAME_SHIPS + *(int*)(game + GAME_SHIPIDX) * SHIP_STRIDE);
    if (!stars || !ship) return;
    const float* m = (const float*)(ship + ACTOR_T_MAT);     // ship axes in world space (rows)
    float sx = (W * 0.5f) / tanH, sy = (H * 0.5f) / tanV;
    GameBitmap bmp = {buf, W - 1, H - 1};
    GameView view = {&bmp, 0, 0, W - 1, H - 1};
    void* shapes = *(void**)ADDR_SHAPES;
    g_numSuns = 0;
    for (int i = 0; i < NUM_STARS; i++, stars += 16) {
        const float* d = (const float*)stars;
        float v[3];
        for (int k = 0; k < 3; k++) v[k] = d[0] * m[k*3] + d[1] * m[k*3 + 1] + d[2] * m[k*3 + 2];   // world -> ship
        float ex = h[0] * v[0] + h[1] * v[1] + h[2] * v[2];
        float ey = h[3] * v[0] + h[4] * v[1] + h[5] * v[2];
        float ez = h[6] * v[0] + h[7] * v[1] + h[8] * v[2];
        if (ez >= -1e-4f) continue;
        int px = (int)floorf(W * 0.5f + ex / -ez * sx), py = (int)floorf(H * 0.5f - ey / -ez * sy);
        int16_t sprite = *(const int16_t*)(stars + 0xE);
        if (sprite == -1) {
            uint8_t c = stars[0xC];
            for (int yy = 0; yy < g_starSize; yy++) for (int xx = 0; xx < g_starSize; xx++) {
                int x = px + xx, y = py + yy;
                if (x >= 0 && x < W && y >= 0 && y < H) buf[y * W + x] = c;
            }
        } else if (g_starSprites && shapes && px > -300 && px < W + 300 && py > -300 && py < H + 300) {
            CallSprite(&view, shapes, sprite, px, py);
            if (sprite == 0x28 && g_numSuns < 8 && px >= 0 && px < W && py >= 0 && py < H) g_suns[g_numSuns++] = {px, py, {v[0], v[1], v[2]}};
        }
    }
}


// Lens flare, after the 3D passes. The flare is a lens effect of the (cyclopean) head view: elements are placed on
// the line sun -> head centre in head-plane coordinates, then each element direction is projected into this eye.
// That keeps them at infinity (no disparity). Only drawn if this eye still sees the sun core colour (not occluded).
static void DrawFlareVr(uint8_t* buf, int W, int H, float tanH, float tanV, const float h[9], const float hh[9]) {
    if (!g_flare || !*(volatile uint8_t*)ADDR_FLARE_ON) return;
    void* shapes = *(void**)ADDR_SHAPES; if (!shapes) return;
    GameBitmap bmp = {buf, W - 1, H - 1};
    GameView view = {&bmp, 0, 0, W - 1, H - 1};
    static const struct { int id; float f; } parts[6] = {{0x24, 0.5f}, {0x23, 0.33f}, {0x25, 0.2f}, {0x24, -0.2f}, {0x25, -0.6f}, {0x26, -0.5f}};
    float sx = (W * 0.5f) / tanH, sy = (H * 0.5f) / tanV;
    for (int i = 0; i < g_numSuns; i++) {
        if (buf[g_suns[i].y * W + g_suns[i].x] != 0xFE) continue;
        const float* d = g_suns[i].dir;
        float hx = hh[0]*d[0] + hh[1]*d[1] + hh[2]*d[2], hy = hh[3]*d[0] + hh[4]*d[1] + hh[5]*d[2], hz = hh[6]*d[0] + hh[7]*d[1] + hh[8]*d[2];
        if (hz >= -1e-4f) continue;
        float qx = hx / -hz, qy = hy / -hz;              // sun on the head's image plane (z = -1)
        for (auto& pt : parts) {
            float hd[3] = {qx * pt.f, qy * pt.f, -1.0f};   // element direction in head space
            float sd[3];                                  // -> ship space (hh rows are head axes in ship space)
            for (int k = 0; k < 3; k++) sd[k] = hh[k] * hd[0] + hh[3 + k] * hd[1] + hh[6 + k] * hd[2];
            float ex = h[0]*sd[0] + h[1]*sd[1] + h[2]*sd[2], ey = h[3]*sd[0] + h[4]*sd[1] + h[5]*sd[2], ez = h[6]*sd[0] + h[7]*sd[1] + h[8]*sd[2];
            if (ez >= -1e-4f) continue;
            CallBlit(ADDR_BLIT2, &view, shapes, pt.id, (int)floorf(W * 0.5f + ex / -ez * sx + 0.5f), (int)floorf(H * 0.5f - ey / -ez * sy + 0.5f));
        }
    }
}

// Space dust, between the background and foreground passes (as in the game). eyePos in ship units.
static void DrawDustVr(uint8_t* buf, int W, int H, float tanH, float tanV, const float h[9], const float eyePos[3]) {
    uint8_t* game = g_game;
    if (!g_dust || !game) return;
    const float* parts = *(const float**)(game + GAME_DUST);
    uint8_t* shipEntry = game + *(int*)(game + GAME_SHIPIDX) * SHIP_STRIDE;
    uint8_t* ship = *(uint8_t**)(shipEntry + GAME_SHIPS);
    const float* spos = *(const float**)(shipEntry + SHIP_POSPTR);
    if (!parts || !ship || !spos) return;
    const float* m = (const float*)(ship + ACTOR_T_MAT);
    const float* cells = (const float*)ADDR_DUST_CELLS;
    float w[3];
    for (int k = 0; k < 3; k++) { w[k] = spos[k]; while (w[k] > 32.f) w[k] -= 32.f; while (w[k] < -32.f) w[k] += 32.f; }
    float sx = (W * 0.5f) / tanH, sy = (H * 0.5f) / tanV;
    // angular size of one original 640x480 pixel, in eye pixels
    float pxScale = g_dustScale * (H * 0.5f / tanV) / 461.0357f;
    for (int c = 0; c < 27; c++) for (int i = 0; i < 10; i++) {
        const float* pp = parts + i * 4;
        float r[3] = {pp[0] - w[0] + cells[c*3], pp[1] - w[1] + cells[c*3 + 1], pp[2] - w[2] + cells[c*3 + 2]};
        float v[3];
        for (int k = 0; k < 3; k++) v[k] = r[0] * m[k*3] + r[1] * m[k*3 + 1] + r[2] * m[k*3 + 2] - eyePos[k];
        float ex = h[0] * v[0] + h[1] * v[1] + h[2] * v[2];
        float ey = h[3] * v[0] + h[4] * v[1] + h[5] * v[2];
        float ez = h[6] * v[0] + h[7] * v[1] + h[8] * v[2];
        if (ez >= -0.5f) continue;
        int col = 0xcf - ((int)floorf(-ez + 0.5f) >> 3);
        if (col < 0xb0) continue;
        int n = col > 0xcb ? 2 : 1;
        int size = (int)(n * pxScale + 0.5f); if (size < 1) size = 1;
        int px = (int)floorf(W * 0.5f + ex / -ez * sx), py = (int)floorf(H * 0.5f - ey / -ez * sy);
        for (int yy = 0; yy < size; yy++) for (int xx = 0; xx < size; xx++) {
            int x = px + xx, y = py + yy;
            if (x >= 0 && x < W && y >= 0 && y < H) buf[y * W + x] = (uint8_t)col;
        }
    }
}

// ---------------------------------------------------------------- VR eye rendering (milestone 1)
static std::vector<uint8_t> g_vrDepth;
static br_pixelmap g_vrPmC, g_vrPmD;
static double g_vrAccMs, g_vrMaxMs; static int g_vrN;
static bool g_vrDumpNext;

static void QuatRows(const float q[4], float r[9]) {   // rows = rotated basis vectors (BRender row-vector convention)
    float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2*(y*y + z*z); r[1] = 2*(x*y + w*z);     r[2] = 2*(x*z - w*y);
    r[3] = 2*(x*y - w*z);     r[4] = 1 - 2*(x*x + z*z); r[5] = 2*(y*z + w*x);
    r[6] = 2*(x*z + w*y);     r[7] = 2*(y*z - w*x);     r[8] = 1 - 2*(x*x + y*y);
}

static void SetCamProjection(uint8_t* actor, uint16_t fov, float aspect) {
    uint8_t* c = *(uint8_t**)(actor + ACTOR_TYPE_DATA);
    if (c) { *(uint16_t*)(c + 6) = fov; *(float*)(c + 16) = aspect; }
}

static std::vector<uint8_t> g_scene; static int g_sceneW, g_sceneH; static bool g_markersPending;

// After FUN_00453b00 drew the 3D-projected HUD markers into the (cleared) game buffer: take them out as their own
// layer for VR (placed on the far boresight plane with the game's exact FOV), add them to the monitor mirror, and
// clear the buffer again so p2den / the HUD panel only carries the static HUD.
extern "C" void __cdecl PostMarkers() {
    if (!g_markersPending || !g_lastCol) return;
    g_markersPending = false;
    br_pixelmap* gc = (br_pixelmap*)g_lastCol;
    int w = gc->width, h = gc->height;
    static std::vector<uint8_t> mk; mk.resize((size_t)w * h);
    for (int y = 0; y < h; y++) {
        uint8_t* row = gc->pixels + (gc->base_y + y) * gc->row_bytes + gc->base_x;
        memcpy(&mk[(size_t)y * w], row, w); memset(row, 0, w);
    }
    uint8_t pal[1024]; GetPalRGBX(pal);
    Vr_SubmitMarkers(mk.data(), w, h, pal);
    if ((int)g_scene.size() == w * h) {
        for (size_t i = 0; i < mk.size(); i++) if (mk[i]) g_scene[i] = mk[i];
        Capture_SetMirror(g_scene.data(), w, h, pal);
    }
}

static void RenderVrEyes() {
    VrEyeTarget t; VrView v[2];
    if (!Vr_GetEyeTarget(&t) || !Vr_GetViews(v)) return;
    void* wbg = *(void**)ADDR_WORLD_BG; void* wfg = *(void**)ADDR_WORLD_FG;
    if (!wbg || !wfg || !g_cam[0] || !g_cam[1] || !g_lastCol) return;
    LONGLONG t0 = Now();
    br_pixelmap* gc = (br_pixelmap*)g_lastCol; br_pixelmap* gd = (br_pixelmap*)g_lastDep;
    g_vrPmC = *gc; g_vrPmD = *gd;
    g_vrPmC.base_x = g_vrPmC.base_y = g_vrPmD.base_x = g_vrPmD.base_y = 0;
    g_vrPmC.width = g_vrPmD.width = (uint16_t)t.width; g_vrPmC.height = g_vrPmD.height = (uint16_t)t.height;
    g_vrPmC.row_bytes = (int16_t)t.width; g_vrPmD.row_bytes = (int16_t)(t.width * 2);
    size_t dsz = (size_t)t.width * t.height * 2;
    if (g_vrDepth.size() != dsz) g_vrDepth.resize(dsz);
    g_vrPmD.pixels = g_vrDepth.data();

    uint16_t fov = (uint16_t)(2.0 * atan(t.tanHalfV) / (2.0 * 3.14159265358979) * 65536.0);
    float aspect = t.tanHalfH / t.tanHalfV;
    SavedCam s0, s1; SaveCam(s0, (uint8_t*)g_cam[0]); SaveCam(s1, (uint8_t*)g_cam[1]);
    g_inEye2 = true;
    // head (cyclopean) orientation = normalised average of the two eye orientations
    float qh[4], dotq = 0; for (int k = 0; k < 4; k++) dotq += v[0].quat[k] * v[1].quat[k];
    float ql = 0; for (int k = 0; k < 4; k++) { qh[k] = v[0].quat[k] + (dotq < 0 ? -v[1].quat[k] : v[1].quat[k]); ql += qh[k] * qh[k]; }
    ql = sqrtf(ql); for (int k = 0; k < 4; k++) qh[k] /= ql;
    float hh[9]; QuatRows(qh, hh);
    for (int e = 0; e < 2; e++) {
        float h[9]; QuatRows(v[e].quat, h);
        // background camera: head rotation on top of the game's own orientation, no translation
        float* mb = (float*)((uint8_t*)g_cam[0] + ACTOR_T_MAT);
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
            mb[i*3 + j] = h[i*3] * s0.mat[j] + h[i*3 + 1] * s0.mat[3 + j] + h[i*3 + 2] * s0.mat[6 + j];
        mb[9] = s0.mat[9]; mb[10] = s0.mat[10]; mb[11] = s0.mat[11];
        // foreground camera (child of the ship): full eye pose in ship space
        float* mf = (float*)((uint8_t*)g_cam[1] + ACTOR_T_MAT);
        for (int i = 0; i < 9; i++) mf[i] = h[i];
        for (int i = 0; i < 3; i++) mf[9 + i] = s1.mat[9 + i] + v[e].pos[i] * g_worldScale;
        SetCamProjection((uint8_t*)g_cam[0], fov, aspect); SetCamProjection((uint8_t*)g_cam[1], fov, aspect);

        g_vrPmC.pixels = Vr_EyeBuffer(e);
        memset(g_vrPmC.pixels, 0, (size_t)t.width * t.height);
        DrawStarsVr(g_vrPmC.pixels, t.width, t.height, t.tanHalfH, t.tanHalfV, h);
        memset(g_vrDepth.data(), 0xFF, dsz);
        CallBegin(wbg, g_cam[0], &g_vrPmC, &g_vrPmD); CallAdd(wbg); CallEnd();
        float eyePos[3] = {v[e].pos[0] * g_worldScale, v[e].pos[1] * g_worldScale, v[e].pos[2] * g_worldScale};
        DrawDustVr(g_vrPmC.pixels, t.width, t.height, t.tanHalfH, t.tanHalfV, h, eyePos);
        memset(g_vrDepth.data(), 0xFF, dsz);
        CallBegin(wfg, g_cam[1], &g_vrPmC, &g_vrPmD); CallAdd(wfg); CallEnd();
        DrawFlareVr(g_vrPmC.pixels, t.width, t.height, t.tanHalfH, t.tanHalfV, h, hh);
        if (g_vrDumpNext) {
            PALETTEENTRY pal[256]; GetPal(pal); char n[64];
            sprintf_s(n, "vr_%02d_%c.bmp", g_dumpCount, e ? 'R' : 'L'); WriteBmp(n, &g_vrPmC, pal);
        }
    }
    g_inEye2 = false;
    RestoreCam(s0); RestoreCam(s1);
    uint8_t pal[1024]; GetPalRGBX(pal);
    Vr_SubmitEyes(v, pal);
    double ms = Ms(Now() - t0); g_vrAccMs += ms; g_vrN++; if (ms > g_vrMaxMs) g_vrMaxMs = ms;
    if (g_vrDumpNext) {
        { uint8_t pp[1024]; bool cap = Capture_GetPalette(pp); if (!cap) GetPalRGBX(pp);
          Log("vr dump palette (%s): c0=%d,%d,%d c8=%d,%d,%d cf=%d,%d,%d fe=%d,%d,%d", cap ? "p2den" : "game6",
              pp[0xc0*4], pp[0xc0*4+1], pp[0xc0*4+2], pp[0xc8*4], pp[0xc8*4+1], pp[0xc8*4+2],
              pp[0xcf*4], pp[0xcf*4+1], pp[0xcf*4+2], pp[0xfe*4], pp[0xfe*4+1], pp[0xfe*4+2]); }
        Log("vr dump %d: head L pos %.3f %.3f %.3f quat %.3f %.3f %.3f %.3f", g_dumpCount,
            v[0].pos[0], v[0].pos[1], v[0].pos[2], v[0].quat[0], v[0].quat[1], v[0].quat[2], v[0].quat[3]);
        g_dumpCount++; g_vrDumpNext = false;
    }
    if (g_hudClear) {
        // hand the game's own mono scene (stars, dust, 3D) to the monitor compositor before clearing it
        g_scene.resize((size_t)gc->width * gc->height); g_sceneW = gc->width; g_sceneH = gc->height;
        for (int y = 0; y < gc->height; y++) memcpy(&g_scene[(size_t)y * gc->width], gc->pixels + (gc->base_y + y) * gc->row_bytes + gc->base_x, gc->width);
        uint8_t mpal[1024]; GetPalRGBX(mpal);
        Capture_SetMirror(g_scene.data(), gc->width, gc->height, mpal);
        g_markersPending = true;
    }
    if (g_hudClear) {   // leave only what the game draws after the 3D passes (HUD) for p2den / the HUD quad
        for (int y = 0; y < gc->height; y++) memset(gc->pixels + (gc->base_y + y) * gc->row_bytes + gc->base_x, 0, gc->width);
    }
}

// ---------------------------------------------------------------- hooks
static LONGLONG g_passStart; static int g_passIdx = -1;

extern "C" void __cdecl PreBegin(void* world, uint8_t* cam, br_pixelmap* col, br_pixelmap* dep) {
    if (g_inEye2) return;
    int idx = (world == *(void**)ADDR_WORLD_BG) ? 0 : (world == *(void**)ADDR_WORLD_FG) ? 1 : -1;
    g_passIdx = idx; g_passStart = Now();
    if (idx >= 0) g_cam[idx] = cam;
    g_lastCol = col; g_lastDep = dep;
    if (g_loggedDetail < 6) {
        g_loggedDetail++;
        Log("Begin world=%p (pass %d)", world, idx);
        LogActor(" cam", cam); LogPm(" colour", col); LogPm(" depth", dep);
    }
    if (idx < 0) return;
    if (g_mode == 1) {
        float yaw = sinf(GetTickCount() * 0.001f * 1.5f) * g_sweepDeg * 3.14159265f / 180.f;
        SaveCam(g_saved[idx], cam); ApplyEye(cam, yaw, 0);
    } else if (g_mode == 2 && idx == 1) {
        SaveCam(g_saved[idx], cam); ApplyEye(cam, 0, -g_ipd * 0.5f);   // main buffer = left eye
    }
    if (g_fovScale != 1.0f) {
        if (!g_saved[idx].valid) SaveCam(g_saved[idx], cam);
        ScaleFov(cam);
    }
}

extern "C" void __cdecl PostBegin() {
    if (g_inEye2) return;
    int idx = g_passIdx;
    // Add+End happen after Begin; camera matrices are already baked in Begin, so restore now.
    if (idx >= 0) RestoreCam(g_saved[idx]);
}

// Hook for End: timing, and in mode 2 the right-eye render after the foreground pass.
extern "C" void __cdecl PostEnd() {
    if (g_inEye2) return;
    int idx = g_passIdx; g_passIdx = -1;
    if (idx >= 0) { g_accMs[idx] += Ms(Now() - g_passStart); g_accN[idx]++; }
    if (idx == 1) {
        g_frames++;
        // Hotkeys (Ctrl + F-key, edge-triggered): F5/F6/F7 = mode 0/1/2, F8 = toggle wide FOV,
        // F9 = dump, F10/F11 = IPD x0.5 / x2
        static bool prev[9];
        const int keys[8] = {VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12};
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        for (int k = 0; k < 8; k++) {
            bool down = ctrl && (GetAsyncKeyState(keys[k]) & 0x8000);
            if (down && !prev[k]) {
                if (k <= 2) { g_mode = k; Log("mode -> %d", g_mode); }
                if (k == 3) { g_fovScale = g_fovScale == 1.0f ? 1.6f : 1.0f; Log("fov scale -> %.2f", g_fovScale); }
                if (k == 4) g_dumpRequested = true;
                if (k == 5) { g_ipd *= 0.5f; Log("ipd -> %.4f", g_ipd); }
                if (k == 6) { g_ipd *= 2.0f; Log("ipd -> %.4f", g_ipd); }
                if (k == 7) { Vr_RequestRecenter(); }
                if (k != 4) g_loggedDetail = 4;   // log two more Begin calls after each change
            }
            prev[k] = down;
        }
        if (g_vrEnable && Vr_Active()) {
            if (g_dumpRequested) { g_dumpRequested = false; g_vrDumpNext = true; }
            RenderVrEyes();
        } else if (g_mode == 2) {
            RenderRightEye();
            if (g_dumpRequested) {
                g_dumpRequested = false;
                PALETTEENTRY pal[256]; GetPal(pal);
                char n[64];
                sprintf_s(n, "eye_%02d_L.bmp", g_dumpCount); WriteBmp(n, (br_pixelmap*)g_lastCol, pal);
                sprintf_s(n, "eye_%02d_R.bmp", g_dumpCount); WriteBmp(n, &g_pmC, pal);
                Log("dump %d: ipd=%.4f fovscale=%.2f", g_dumpCount, g_ipd, g_fovScale);
                LogActor(" fg cam", (uint8_t*)g_cam[1]);
                g_dumpCount++;
            }
        } else if (g_dumpRequested) {
            g_dumpRequested = false;
            PALETTEENTRY pal[256]; GetPal(pal); char n[64];
            sprintf_s(n, "frame_%02d.bmp", g_dumpCount++); WriteBmp(n, (br_pixelmap*)g_lastCol, pal);
        }
    }
    DWORD now = GetTickCount();
    if (now - g_lastReport > 5000 && g_frames) {
        br_pixelmap* c = (br_pixelmap*)g_lastCol;
        Log("[perf] %d frames | bg %.2f ms | fg %.2f ms | right-eye(bg+fg) %.2f ms (max %.2f) @ %ux%u | game buffer %ux%u",
            g_frames, g_accN[0] ? g_accMs[0] / g_accN[0] : 0, g_accN[1] ? g_accMs[1] / g_accN[1] : 0,
            g_accEyeN ? g_accEyeMs / g_accEyeN : 0, g_maxEyeMs, g_pmC.width, g_pmC.height, c ? c->width : 0, c ? c->height : 0);
        g_maxEyeMs = 0;
        if (g_vrN) { VrEyeTarget t = {}; Vr_GetEyeTarget(&t);
            Log("[perf] VR both eyes %.2f ms avg (max %.2f) @ %dx%d per eye", g_vrAccMs / g_vrN, g_vrMaxMs, t.width, t.height);
            g_vrAccMs = 0; g_vrN = 0; g_vrMaxMs = 0; }
        g_frames = 0; g_accMs[0] = g_accMs[1] = g_accEyeMs = 0; g_accN[0] = g_accN[1] = g_accEyeN = 0;
        g_lastReport = now;
    }
}

static uintptr_t g_beginCont = ADDR_BEGIN + 6;
static uintptr_t g_starsCont = ADDR_STARS_FN + 6;
static uintptr_t g_markersFn = ADDR_MARKERS_FN;

__declspec(naked) static void Trampoline() {
    __asm {
        push esi
        push edi
        enter 0x34, 0
        jmp dword ptr [g_beginCont]
    }
}

__declspec(naked) static void HookBegin() {
    __asm {
        pushad
        push ecx
        push ebx
        push edx
        push eax
        call PreBegin
        add esp, 16
        popad
        call Trampoline          // original BrZbSceneRenderBegin
        pushad
        call PostBegin
        popad
        ret
    }
}

__declspec(naked) static void HookStars() {
    __asm {
        mov g_game, eax
        push ebx
        push ecx
        push edx
        push esi
        push edi
        push ebp
        jmp dword ptr [g_starsCont]
    }
}

__declspec(naked) static void HookMarkersCall() {
    __asm {
        call dword ptr [g_markersFn]
        pushad
        call PostMarkers
        popad
        ret
    }
}

__declspec(naked) static void HookEnd() {
    __asm {
        mov dword ptr ds:[0x55e640], 0   // original End body
        pushad
        call PostEnd
        popad
        ret
    }
}

static bool WriteJmp(uintptr_t at, void* to, const uint8_t* expect, size_t n) {
    if (expect && memcmp((void*)at, expect, n) != 0) {
        const uint8_t* b = (const uint8_t*)at;
        Log("hook site %08x unexpected bytes %02x %02x %02x %02x %02x %02x", (unsigned)at, b[0], b[1], b[2], b[3], b[4], b[5]);
        return false;
    }
    DWORD old; VirtualProtect((void*)at, 8, PAGE_EXECUTE_READWRITE, &old);
    *(uint8_t*)at = 0xE9; *(int32_t*)(at + 1) = (int32_t)((uintptr_t)to - (at + 5));
    VirtualProtect((void*)at, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 8);
    return true;
}

static void InstallHooks() {
    static const uint8_t endBytes[10] = {0xC7, 0x05, 0x40, 0xE6, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00};
    g_tramp = (void*)Trampoline;
    bool a = WriteJmp(ADDR_BEGIN, (void*)HookBegin, BEGIN_PROLOGUE, 6);
    bool b = WriteJmp(ADDR_END, (void*)HookEnd, endBytes, 10);
    static const uint8_t starsBytes[6] = {0x53, 0x51, 0x52, 0x56, 0x57, 0x55};
    bool c = WriteJmp(ADDR_STARS_FN, (void*)HookStars, starsBytes, 6);
    bool d = false;
    if (*(uint8_t*)ADDR_MARKERS_CALL == 0xE8 && ADDR_MARKERS_CALL + 5 + *(int32_t*)(ADDR_MARKERS_CALL + 1) == ADDR_MARKERS_FN) {
        DWORD old; VirtualProtect((void*)ADDR_MARKERS_CALL, 5, PAGE_EXECUTE_READWRITE, &old);
        *(int32_t*)(ADDR_MARKERS_CALL + 1) = (int32_t)((uintptr_t)HookMarkersCall - (ADDR_MARKERS_CALL + 5));
        VirtualProtect((void*)ADDR_MARKERS_CALL, 5, old, &old); d = true;
    }
    Log("hooks: begin=%d end=%d stars=%d markers=%d", a, b, c, d);
}

// ---------------------------------------------------------------- DirectDrawCreate wrapper (forwarded to p2den)
extern "C" __declspec(dllimport) HRESULT WINAPI DirectDrawCreate(GUID*, void**, IUnknown*);  // p2den.dll
extern "C" HRESULT WINAPI P2VR_DirectDrawCreate(GUID* g, void** dd, IUnknown* o) {
    HRESULT hr = DirectDrawCreate(g, dd, o);
    Log("DirectDrawCreate -> hr=%08x dd=%p", hr, dd ? *dd : 0);
    if (SUCCEEDED(hr) && dd && *dd && !g_origCreatePalette) {
        void** vt = *(void***)*dd; DWORD old;
        VirtualProtect(&vt[5], 4, PAGE_READWRITE, &old);
        g_origCreatePalette = (PFN_CreatePalette)vt[5]; vt[5] = (void*)HookCreatePalette;
        VirtualProtect(&vt[5], 4, old, &old);
    }
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        GetModuleFileNameA(h, g_dir, MAX_PATH); *strrchr(g_dir, '\\') = 0;
        char p[MAX_PATH]; sprintf_s(p, "%s\\p2vr.log", g_dir); g_log = _fsopen(p, "w", _SH_DENYNO);
        sprintf_s(p, "%s\\p2vr.ini", g_dir);
        g_mode = GetPrivateProfileIntA("VR", "MODE", 0, p);
        char b[32];
        GetPrivateProfileStringA("VR", "IPD", "0.5", b, 32, p); g_ipd = (float)atof(b);
        GetPrivateProfileStringA("VR", "SWEEP_DEG", "25", b, 32, p); g_sweepDeg = (float)atof(b);
        GetPrivateProfileStringA("VR", "FOV_SCALE", "1.0", b, 32, p); g_fovScale = (float)atof(b);
        g_eyeW = GetPrivateProfileIntA("VR", "EYE_W", 0, p); g_eyeH = GetPrivateProfileIntA("VR", "EYE_H", 0, p);
        if (g_eyeW > 2048 || g_eyeH > 2048 || g_eyeW <= 0 || g_eyeH <= 0) g_eyeW = g_eyeH = 0;
        g_vrEnable = GetPrivateProfileIntA("VR", "VR_ENABLE", 1, p) != 0;
        g_hudClear = GetPrivateProfileIntA("VR", "HUD_CLEAR", 1, p) != 0;
        g_starSize = GetPrivateProfileIntA("VR", "STAR_SIZE", 2, p);
        g_starSprites = GetPrivateProfileIntA("VR", "STAR_SPRITES", 1, p) != 0;
        g_dust = GetPrivateProfileIntA("VR", "DUST", 1, p) != 0;
        g_flare = GetPrivateProfileIntA("VR", "LENS_FLARE", 1, p) != 0;
        GetPrivateProfileStringA("VR", "DUST_SCALE", "1.0", b, 32, p); g_dustScale = (float)atof(b);
        GetPrivateProfileStringA("VR", "WORLD_SCALE", "10", b, 32, p); g_worldScale = (float)atof(b);
        QueryPerformanceFrequency(&g_qpf);
        Log("p2vr loaded: mode=%d ipd=%.3f sweep=%.1f fovscale=%.2f eye=%dx%d", g_mode, g_ipd, g_sweepDeg, g_fovScale, g_eyeW, g_eyeH);
        InstallHooks();
        if (g_vrEnable) { Capture_Install(Log); Vr_Start(p, Log); }
    }
    return TRUE;
}
