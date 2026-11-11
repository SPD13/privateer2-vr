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
#include <mmsystem.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <share.h>
#include <vector>
#include <unordered_map>
#include "vr.h"
#ifndef P2VR_VERSION
#define P2VR_VERSION "dev"   // set by build.bat (release workflow: the tag without the v)
#endif
void Capture_Install(LogFn log);
void Capture_SetOverlayCallback(void (*f)(uint8_t*, int, int, int));
void Macro_Start(const char* dir, bool autoplay, LogFn log); void Macro_Shutdown();
void Smooth_Install(LogFn log); void Smooth_SetSpeed(int speed); void Smooth_SetGameThread(); void Smooth_Report(double seconds); void Smooth_SetIdleCallback(void (*cb)());

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
static const uintptr_t ADDR_HUD_FN = 0x454b5c, ADDR_HUD_CALL = 0x439adb;   // static HUD
static const uintptr_t ADDR_PIP_CALL = 0x45442a;
// Flight controls: joystick normalise call inside the player control function, roll write point, control method,
// key tables (DirectInput scancode indexed: held state / just-pressed)
static const uintptr_t ADDR_JOYNORM_FN = 0x44b764, ADDR_JOYNORM_CALL = 0x44fe14;
static const uintptr_t ADDR_ROLL_PATCH = 0x4503f6;   // cmp byte ptr [0x52e063],0 (7 bytes), ebp = game
static const uintptr_t ADDR_CTRL_METHOD = 0x52e062;  // low 2 bits: 0 mouse, 1 joystick, 2 joystick + throttle
static const uintptr_t ADDR_KEYSTATE = 0x55d3f0, ADDR_KEYEDGE = 0x55d4f0;
static const int GAME_JOY_FLAGS = 0x27920, GAME_JOY_X = 0x27928, GAME_JOY_Y = 0x2792c, GAME_JOY_THROTTLE = 0x27970,
                 GAME_JOY_BUTTONS = 0x27960, GAME_PLAYER = 0x2785c;
   // inside the marker function: blit of sprite 0x3a = lead indicator
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
static int   g_actorLog = 0;
static float g_laserWiden = 3.0f, g_laserWidenDist = 60.0f;
static bool  g_rumble = true; static float g_rumbleGuns = 0.35f, g_rumbleHit = 0.9f;   // controller vibration
static bool  g_retDynamic = false;
static bool  g_retInEyes = true;     // crosshair + lead indicator drawn into the 3D image at infinity (ships occlude them)
static float g_retFracP2 = 0.20f;    // crosshair region (fraction of the HUD width), same as RETICLE_SIZE
static float g_observerBack = 1.0f;  // metres: eyes sit this far behind the ship centre (HUD plane passes the centre)
static DWORD g_lastFlightTick;   // last flight frame (GetTickCount)
static bool  g_showOwnShip = false;  // draw the player's own ship model in VR (visible once the eyes are behind it)    // crosshair/markers at the depth of what is under the crosshair
static int   g_smooth = 1;
// VR controllers
static bool  g_ctlEnable = true;
static float g_rollGain = 2.5f, g_yawGain = 1.0f, g_pitchGain = 1.0f, g_throttleRate = 0.6f;
static bool  g_invertPitch = false, g_invertRoll = false;
static uint8_t g_keyFor[kVrCommandCount]; static bool g_altFor[kVrCommandCount];   // game key per command (scancode, +Alt)
static volatile bool g_rollActive; static volatile float g_rollValue;
static bool g_joyPass = true; static volatile bool g_pitchOn = true, g_yawOn = true, g_rollOn = true;   // JOYSTICK_PASSTHROUGH           // 0 = off, 1 = interpolate (1 game frame of delay), 2 = extrapolate (no delay)   // VR-only: shots appear to leave from the wings         // diagnostic: log actors appearing near the ship (lasers etc.)
static float g_dustScale = 1.0f;     // dust speck size multiplier
void Capture_SetMirror(const uint8_t* scene, int w, int h, const uint8_t* palRGBX);
bool Capture_GetPalette(uint8_t out[1024]);
bool Capture_GetHudRGB(uint8_t* out);
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

// Bug-report captures (Ctrl+F9): each capture gets its own folder, <game>\p2vr-reports\<session start>\capture-NN, that every
// dump file of it goes into, with context.txt and copies of p2vr.ini and p2vr.log (the launcher zips one for a bug report).
static char g_capDir[MAX_PATH], g_capSession[MAX_PATH]; static int g_capCount; static DWORD g_capOpened;
static void BeginCapture(bool flight); static void FinishCapture();
static void WriteRgbBmp(const char* name, const uint8_t* rgb, int w, int h);
static void WriteBmp(const char* name, const br_pixelmap* pm, const PALETTEENTRY* pal, int step = 1) {
    char path[MAX_PATH]; sprintf_s(path, "%s\\%s", g_capDir[0] ? g_capDir : g_dir, name);
    FILE* f; if (fopen_s(&f, path, "wb")) return;
    if (step < 1) step = 1;
    int w = pm->width / step, h = pm->height / step, stride = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh = {0x4D42, (DWORD)(54 + stride * h), 0, 0, 54};
    BITMAPINFOHEADER ih = {40, w, h, 1, 24, BI_RGB, (DWORD)(stride * h)};
    fwrite(&fh, 14, 1, f); fwrite(&ih, 40, 1, f);
    uint8_t* row = (uint8_t*)calloc(stride, 1);
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t* src = pm->pixels + (pm->base_y + y * step) * pm->row_bytes + pm->base_x;
        for (int x = 0; x < w; x++) { const PALETTEENTRY& p = pal[src[x * step]]; row[x*3] = p.peBlue; row[x*3+1] = p.peGreen; row[x*3+2] = p.peRed; }
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
    if (!parts || !ship) return;
    const float* m = (const float*)(ship + ACTOR_T_MAT);
    const float* spos = m + 9;
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


// ---------------------------------------------------------------- diagnostic: actors appearing near the ship
struct ActorState { uint8_t type; void* model; int framesLogged; };
static std::unordered_map<uint8_t*, ActorState> g_actorStates;
static int g_actorLines;

static void WorldPos(uint8_t* a, float out[3]) {   // translation accumulated up to the world (rotation ignored for parents)
    out[0] = out[1] = out[2] = 0;
    for (uint8_t* p = a; p && *(uint8_t**)(p + 0xC); p = *(uint8_t**)(p + 0xC)) {
        float* m = (float*)(p + ACTOR_T_MAT); out[0] += m[9]; out[1] += m[10]; out[2] += m[11];
    }
}
static void ScanActors(uint8_t* a, int depth, uint8_t* ship, const float sp[3], const float* sm) {
    for (; a && depth < 4; a = *(uint8_t**)a) {
        uint8_t type = a[0x12]; void* model = *(void**)(a + 0x18);
        ActorState& st = g_actorStates[a];
        bool visible = type == 1 && model;
        bool wasVisible = st.type == 1 && st.model;
        if (a != ship && visible && (!wasVisible || st.model != model || st.framesLogged < 3)) {
            float wp[3]; WorldPos(a, wp);
            float d[3] = {wp[0] - sp[0], wp[1] - sp[1], wp[2] - sp[2]};
            float v[3]; for (int k = 0; k < 3; k++) v[k] = d[0] * sm[k*3] + d[1] * sm[k*3 + 1] + d[2] * sm[k*3 + 2];
            float dist = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
            if (dist < 80.f && g_actorLines < 400) {
                if (!wasVisible || st.model != model) st.framesLogged = 0;
                float* m = (float*)(a + ACTOR_T_MAT);
                float sx = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]), sz = sqrtf(m[6]*m[6] + m[7]*m[7] + m[8]*m[8]);
                const char* aid = *(const char**)(a + 0x14); const char* mid = model ? *(const char**)((uint8_t*)model + 4) : nullptr;
                Log("[actor] f%d %p parent=%p depth=%d model=%p '%s'/'%s' ship-space pos (%.2f %.2f %.2f) scale x%.2f z%.2f",
                    st.framesLogged, a, *(void**)(a + 0xC), depth, model,
                    (aid && !IsBadReadPtr(aid, 1)) ? aid : "", (mid && !IsBadReadPtr(mid, 1)) ? mid : "", v[0], v[1], v[2], sx, sz);
                g_actorLines++; st.framesLogged++;
            }
        }
        st.type = type; st.model = model;
        uint8_t* kids = *(uint8_t**)(a + 8);
        if (kids) ScanActors(kids, depth + 1, ship, sp, sm);
    }
}
static void LogNewActors() {
    uint8_t* game = g_game; uint8_t* wfg = *(uint8_t**)ADDR_WORLD_FG;
    if (!game || !wfg) return;
    uint8_t* ship = *(uint8_t**)(game + GAME_SHIPS + *(int*)(game + GAME_SHIPIDX) * SHIP_STRIDE);
    if (!ship) return;
    float sp[3]; WorldPos(ship, sp);
    ScanActors(*(uint8_t**)(wfg + 8), 1, ship, sp, (const float*)(ship + ACTOR_T_MAT));
}


// ---------------------------------------------------------------- VR-only: widen the gun origin of shots
// The game's guns sit at about (+-1.13, -0.23, -1.75) ship units: ~11 cm either side of the eyes in VR, which makes
// the ship feel narrow. Shots fly straight, so for rendering only we scale their sideways offset by a factor that
// fades from LASER_WIDEN at the muzzle to 1 at LASER_WIDEN_DIST. Shot models are learned from what spawns at the
// guns. Positions are restored right after the eye renders, so game logic and hit detection are unchanged.
static void* g_boltModels[16]; static int g_nBoltModels;
static std::unordered_map<uint8_t*, bool> g_wasVisible;
struct MovedActor { float* t; float orig[3]; };
static MovedActor g_moved[128]; static int g_nMoved;

static bool IsBoltModel(void* m) { for (int i = 0; i < g_nBoltModels; i++) if (g_boltModels[i] == m) return true; return false; }

static void WidenShots() {
    g_nMoved = 0;
    uint8_t* game = g_game; uint8_t* wfg = *(uint8_t**)ADDR_WORLD_FG;
    if (!game || !wfg) return;
    uint8_t* ship = *(uint8_t**)(game + GAME_SHIPS + *(int*)(game + GAME_SHIPIDX) * SHIP_STRIDE);
    if (!ship) return;
    const float* sm = (const float*)(ship + ACTOR_T_MAT);
    const float* sp = sm + 9;
    for (uint8_t* a = *(uint8_t**)(wfg + 8); a; a = *(uint8_t**)a) {
        if (a == ship || *(uint8_t**)(a + 0xC) != wfg) continue;
        bool visible = a[0x12] == 1 && *(void**)(a + 0x18);
        bool& was = g_wasVisible[a];
        float* t = (float*)(a + ACTOR_T_MAT) + 9;
        float d[3] = {t[0] - sp[0], t[1] - sp[1], t[2] - sp[2]};
        float v[3]; for (int k = 0; k < 3; k++) v[k] = d[0] * sm[k*3] + d[1] * sm[k*3 + 1] + d[2] * sm[k*3 + 2];
        void* model = *(void**)(a + 0x18);
        if (visible && !was && fabsf(v[0]) < 4.f && fabsf(v[1]) < 3.f && v[2] < 0.5f && v[2] > -6.f &&
            !IsBoltModel(model) && g_nBoltModels < 16) {
            g_boltModels[g_nBoltModels++] = model;
            Log("[shots] learned shot model %p (spawned at %.2f %.2f %.2f)", model, v[0], v[1], v[2]);
        }
        if (visible && !was && g_rumble && g_rumbleGuns > 0 && IsBoltModel(model) &&
            fabsf(v[0]) < 4.f && fabsf(v[1]) < 3.f && v[2] < 0.5f && v[2] > -6.f)
            Vr_Haptic(1, g_rumbleGuns, 0.04f);                      // our shot just left the guns: right hand
        was = visible;
        if (g_laserWiden == 1.0f || !visible || !IsBoltModel(model) || g_nMoved >= 128) continue;
        float ahead = -v[2];
        if (ahead <= 0.f || ahead >= g_laserWidenDist) continue;
        float f = 1.0f + (g_laserWiden - 1.0f) * (1.0f - ahead / g_laserWidenDist);
        float dx = v[0] * (f - 1.0f);                       // extra sideways offset in ship space
        MovedActor& mv = g_moved[g_nMoved++];
        mv.t = t; mv.orig[0] = t[0]; mv.orig[1] = t[1]; mv.orig[2] = t[2];
        for (int k = 0; k < 3; k++) t[k] += dx * sm[k];      // ship X axis in world space = row 0
    }
}
static void RestoreShots() {
    for (int i = 0; i < g_nMoved; i++) { g_moved[i].t[0] = g_moved[i].orig[0]; g_moved[i].t[1] = g_moved[i].orig[1]; g_moved[i].t[2] = g_moved[i].orig[2]; }
    g_nMoved = 0;
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
static bool g_hudDumpNext; static int g_hudDumpIdx;
static bool g_frameDone;   // game finished drawing this cycle (static HUD done); smoothing may render until the next one
static std::vector<uint8_t> g_hudCopy;   // static HUD (crosshair source for the eye images)
// p2den shows HUD palette indices in other colours than the scene palette says (e.g. the blue crosshair). Learn the
// displayed colour per index from p2den's frame and map each index to the closest scene-palette colour.
static uint8_t g_hudRemap[256]; static bool g_hudRemapValid;
// p2den displays HUD graphics with red and blue swapped compared to the scene palette (measured on its output for
// every HUD colour). HUD pixels drawn into the eye images are mapped to the scene colour nearest to that swap.
static uint8_t g_hudSwapLut[256]; static uint32_t g_hudSwapPalHash;
static void UpdateHudSwapLut() {
    uint8_t pal[1024]; GetPalRGBX(pal);
    uint32_t hsh = 2166136261u; for (int i = 0; i < 1024; i++) hsh = (hsh ^ pal[i]) * 16777619u;
    if (hsh == g_hudSwapPalHash) return;
    g_hudSwapPalHash = hsh;
    g_hudSwapLut[0] = 0;
    for (int c = 1; c < 256; c++) {
        int r = pal[c*4 + 2], g = pal[c*4 + 1], b = pal[c*4];      // swapped colour
        int bi = c, bd = 1 << 30;
        for (int k = 1; k < 256; k++) { int dr = pal[k*4] - r, dg = pal[k*4+1] - g, db = pal[k*4+2] - b, d = dr*dr + dg*dg + db*db; if (d < bd) { bd = d; bi = k; } }
        g_hudSwapLut[c] = (uint8_t)bi;
    }
}
static void UpdateHudRemap() {
    static std::vector<uint8_t> rgb(640 * 480 * 3); static DWORD last;
    if (GetTickCount() - last < 1000 || g_hudCopy.size() != 640 * 480) return;
    if (!Capture_GetHudRGB(rgb.data())) return;
    last = GetTickCount();
    int best[256]; uint8_t col[256][3]; memset(best, -1, sizeof best);
    for (int i = 0; i < 640 * 480; i++) {                    // brightest displayed colour seen for each index
        uint8_t c = g_hudCopy[i]; if (!c) continue;
        const uint8_t* p = &rgb[i * 3]; int s = p[0] + p[1] + p[2];
        if (s > best[c]) { best[c] = s; col[c][0] = p[0]; col[c][1] = p[1]; col[c][2] = p[2]; }
    }
    uint8_t pal[1024]; GetPalRGBX(pal);
    for (int c = 0; c < 256; c++) {
        g_hudRemap[c] = (uint8_t)c;
        if (best[c] < 0) continue;
        int bi = c, bd = 1 << 30;
        for (int k = 1; k < 256; k++) {
            int dr = pal[k*4] - col[c][0], dg = pal[k*4+1] - col[c][1], db = pal[k*4+2] - col[c][2], d = dr*dr + dg*dg + db*db;
            if (d < bd) { bd = d; bi = k; }
        }
        g_hudRemap[c] = (uint8_t)bi;
    }
    if (!g_hudRemapValid) {
        int c0 = g_hudCopy[240 * 640 + 320 + 2];
        Log("[hudcolor] remap learned (e.g. index %d -> %d)", c0, g_hudRemap[c0]);
    }
    g_hudRemapValid = true;
}
// Text or graphics the game draws across a panel edge (e.g. "OBSERVER VIEW" on the VELOCITY panel's top edge) would be cut
// between the angled panel and the flat HUD. Each pixel group crossing an edge (pixels joined across small gaps, so a
// line of text is one group) is added whole to that panel for this frame. Groups reaching into another panel, or too
// big to be text, are left alone.
static void GrowPanelsAroundText(br_pixelmap* gc, int (*rects)[4], int n) {
    static uint16_t stamp[480][640]; static uint16_t stampId; static int qx[8192], qy[8192];
    auto px = [&](int x, int y) -> uint8_t { return (gc->pixels + (gc->base_y + y) * gc->row_bytes + gc->base_x)[x]; };
    auto inRect = [](const int* r, int x, int y) { return x >= r[0] && x < r[2] && y >= r[1] && y < r[3]; };
    int out[12][4];
    for (int k = 0; k < n; k++) {
        const int* r = rects[k]; for (int i = 0; i < 4; i++) out[k][i] = r[i];
        // seed pairs straddling the edges: a drawn pixel inside next to a drawn pixel outside
        int seeds[256][2]; int ns = 0;
        auto seed = [&](int ix, int iy, int ox2, int oy2) {
            if (ns < 256 && ox2 >= 0 && ox2 < 640 && oy2 >= 0 && oy2 < 480 && ix >= 0 && ix < 640 && iy >= 0 && iy < 480 && px(ix, iy) && px(ox2, oy2)) { seeds[ns][0] = ix; seeds[ns][1] = iy; ns++; } };
        for (int x = r[0]; x < r[2]; x++) { seed(x, r[1], x, r[1] - 1); seed(x, r[3] - 1, x, r[3]); }
        for (int y = r[1]; y < r[3]; y++) { seed(r[0], y, r[0] - 1, y); seed(r[2] - 1, y, r[2], y); }
        if (stampId > 0xF000) { memset(stamp, 0, sizeof stamp); stampId = 0; }
        uint16_t base = (uint16_t)(stampId + 1);                       // groups of this panel: ids >= base
        for (int s = 0; s < ns; s++) {
            if (stamp[seeds[s][1]][seeds[s][0]] >= base) continue;     // already part of a group of this panel
            ++stampId;
            int head = 0, tail = 0, bx0 = 640, by0 = 480, bx1 = -1, by1 = -1; bool shared = false;
            qx[tail] = seeds[s][0]; qy[tail++] = seeds[s][1]; stamp[seeds[s][1]][seeds[s][0]] = stampId;
            while (head < tail && tail < 8000) {
                int x = qx[head], y = qy[head++];
                if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; if (y < by0) by0 = y; if (y > by1) by1 = y;
                if (!inRect(r, x, y)) for (int j = 0; j < n; j++) if (j != k && inRect(rects[j], x, y)) shared = true;
                for (int dy = -1; dy <= 1; dy++) for (int dx = -5; dx <= 5; dx++) {   // letters and word gaps of a line
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || nx >= 640 || ny < 0 || ny >= 480 || stamp[ny][nx] == stampId || !px(nx, ny)) continue;
                    stamp[ny][nx] = stampId; if (tail < 8000) { qx[tail] = nx; qy[tail++] = ny; }
                }
            }
            if (shared || tail >= 8000 || bx1 - bx0 > 320 || by1 - by0 > 120) continue;
            if (bx0 < out[k][0]) out[k][0] = bx0; if (by0 < out[k][1]) out[k][1] = by0;
            if (bx1 + 1 > out[k][2]) out[k][2] = bx1 + 1; if (by1 + 1 > out[k][3]) out[k][3] = by1 + 1;
        }
        static int lastLogged[12][4];
        if (memcmp(out[k], lastLogged[k], sizeof out[k]) && (out[k][0] != r[0] || out[k][1] != r[1] || out[k][2] != r[2] || out[k][3] != r[3])) {
            memcpy(lastLogged[k], out[k], sizeof out[k]);
            Log("[panels] panel %d grown to (%d,%d)-(%d,%d) around text on its edge", k, out[k][0], out[k][1], out[k][2], out[k][3]);
        }
    }
    Vr_SetPanelRects(out, n);
}
// After the flight HUD (game thread, the frame buffer still holds it): which HUD panels hold something (closed windows: no
// compositor layer), and panels grown around text crossing their edges. (Not at Present: that runs on p2den's thread,
// when the game's buffer may already be cleared for the next frame.)
static void PanelLayout() {
    if (g_lastCol && Vr_Active()) {
        br_pixelmap* gc = (br_pixelmap*)g_lastCol;
        static int rects[12][4]; int n = Vr_GetPanelRects(rects, 12);
        if (n && gc->width == 640 && gc->height == 480) {
            uint32_t mask = 0;
            for (int k = 0; k < n; k++) {
                const int* r = rects[k]; bool any = false;
                for (int y = r[1]; y < r[3] && y < 480 && !any; y += 2) {
                    const uint8_t* row = gc->pixels + (gc->base_y + y) * gc->row_bytes + gc->base_x;
                    for (int x = r[0]; x < r[2] && x < 640; x += 2) if (row[x]) { any = true; break; }
                }
                if (any) mask |= 1u << k;
            }
            Vr_SetPanelMask(mask);
            GrowPanelsAroundText(gc, rects, n);
        }
    }
}
static void DrawCheatOverlay(uint8_t* data, int pitch, int W, int H);
extern "C" void __cdecl PostHud() {   // static HUD done: the rest of the cycle is idle (smoothing may render)
    g_frameDone = true;
    Vr_NoteHudDone();
    PanelLayout();
    if (g_lastCol) {   // flight: the cheat message into the game's frame after its HUD (p2den's flight upload is not hooked)
        br_pixelmap* gc = (br_pixelmap*)g_lastCol;
        if (gc->width == 640 && gc->height == 480) DrawCheatOverlay(gc->pixels + gc->base_y * gc->row_bytes + gc->base_x, gc->row_bytes, 640, 480);
    }
    if (g_retInEyes && g_lastCol) {   // crosshair source for the next frames' eye images
        br_pixelmap* gc = (br_pixelmap*)g_lastCol;
        if (gc->width == 640 && gc->height == 480) {
            g_hudCopy.resize(640 * 480);
            for (int y = 0; y < 480; y++) memcpy(&g_hudCopy[y * 640], gc->pixels + (gc->base_y + y) * gc->row_bytes + gc->base_x, 640);
            UpdateHudSwapLut();
        }
    }
    if (!g_hudDumpNext || !g_lastCol) return;
    g_hudDumpNext = false;
    PALETTEENTRY pal[256]; GetPal(pal); char n[64];
    sprintf_s(n, "hud_%02d.bmp", g_hudDumpIdx); WriteBmp(n, (br_pixelmap*)g_lastCol, pal);
    {   // raw 8-bit HUD indices + palette + p2den's displayed HUD (RGB) for offline comparison
        static std::vector<uint8_t> rgb(640 * 480 * 3);
        char f[MAX_PATH]; sprintf_s(f, "%s\\hudraw_%02d.bin", g_capDir[0] ? g_capDir : g_dir, g_hudDumpIdx);
        FILE* fp; if (!fopen_s(&fp, f, "wb")) {
            br_pixelmap* gc = (br_pixelmap*)g_lastCol;
            for (int y = 0; y < 480; y++) fwrite(gc->pixels + (gc->base_y + y) * gc->row_bytes + gc->base_x, 1, 640, fp);
            uint8_t pr[1024]; GetPalRGBX(pr); fwrite(pr, 1, 1024, fp);
            bool ok = Capture_GetHudRGB(rgb.data()); if (!ok) memset(rgb.data(), 0, rgb.size()); fwrite(rgb.data(), 1, rgb.size(), fp);
            fclose(fp); Log("wrote %s (hud rgb %s)", f, ok ? "ok" : "missing");
        }
    }
    g_hudDumpIdx++;
    FinishCapture();
}

// Target selection marker: the marker function blits sprite 0x3a at the target's projected position (call site 0x45442a). Its rectangle comes
// from the shape table: entry = shapes + shapes[id*2+2]; bounds at entry+8..0x14 (x0,y0,x1,y1) relative to (x,y).
struct PipRect { int x0, y0, x1, y1; };
static PipRect g_pips[8]; static int g_nPips;
static std::vector<uint8_t> g_pipCopy; static PipRect g_pipCopyRects[8]; static int g_nPipCopy;   // lead indicator source
extern "C" void __cdecl RecordSprite(int id, int x, int y) {
    const int32_t* sh = *(const int32_t**)ADDR_SHAPES;
    if (!sh || g_nPips >= 8) return;
    const uint8_t* e = (const uint8_t*)sh + sh[id * 2 + 2];
    g_pips[g_nPips++] = {*(const int32_t*)(e + 8) + x, *(const int32_t*)(e + 12) + y, *(const int32_t*)(e + 16) + x, *(const int32_t*)(e + 20) + y};
}
// Every marker line goes through FUN_004661a0 (eax view, edx x0, ebx y0, ecx x1, stack: y1, colour, ...), which clips
// to the game's view. Lines of the missile-lock reticle (drawn from FUN_00453580) are recorded here unclipped and
// redrawn by p2vr around the target, so the reticle is complete even where it reaches outside the game's view.
struct MkLine { int x0, y0, x1, y1, colour; };
static MkLine g_lockLines[128]; static int g_nLockLines; static bool g_recLines;
struct PushadFrame { uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax, ret, y1, colour; };
// Target-direction triangle: drawn by the HUD routine FUN_00454b5c (3 lines, colour 0x2f / 0x6f, calls returning to
// 0x45689e / 0x4568e0 / 0x456917), i.e. into the HUD image, where the HUD panel cut-outs would slice it. With
// HUD_ARROW_LAYER these lines are not drawn by the game; p2vr redraws them into the direction-arrow layer (own quads on the
// HUD glass, below the panels) with the next marker set (one game frame later).
static bool g_hudArrowLayer = true;
static MkLine g_hudArrow[8], g_hudArrowPrev[8]; static int g_nHudArrow, g_nHudArrowPrev;
static bool g_cursorFix = true;
static void DetectCursorOffset(uint32_t ret, int x, int y, const int* view);
extern "C" int __cdecl RecordLine(PushadFrame* f) {
    if (g_cursorFix) DetectCursorOffset(f->ret, (int)f->edx, (int)f->ebx, (const int*)f->eax);
    if (f->ret == 0x45689e || f->ret == 0x4568e0 || f->ret == 0x456917) {
        if (!g_hudArrowLayer || !Vr_Active()) return 0;
        if (g_nHudArrow < 8) g_hudArrow[g_nHudArrow++] = {(int)f->edx, (int)f->ebx, (int)f->ecx, (int)f->y1, (int)(f->colour & 0xff)};
        return 1;                                                // skip the game's drawing
    }
    if (!g_recLines || f->ret < 0x453580 || f->ret >= 0x453b00 || g_nLockLines >= 128) return 0;
    g_lockLines[g_nLockLines++] = {(int)f->edx, (int)f->ebx, (int)f->ecx, (int)f->y1, (int)(f->colour & 0xff)};
    return 0;
}
static uintptr_t g_lineCont = 0x4661a0 + 7;
__declspec(naked) static void HookLine() {
    __asm {
        pushad
        push esp
        call RecordLine
        add esp, 4
        test eax, eax
        popad                       // (flags kept)
        jnz skip
        push esi
        push edi
        push ebp
        mov esi, dword ptr [esp + 0x20]
        jmp dword ptr [g_lineCont]
    skip:
        ret 0x14                    // FUN_004661a0 pops its 5 stack arguments
    }
}
static void DrawLine(uint8_t* img, int w, int h, int x0, int y0, int x1, int y1, uint8_t c, int brush) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (int n = 0; n < 4096; n++) {
        for (int by = -brush; by <= brush; by++) for (int bx = -brush; bx <= brush; bx++) {
            int x = x0 + bx, y = y0 + by; if (x >= 0 && y >= 0 && x < w && y < h) img[(size_t)y * w + x] = c; }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err; if (e2 >= dy) { err += dy; x0 += sx; } if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
extern "C" void __cdecl PreMarkers() {
    g_nPips = 0; g_nLockLines = 0; g_recLines = true;
    memcpy(g_hudArrowPrev, g_hudArrow, sizeof g_hudArrow); g_nHudArrowPrev = g_nHudArrow; g_nHudArrow = 0;   // last HUD pass
}

// low-res depth pass through the game camera (see HudFovDepth)
static std::vector<uint8_t> g_dpC, g_dpD; static float g_dpHither = 1, g_dpYon = 1024; static bool g_dpValid;
// Split the marker image into separate markers: occupancy on a 4x4-pixel grid, dilated so a bracket's corners
// join, connected components, then the nearest depth (low-res depth pass = same camera) inside each marker's box.
static int FindMarkerBlobs(const uint8_t* mk, int w, int h, VrMarkerBlob* out, int maxOut) {
    const int GW = 160, GH = 120, R = 3;                        // grid cells, merge radius in cells (12 px)
    if (w != 640 || h != 480) return 0;
    static uint8_t occ[GW * GH], dil[GW * GH]; static int lab[GW * GH];
    memset(occ, 0, sizeof occ);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (mk[y * w + x]) occ[(y >> 2) * GW + (x >> 2)] = 1;
    memset(dil, 0, sizeof dil);
    for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) if (occ[y * GW + x])
        for (int dy = -R; dy <= R; dy++) for (int dx = -R; dx <= R; dx++) {
            int xx = x + dx, yy = y + dy; if (xx >= 0 && xx < GW && yy >= 0 && yy < GH) dil[yy * GW + xx] = 1; }
    memset(lab, 0, sizeof lab);
    int n = 0; static int stack[GW * GH];
    for (int i = 0; i < GW * GH && n < maxOut; i++) {
        if (!dil[i] || lab[i]) continue;
        int sp = 0; stack[sp++] = i; lab[i] = n + 1;
        int x0 = GW, y0 = GH, x1 = -1, y1 = -1;
        while (sp) {
            int c = stack[--sp], cx = c % GW, cy = c / GW;
            if (occ[c]) { if (cx < x0) x0 = cx; if (cx > x1) x1 = cx; if (cy < y0) y0 = cy; if (cy > y1) y1 = cy; }
            const int nb[4] = {c - 1, c + 1, c - GW, c + GW};
            for (int k = 0; k < 4; k++) { int q = nb[k];
                if (q < 0 || q >= GW * GH || (k < 2 && (q / GW) != cy)) continue;
                if (dil[q] && !lab[q]) { lab[q] = n + 1; stack[sp++] = q; } }
        }
        if (x1 < 0) continue;
        VrMarkerBlob& b = out[n++];
        int px0 = 640, py0 = 480, px1 = -1, py1 = -1;              // exact pixel bounds inside the component's cells
        for (int y = y0 * 4; y <= y1 * 4 + 3 && y < h; y++) for (int x = x0 * 4; x <= x1 * 4 + 3 && x < w; x++)
            if (mk[y * w + x] && lab[(y >> 2) * GW + (x >> 2)] == n) { if (x < px0) px0 = x; if (x > px1) px1 = x; if (y < py0) py0 = y; if (y > py1) py1 = y; }
        if (px1 < 0) { px0 = x0 * 4; py0 = y0 * 4; px1 = x1 * 4 + 3; py1 = y1 * 4 + 3; }
        b.x0 = px0; b.y0 = py0; b.x1 = px1; b.y1 = py1; b.depthM = -1; b.layer = 0;
    }
    return n;
}

// Depth of each marker = distance (along the ship axis) of the object whose centre the game projects inside it.
struct MarkerLink { uint8_t* actor; float offX, offY; float w, h; bool keepOffset; float worldW, worldH; };   // worldW>0: fixed physical size (units)
static MarkerLink g_mkLink[10]; static int g_nMkLink; static uint32_t g_mkSerial;
static uint8_t* CameraRef() {   // the actor the game's foreground camera is attached to
    uint8_t* cam = (uint8_t*)g_cam[1];
    uint8_t* par = cam ? *(uint8_t**)(cam + 0xC) : nullptr;
    if (par && par != *(uint8_t**)ADDR_WORLD_FG) return par;
    return g_game ? *(uint8_t**)(g_game + GAME_SHIPS + *(int*)(g_game + GAME_SHIPIDX) * SHIP_STRIDE) : nullptr;
}
static uint8_t* g_targetActor;   // the game's current target object
static const int ARROW_COLOUR = 0x4f;   // direction arrows / nav bracket lines (FUN_004661a0 calls in 0x453b00); shaded with the 0x40..0x4f ramp
static void AssignMarkerDepths(VrMarkerBlob* b, int n, MarkerLink* link) {
    uint8_t* game = g_game; uint8_t* wfg = *(uint8_t**)ADDR_WORLD_FG;
    if (!game || !wfg || !n) return;
    uint8_t* ship = CameraRef();
    if (!ship) return;
    {   static bool logged; uint8_t* tbl = *(uint8_t**)(game + GAME_SHIPS + *(int*)(game + GAME_SHIPIDX) * SHIP_STRIDE);
        if (!logged && tbl) { logged = true; const float* a = (const float*)(tbl + ACTOR_T_MAT); const float* c2 = (const float*)(ship + ACTOR_T_MAT);
            Log("[markers] reference: ship-table actor %p T(%.2f %.2f %.2f) | camera parent %p T(%.2f %.2f %.2f)", tbl, a[9], a[10], a[11], ship, c2[9], c2[10], c2[11]); } }
    const float* sm = (const float*)(ship + ACTOR_T_MAT); const float K = 461.0357f;
    struct Cand { float x, y, z; uint8_t* a; }; static Cand cand[256]; int nc = 0;
    for (int i = 0; i < n; i++) link[i].actor = nullptr;
    for (uint8_t* a = *(uint8_t**)(wfg + 8); a && nc < 256; a = *(uint8_t**)a) {
        if (a == ship) continue;
        bool modelled = a[0x12] == 1 && *(void**)(a + 0x18) && !IsBoltModel(*(void**)(a + 0x18));
        bool group = !*(void**)(a + 0x18) && *(uint8_t**)(a + 8);           // multi-part object: parts are its children
        if (!modelled && !group) continue;
        const float* m = (const float*)(a + ACTOR_T_MAT);
        float d[3] = {m[9] - sm[9], m[10] - sm[10], m[11] - sm[11]}, v[3];
        for (int k = 0; k < 3; k++) v[k] = d[0]*sm[k*3] + d[1]*sm[k*3+1] + d[2]*sm[k*3+2];
        if (v[2] > -1.0f) continue;
        cand[nc++] = {320.f + v[0] * K / -v[2], 240.f - v[1] * K / -v[2], -v[2], a};
    }
    // current target = game entry index at (game + 0x1216 + player*0x3a0) >> 16, player = *(game + 0x2785c); -1 = none
    g_targetActor = nullptr;
    {   int player = *(int*)(game + 0x2785c);
        int tgt = *(int*)(game + 0x1216 + player * SHIP_STRIDE) >> 16;
        if (tgt >= 0 && tgt < 0x95) g_targetActor = *(uint8_t**)(game + GAME_SHIPS + tgt * SHIP_STRIDE); }
    bool targetAlive = false;
    for (int c = 0; c < nc; c++) if (cand[c].a == g_targetActor) targetAlive = true;
    if (g_targetActor && !targetAlive && nc < 256 && g_targetActor != ship) {   // not in the list: project it directly
        const float* m = (const float*)(g_targetActor + ACTOR_T_MAT);
        float d[3] = {m[9] - sm[9], m[10] - sm[10], m[11] - sm[11]}, v[3];
        for (int k = 0; k < 3; k++) v[k] = d[0]*sm[k*3] + d[1]*sm[k*3+1] + d[2]*sm[k*3+2];
        if (v[2] < -1.0f) { cand[nc++] = {320.f + v[0] * K / -v[2], 240.f - v[1] * K / -v[2], -v[2], g_targetActor}; targetAlive = true; }
    }
    for (int i = 0; i < n; i++) {
        float cx = (b[i].x0 + b[i].x1) * 0.5f, cy = (b[i].y0 + b[i].y1) * 0.5f, best = 1e30f;
        for (int c = 0; c < nc; c++) {
            // the game draws its markers a bit off the object's origin (more so when close): generous tolerance
            int tol = (b[i].x1 - b[i].x0 + 1) > 24 ? (b[i].x1 - b[i].x0 + 1) : 24;
            if (b[i].layer == 1) tol = 96;                      // lead indicator sits ahead of the target
            if (cand[c].x < b[i].x0 - tol || cand[c].x > b[i].x1 + tol || cand[c].y < b[i].y0 - tol || cand[c].y > b[i].y1 + tol) continue;
            float dd = (cand[c].x - cx) * (cand[c].x - cx) + (cand[c].y - cy) * (cand[c].y - cy);
            if (dd < best) { best = dd; b[i].depthM = cand[c].z / g_worldScale;
                link[i] = {cand[c].a, cx - cand[c].x, cy - cand[c].y, (float)(b[i].x1 - b[i].x0 + 1), (float)(b[i].y1 - b[i].y0 + 1), b[i].layer == 1}; }
        }
        // Target bracket (octagon) and lead indicator belong to the game's current target (read from game data).
        bool octagon = b[i].layer == 0 && (b[i].x1 - b[i].x0) >= 30 && (b[i].y1 - b[i].y0) >= 30;
        bool lead = b[i].layer == 1;
        if ((octagon || lead) && targetAlive) {
            for (int c = 0; c < nc; c++) if (cand[c].a == g_targetActor) {
                b[i].depthM = cand[c].z / g_worldScale;
                link[i] = {cand[c].a, lead ? cx - cand[c].x : 0.f, lead ? cy - cand[c].y : 0.f,
                           (float)(b[i].x1 - b[i].x0 + 1), (float)(b[i].y1 - b[i].y0 + 1), lead};
            }
        }
    }
}

static bool g_posesWithEyes = true;   // marker poses travel with the eye images rendered at the same moment
static void UpdateMarkerPoses(); static void ApplyBlend(float f); static void RestoreBlend(); static float BlendFactor();
extern "C" void __cdecl PostMarkers() {
    if (!g_markersPending || !g_lastCol) return;
    g_markersPending = false;
    br_pixelmap* gc = (br_pixelmap*)g_lastCol;
    int w = gc->width, h = gc->height;
    static std::vector<uint8_t> mk, pip, both; mk.resize((size_t)w * h); pip.assign((size_t)w * h, 0);
    for (int y = 0; y < h; y++) {
        uint8_t* row = gc->pixels + (gc->base_y + y) * gc->row_bytes + gc->base_x;
        memcpy(&mk[(size_t)y * w], row, w); memset(row, 0, w);
    }
    // lead-indicator pixels -> their own image (shown on the crosshair plane)
    VrMarkerBlob blobs[10]; int nb = 0;
    for (int i = 0; i < g_nPips && nb < 4; i++) {
        PipRect r = g_pips[i];
        if (r.x0 < 0) r.x0 = 0; if (r.y0 < 0) r.y0 = 0; if (r.x1 >= w) r.x1 = w - 1; if (r.y1 >= h) r.y1 = h - 1;
        if (r.x1 < r.x0 || r.y1 < r.y0) continue;
        for (int y = r.y0; y <= r.y1; y++) for (int x = r.x0; x <= r.x1; x++) { size_t k = (size_t)y * w + x; if (mk[k]) { pip[k] = mk[k]; mk[k] = 0; } }
        blobs[nb++] = {r.x0, r.y0, r.x1, r.y1, -1.0f, 1};
    }
    g_recLines = false;
    bool ownLock = g_nLockLines > 0 && w == 640 && h == 480;
    if (ownLock) for (int i = 0; i < g_nLockLines; i++) { const MkLine& l = g_lockLines[i]; DrawLine(mk.data(), w, h, l.x0, l.y0, l.x1, l.y1, 0, 1); }
    // Direction arrows / nav bracket (orange ramp 0x40..0x4f) are separated before the blob search, so an arrow lying on
    // a ship's bracket can never merge with it: their pixels go to the lead-indicator layer and they stay on the HUD glass.
    static std::vector<uint8_t> arrowImg; arrowImg.assign((size_t)w * h, 0); bool anyArrow = false;
    for (size_t k = 0; k < (size_t)w * h; k++) if ((mk[k] >> 4) == (ARROW_COLOUR >> 4)) { arrowImg[k] = mk[k]; mk[k] = 0; anyArrow = true; }
    if (w == 640 && h == 480) for (int i = 0; i < g_nHudArrowPrev; i++) {   // the HUD's target-direction triangle (not clipped)
        const MkLine& l = g_hudArrowPrev[i]; DrawLine(arrowImg.data(), w, h, l.x0, l.y0, l.x1, l.y1, (uint8_t)l.colour, 0); anyArrow = true; }
    int nm = FindMarkerBlobs(mk.data(), w, h, blobs + nb, 10 - nb);
    nb += nm;
    MarkerLink links[10] = {};
    AssignMarkerDepths(blobs, nb, links);   // target sprite + other markers: placed at the object they are drawn on
    if (anyArrow && nb < 8) {
        int na = FindMarkerBlobs(arrowImg.data(), w, h, blobs + nb, 8 - nb);
        for (int i = nb; i < nb + na; i++) {
            blobs[i].layer = 1; blobs[i].depthM = -2.0f; links[i] = {};
            for (int y = blobs[i].y0; y <= blobs[i].y1; y++) for (int x = blobs[i].x0; x <= blobs[i].x1; x++) {
                size_t k = (size_t)y * w + x; if (arrowImg[k] && !pip[k]) pip[k] = arrowImg[k]; }
        }
        nb += na;
    }
    // Target octagon: always shown from a stored copy of the last *complete* octagon the game drew, with a fixed
    // physical size around the target (measured when complete, smoothed) -> no clipping at the game's view edges,
    // and smooth growth while closing in (the game only resizes it at 30 Hz).
    static std::vector<uint8_t> octCache; static int octW, octH; static float octWorldW, octWorldH;   // world units
    const float K = 461.0357f;
    static uint8_t* octFor;                                    // target the stored octagon belongs to
    if (g_targetActor != octFor) { octFor = g_targetActor; octCache.clear(); octWorldW = octWorldH = 0; }
    // dominant (most frequent) colour index of a marker box: the octagon and e.g. the missile-lock reticle differ
    auto dominant = [&](const VrMarkerBlob& bl) -> int {
        int cnt[256] = {};
        for (int y = bl.y0; y <= bl.y1; y++) for (int x = bl.x0; x <= bl.x1; x++) cnt[mk[(size_t)y * w + x]]++;
        int best = 0; for (int c = 1; c < 256; c++) if (cnt[c] > cnt[best] || best == 0) best = cnt[c] ? c : best;
        return best;
    };
    // Direction arrows (and the nav-point bracket) are drawn by the game in line colour 0x4f around the screen centre:
    // they never belong to an object, so they always stay on the crosshair plane even when they lie over a target.
    bool arrow[10] = {};
    for (int i = 0; i < nb; i++) if (blobs[i].layer == 0 && (dominant(blobs[i]) >> 4) == (ARROW_COLOUR >> 4)) { arrow[i] = true; links[i] = {}; }
    {   static DWORD lastLog;   // diagnostic: marker pieces with their colour
        if (GetTickCount() - lastLog > 3000 && nb) { lastLog = GetTickCount(); char line[512]; int o = 0;
            for (int i = 0; i < nb && o < 440; i++) o += sprintf_s(line + o, sizeof line - o, " [L%d (%d,%d)-(%d,%d) c%02x%s%s]", blobs[i].layer, blobs[i].x0, blobs[i].y0,
                blobs[i].x1, blobs[i].y1, blobs[i].layer == 0 ? dominant(blobs[i]) : 0, arrow[i] ? " arrow" : "", links[i].actor ? " linked" : "");
            Log("[markers]%s", line); } }
    static int knownOctColour = -1;                            // learned from the first complete octagon
    int oct = -1;
    for (int i = 0; i < nb; i++)
        if (blobs[i].layer == 0 && !arrow[i] && links[i].actor && links[i].actor == g_targetActor && blobs[i].x1 - blobs[i].x0 >= 20 && blobs[i].y1 - blobs[i].y0 >= 20) {
            if (oct < 0) oct = i;
            else if (knownOctColour >= 0 && dominant(blobs[i]) == knownOctColour && dominant(blobs[oct]) != knownOctColour) oct = i;
        }
    int octColour = oct >= 0 ? dominant(blobs[oct]) : -1;
    if (oct >= 0) {   // a large octagon falls apart into several pieces: merge nearby pieces of the same colour into it
        bool grew = true;
        while (grew) {
            grew = false;
            for (int i = 0; i < nb; i++) {
                if (i == oct || blobs[i].layer != 0 || arrow[i]) continue;
                const int m = 20;
                bool isNear = blobs[i].x1 >= blobs[oct].x0 - m && blobs[i].x0 <= blobs[oct].x1 + m && blobs[i].y1 >= blobs[oct].y0 - m && blobs[i].y0 <= blobs[oct].y1 + m;
                if (!isNear || dominant(blobs[i]) != octColour) continue;
                if (blobs[i].x0 < blobs[oct].x0) blobs[oct].x0 = blobs[i].x0; if (blobs[i].y0 < blobs[oct].y0) blobs[oct].y0 = blobs[i].y0;
                if (blobs[i].x1 > blobs[oct].x1) blobs[oct].x1 = blobs[i].x1; if (blobs[i].y1 > blobs[oct].y1) blobs[oct].y1 = blobs[i].y1;
                for (int k = i; k < nb - 1; k++) { blobs[k] = blobs[k + 1]; links[k] = links[k + 1]; arrow[k] = arrow[k + 1]; }
                nb--; if (oct > i) oct--; grew = true; break;
            }
        }
    }
    // the game clips markers to its own view rectangle: learn it from all markers seen
    static int clipX0 = 1 << 20, clipY0 = 1 << 20, clipX1 = -1, clipY1 = -1;
    for (int i = 0; i < nb; i++) if (blobs[i].layer == 0) {
        if (blobs[i].x0 < clipX0) clipX0 = blobs[i].x0; if (blobs[i].y0 < clipY0) clipY0 = blobs[i].y0;
        if (blobs[i].x1 > clipX1) clipX1 = blobs[i].x1; if (blobs[i].y1 > clipY1) clipY1 = blobs[i].y1; }
    // target position as the game sees it (its projection centre) and distance along the ship axis
    float tx = 0, ty = 0, tz = -1;
    if (g_targetActor) { uint8_t* ship = CameraRef();
        if (ship) { const float* sm = (const float*)(ship + ACTOR_T_MAT); const float* m = (const float*)(g_targetActor + ACTOR_T_MAT);
            float d[3] = {m[9] - sm[9], m[10] - sm[10], m[11] - sm[11]}, v[3];
            for (int k = 0; k < 3; k++) v[k] = d[0]*sm[k*3] + d[1]*sm[k*3+1] + d[2]*sm[k*3+2];
            if (v[2] < -1.0f) { tz = -v[2]; tx = 320.f + v[0] * K / tz; ty = 240.f - v[1] * K / tz; } } }
    if (oct >= 0 && tz > 0) {
        int ow = blobs[oct].x1 - blobs[oct].x0 + 1, oh = blobs[oct].y1 - blobs[oct].y0 + 1;
        float cx = (blobs[oct].x0 + blobs[oct].x1) * 0.5f, cy = (blobs[oct].y0 + blobs[oct].y1) * 0.5f;
        bool square = ow * 10 >= oh * 7 && oh * 10 >= ow * 7;
        bool inside = blobs[oct].x0 > clipX0 + 1 && blobs[oct].y0 > clipY0 + 1 && blobs[oct].x1 < clipX1 - 1 && blobs[oct].y1 < clipY1 - 1;
        bool centred = fabsf(cx - tx) < 0.2f * ow + 3 && fabsf(cy - ty) < 0.2f * oh + 3;
        if (square && inside && centred && ow <= 640 && oh <= 480) {      // complete: store it and its physical size
            octW = ow; octH = oh; octCache.assign((size_t)ow * oh, 0);
            for (int y = 0; y < oh; y++) memcpy(&octCache[(size_t)y * ow], &mk[(size_t)(blobs[oct].y0 + y) * w + blobs[oct].x0], ow);
            knownOctColour = octColour;
            float ww = ow / K * tz, wh = oh / K * tz;
            if (octWorldW <= 0) { octWorldW = ww; octWorldH = wh; } else { octWorldW += (ww - octWorldW) * 0.3f; octWorldH += (wh - octWorldH) * 0.3f; }
        }
    }
    // The game's own screen point for its target (entry+0x12d0 / +0x12d2, 16.16): it draws the octagon and the missile-lock
    // reticle around it, and the lock reticle converges onto it. Offsets of target-attached markers are measured from it,
    // so they line up with the cached octagon (centred on the target itself) and a completed lock sits exactly on it.
    float gx = tx, gy = ty; bool lockActive = false;
    if (g_targetActor && tz > 0) {
        int player = *(int*)(g_game + 0x2785c), tgt = *(int*)(g_game + 0x1216 + player * SHIP_STRIDE) >> 16;
        if (tgt >= 0 && tgt < 0x95) {
            const uint8_t* te = g_game + tgt * SHIP_STRIDE;
            gx = (float)(*(const int*)(te + 0x12d0) >> 16); gy = (float)(*(const int*)(te + 0x12d2) >> 16);
            if (fabsf(gx - tx) > 120 || fabsf(gy - ty) > 120) { gx = tx; gy = ty; }   // stale (target off screen)
        }
        lockActive = *(int*)(g_game + 0x279e4) != -1;            // a missile is selected: the lock reticle may be drawn
        static DWORD lastLog;
        if (GetTickCount() - lastLog > 5000) { lastLog = GetTickCount();
            Log("[markers] target point game (%.0f,%.0f) vs projected (%.1f,%.1f), missile %s, lock %.2f", gx, gy, tx, ty,
                lockActive ? "selected" : "none", *(float*)(g_game + 0x279f0)); }
    }
    for (int i = 0; i < nb; i++) {
        if (i == oct || blobs[i].layer != 0 || arrow[i] || tz <= 0 || !g_targetActor) continue;
        float cx = (blobs[i].x0 + blobs[i].x1) * 0.5f, cy = (blobs[i].y0 + blobs[i].y1) * 0.5f;
        bool mine = links[i].actor == g_targetActor;
        // lock reticle pieces far from the target while converging: attach them to the target too (not the HUD glass)
        if (!mine && !links[i].actor && lockActive && fabsf(cx - gx) < 220 && fabsf(cy - gy) < 220) mine = true;
        if (!mine) continue;
        links[i].actor = g_targetActor; links[i].w = (float)(blobs[i].x1 - blobs[i].x0 + 1); links[i].h = (float)(blobs[i].y1 - blobs[i].y0 + 1);
        links[i].worldW = links[i].worldH = 0;
        links[i].offX = cx - gx; links[i].offY = cy - gy; links[i].keepOffset = true;   // animation around the target kept
        blobs[i].depthM = tz / g_worldScale;
    }
    // missile-lock reticle redrawn from its recorded lines into a 640x480 canvas centred on the game's target point
    static std::vector<uint8_t> lockImg; lockImg.assign((size_t)640 * 480, 0);
    if (ownLock && g_targetActor && tz > 0 && nb < 9) {
        int bx0 = 640, by0 = 480, bx1 = -1, by1 = -1, ox = 320 - (int)gx, oy = 240 - (int)gy;
        for (int i = 0; i < g_nLockLines; i++) {
            const MkLine& l = g_lockLines[i];
            DrawLine(lockImg.data(), 640, 480, l.x0 + ox, l.y0 + oy, l.x1 + ox, l.y1 + oy, (uint8_t)l.colour, 0);
            int xs[2] = {l.x0 + ox, l.x1 + ox}, ys[2] = {l.y0 + oy, l.y1 + oy};
            for (int k = 0; k < 2; k++) { if (xs[k] < bx0) bx0 = xs[k]; if (xs[k] > bx1) bx1 = xs[k]; if (ys[k] < by0) by0 = ys[k]; if (ys[k] > by1) by1 = ys[k]; }
        }
        if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0; if (bx1 > 639) bx1 = 639; if (by1 > 479) by1 = 479;
        if (bx1 >= bx0 && by1 >= by0) {
            MarkerLink l = {}; l.actor = g_targetActor; l.w = (float)(bx1 - bx0 + 1); l.h = (float)(by1 - by0 + 1);
            l.offX = (bx0 + bx1) * 0.5f - 320.f; l.offY = (by0 + by1) * 0.5f - 240.f; l.keepOffset = true;
            blobs[nb] = {bx0, by0, bx1, by1, tz / g_worldScale, 3}; links[nb] = l; arrow[nb] = false; nb++;
        }
    }
    bool useCache = g_targetActor && !octCache.empty() && octWorldW > 0;
    if (useCache) {
        if (oct >= 0) { for (int i = oct; i < nb - 1; i++) { blobs[i] = blobs[i + 1]; links[i] = links[i + 1]; arrow[i] = arrow[i + 1]; } nb--; }   // replace the game's
        if (nb < 10) {
            VrMarkerBlob bb = {0, 0, octW - 1, octH - 1, -1.0f, 2};
            MarkerLink l = {}; l.actor = g_targetActor; l.w = (float)octW; l.h = (float)octH; l.worldW = octWorldW; l.worldH = octWorldH;
            blobs[nb] = bb; links[nb] = l; nb++;
        }
    } else if (!g_targetActor) { octCache.clear(); octWorldW = octWorldH = 0; }
    both.assign((size_t)w * 4 * h, 0);                        // [ markers | lead indicator | cached octagon | lock reticle ]
    for (int y = 0; y < h; y++) { memcpy(&both[(size_t)y * w * 4], &mk[(size_t)y * w], w); memcpy(&both[(size_t)y * w * 4 + w], &pip[(size_t)y * w], w); }
    if (useCache) for (int y = 0; y < octH && y < h; y++) memcpy(&both[(size_t)y * w * 4 + 2 * w], &octCache[(size_t)y * octW], octW < w ? octW : w);
    if (w == 640 && h == 480) for (int y = 0; y < h; y++) memcpy(&both[(size_t)y * w * 4 + 3 * w], &lockImg[(size_t)y * 640], 640);
    uint8_t pal[1024]; GetPalRGBX(pal);
    memcpy(g_mkLink, links, sizeof links); g_nMkLink = nb;
    for (int i = 0; i < nb; i++) if (blobs[i].layer == 0 && !links[i].actor) blobs[i].depthM = -2.0f;   // HUD glass
    // the game's 2D layer (HUD, markers) is displayed by p2den with red and blue swapped against the 3D palette (see
    // UpdateHudSwapLut): the markers get that palette too, so they show the flat game's colours (red / yellow, not blue)
    uint8_t mkPal[1024]; memcpy(mkPal, pal, sizeof mkPal);
    for (int c = 0; c < 256; c++) { mkPal[c*4] = pal[c*4 + 2]; mkPal[c*4 + 2] = pal[c*4]; }
    g_mkSerial = Vr_SubmitMarkers(both.data(), w * 4, h, mkPal, blobs, nb);
    // exact 3D poses for this marker set right away, in the same (smoothed) state as the eye images on screen,
    // so the headset never shows the game's 30 Hz placement in between (that caused a jumping/double marker)
    g_posesWithEyes = false; ApplyBlend(BlendFactor()); UpdateMarkerPoses(); RestoreBlend(); g_posesWithEyes = true;
    if (g_hudDumpNext && g_game) {   // debug: every actor in the foreground world, with screen projection
        uint8_t* ship = CameraRef(); const float K = 461.0357f;
        if (ship) {
            const float* sm = (const float*)(ship + ACTOR_T_MAT);
            struct It { uint8_t* a; int depth; float M[12]; };
            std::vector<It> st; uint8_t* wfg = *(uint8_t**)ADDR_WORLD_FG;
            float I[12] = {1,0,0, 0,1,0, 0,0,1, 0,0,0};
            for (uint8_t* a = *(uint8_t**)(wfg + 8); a; a = *(uint8_t**)a) { It it; it.a = a; it.depth = 1; memcpy(it.M, I, 48); st.push_back(it); }
            int logged = 0;
            while (!st.empty() && logged < 120) {
                It it = st.back(); st.pop_back();
                const float* l = (const float*)(it.a + ACTOR_T_MAT); const float* P = it.M;
                float W[12];   // world = local * parent (row vectors)
                for (int r = 0; r < 4; r++) for (int c = 0; c < 3; c++) {
                    float lr0 = l[r*3], lr1 = l[r*3+1], lr2 = l[r*3+2];
                    W[r*3+c] = lr0 * P[c] + lr1 * P[3+c] + lr2 * P[6+c] + (r == 3 ? P[9+c] : 0); }
                float d[3] = {W[9] - sm[9], W[10] - sm[10], W[11] - sm[11]}, v[3];
                for (int k = 0; k < 3; k++) v[k] = d[0]*sm[k*3] + d[1]*sm[k*3+1] + d[2]*sm[k*3+2];
                float px = v[2] < -0.1f ? 320.f + v[0] * K / -v[2] : -9999, py = v[2] < -0.1f ? 240.f - v[1] * K / -v[2] : -9999;
                uint8_t* kids = *(uint8_t**)(it.a + 8);
                Log("[actors] %*s%p type %u ttype %u model %p kids %s | ship-space (%.1f %.1f %.1f) screen (%.0f,%.0f)%s",
                    it.depth * 2, "", it.a, it.a[0x12], *(uint16_t*)(it.a + ACTOR_T_TYPE), *(void**)(it.a + 0x18), kids ? "yes" : "no",
                    v[0], v[1], v[2], px, py, it.a == ship ? " <- own ship" : "");
                logged++;
                if (kids && it.depth < 4) for (uint8_t* k = kids; k; k = *(uint8_t**)k) { It c; c.a = k; c.depth = it.depth + 1; memcpy(c.M, W, 48); st.push_back(c); }
            }
        }
    }
    if (g_hudDumpNext) {   // debug: marker matching details on dump
        for (int i = 0; i < nb; i++) {
            float cx = (blobs[i].x0 + blobs[i].x1) * 0.5f, cy = (blobs[i].y0 + blobs[i].y1) * 0.5f;
            Log("[markers] dump #%d %s box (%d,%d)-(%d,%d) centre %.0f,%.0f depth %.2f m | actor %p offset px (%.1f, %.1f)",
                i, blobs[i].layer ? "target" : "mark", blobs[i].x0, blobs[i].y0, blobs[i].x1, blobs[i].y1, cx, cy, blobs[i].depthM,
                links[i].actor, links[i].offX, links[i].offY);
        }
    }
    g_pipCopy = pip; g_nPipCopy = 0;                          // lead indicator source for the eye images
    for (int i = 0; i < nb; i++) if (blobs[i].layer == 1 && blobs[i].depthM != -2.0f && g_nPipCopy < 8) g_pipCopyRects[g_nPipCopy++] = {blobs[i].x0, blobs[i].y0, blobs[i].x1, blobs[i].y1};
    for (size_t i = 0; i < mk.size(); i++) if (pip[i]) mk[i] = pip[i];      // mirror below gets everything
    static int lastNb = -1; if (nb != lastNb) { lastNb = nb; char t[256]; int o = sprintf_s(t, "[markers] %d:", nb);
        for (int i = 0; i < nb && o < 230; i++) o += sprintf_s(t + o, sizeof t - o, " (%s%d,%d %.1fm)", blobs[i].layer ? "target " : "", (blobs[i].x0 + blobs[i].x1) / 2, (blobs[i].y0 + blobs[i].y1) / 2, blobs[i].depthM);
        Log("%s", t); }
    if (g_hudDumpNext) {
        br_pixelmap pm = *gc; pm.pixels = mk.data(); pm.base_x = pm.base_y = 0; pm.row_bytes = (int16_t)w;
        PALETTEENTRY pe[256]; GetPal(pe); char n[64]; sprintf_s(n, "markers_%02d.bmp", g_hudDumpIdx); WriteBmp(n, &pm, pe);
    }
    if ((int)g_scene.size() == w * h) {
        for (size_t i = 0; i < mk.size(); i++) if (mk[i]) g_scene[i] = mk[i];
        Capture_SetMirror(g_scene.data(), w, h, pal);
    }
}

// ---------------------------------------------------------------- 90 Hz smoothing
// The game updates at 30 Hz. Each update the transforms of all actors (both worlds) are snapshotted. Extra VR frames
// are rendered while p2den spin-waits for the next cycle (see smooth.cpp); for those, every actor's transform is
// blended between the last two snapshots (interpolation: one game frame of delay; extrapolation: none), the eyes are
// rendered with the newest head pose, and the exact pre-blend values are written back before returning to the game.
struct Snap { uint8_t* a; float m[12]; };
static std::vector<Snap> g_snapPrev, g_snapCur, g_blendSaved;
static std::unordered_map<uint8_t*, int> g_prevIdx;
static LONGLONG g_snapT, g_cycleStart, g_lastSpin;
static double g_period = 1000.0 / 30.0, g_renderCostMs = 5.0;
static LONG g_lastVrFrame = -1;
static int g_idleRenders, g_idleSkips;

static void CollectActors(uint8_t* a, std::vector<Snap>& out, int depth) {
    for (; a && depth < 8; a = *(uint8_t**)a) {
        if (*(uint16_t*)(a + ACTOR_T_TYPE) == 0) { Snap s; s.a = a; memcpy(s.m, a + ACTOR_T_MAT, 48); out.push_back(s); }
        uint8_t* kids = *(uint8_t**)(a + 8);
        if (kids) CollectActors(kids, out, depth + 1);
    }
}
static void TakeSnapshot() {
    LONGLONG now = Now();
    if (g_snapT) { double p = Ms(now - g_snapT); if (p > 10 && p < 100) g_period = g_period * 0.9 + p * 0.1; }
    g_snapT = now;
    g_snapPrev.swap(g_snapCur); g_snapCur.clear();
    uint8_t* wbg = *(uint8_t**)ADDR_WORLD_BG; uint8_t* wfg = *(uint8_t**)ADDR_WORLD_FG;
    if (wbg) CollectActors(*(uint8_t**)(wbg + 8), g_snapCur, 1);
    if (wfg) CollectActors(*(uint8_t**)(wfg + 8), g_snapCur, 1);
    g_prevIdx.clear();
    for (int i = 0; i < (int)g_snapPrev.size(); i++) g_prevIdx[g_snapPrev[i].a] = i;
}
static void Norm(float* v) { float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); if (l > 1e-12f) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static float Dot(const float* a, const float* b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
// f: 0 = previous snapshot, 1 = current, >1 = extrapolated
static void ApplyBlend(float f) {
    g_blendSaved.clear();
    if (g_snapPrev.empty() || f == 1.0f) return;
    for (auto& c : g_snapCur) {
        auto it = g_prevIdx.find(c.a); if (it == g_prevIdx.end()) continue;
        const float* p = g_snapPrev[it->second].m;
        if (!memcmp(p, c.m, 48)) continue;
        float dx = c.m[9] - p[9], dy = c.m[10] - p[10], dz = c.m[11] - p[11];
        if (dx*dx + dy*dy + dz*dz > 300.f * 300.f) continue;            // teleport / jump: don't blend
        Snap sv; sv.a = c.a; memcpy(sv.m, c.a + ACTOR_T_MAT, 48); g_blendSaved.push_back(sv);
        float m[12];
        for (int k = 0; k < 12; k++) m[k] = p[k] + (c.m[k] - p[k]) * f;
        // re-orthonormalise the rotation rows (Gram-Schmidt keeps handedness)
        float sx = sqrtf(Dot(c.m, c.m)), sy = sqrtf(Dot(c.m + 3, c.m + 3)), sz = sqrtf(Dot(c.m + 6, c.m + 6));
        float* x = m; float* y = m + 3; float* z = m + 6;
        Norm(x);
        float d = Dot(x, y); for (int k = 0; k < 3; k++) y[k] -= x[k] * d; Norm(y);
        float d1 = Dot(x, z), d2 = Dot(y, z); for (int k = 0; k < 3; k++) z[k] -= x[k] * d1 + y[k] * d2; Norm(z);
        for (int k = 0; k < 3; k++) { x[k] *= sx; y[k] *= sy; z[k] *= sz; }   // keep any actor scale
        memcpy(c.a + ACTOR_T_MAT, m, 48);
    }
}
static void RestoreBlend() {
    for (auto it = g_blendSaved.rbegin(); it != g_blendSaved.rend(); ++it) memcpy(it->a + ACTOR_T_MAT, it->m, 48);
    g_blendSaved.clear();
}
static float BlendFactor() {
    if (!g_smooth || !g_snapT) return 1.0f;
    double a = Ms(Now() - g_snapT) / g_period;
    if (g_smooth == 1) return (float)(a < 0 ? 0 : a > 1 ? 1 : a);
    return (float)(1.0 + (a < 0 ? 0 : a > 1 ? 1 : a));
}

// Distance (game units, along the view axis) of the nearest foreground object inside the game's own field of view
// (70 x 55 degrees around the boresight = where the crosshair and markers can be), from the left eye's depth buffer
// right after its foreground pass. The crosshair/marker plane is put in front of it, so no ship appears in front of
// the HUD symbols. Z-buffer: d/65535 = (1/hither - 1/z) / (1/hither - 1/yon).
static float g_lastBoreZ = -1, g_minSurf = -1, g_minSurf5 = -1, g_minCentre = -1;
static const float GAME_TAN_H = 320.0f / 461.0357f, GAME_TAN_V = 240.0f / 461.0357f;
static float HudFovDepth() {
    void* wfg = *(void**)ADDR_WORLD_FG; uint8_t* cam = g_cam[1] ? *(uint8_t**)((uint8_t*)g_cam[1] + ACTOR_TYPE_DATA) : nullptr;
    if (!wfg || !cam || !g_lastCol || !g_lastDep || !g_game) return -1;
    const int W = 160, H = 120;
    g_dpC.resize(W * H); g_dpD.resize(W * H * 2);
    br_pixelmap pc = *(br_pixelmap*)g_lastCol, pd = *(br_pixelmap*)g_lastDep;
    pc.pixels = g_dpC.data(); pd.pixels = g_dpD.data();
    pc.base_x = pc.base_y = pd.base_x = pd.base_y = 0; pc.width = pd.width = W; pc.height = pd.height = H;
    pc.row_bytes = W; pd.row_bytes = W * 2;
    memset(g_dpD.data(), 0xFF, g_dpD.size());
    // hide the player's own ship and shots for this measurement
    uint8_t* ship = *(uint8_t**)(g_game + GAME_SHIPS + *(int*)(g_game + GAME_SHIPIDX) * SHIP_STRIDE);
    std::vector<uint8_t*> hidden;
    for (uint8_t* a = *(uint8_t**)((uint8_t*)wfg + 8); a; a = *(uint8_t**)a)
        if (a[0x12] == 1 && (a == ship || IsBoltModel(*(void**)(a + 0x18)))) { a[0x12] = 0; hidden.push_back(a); }
    g_inEye2 = true;
    CallBegin(wfg, g_cam[1], &pc, &pd); CallAdd(wfg); CallEnd();   // game camera = exactly the HUD's field of view
    g_inEye2 = false;
    for (auto a : hidden) a[0x12] = 1;
    const uint16_t* d = (const uint16_t*)g_dpD.data(); uint16_t best = 0xFFFF;
    for (int i = 0; i < W * H; i++) if (d[i] < best) best = d[i];
    float hither = *(float*)(cam + 8), yon = *(float*)(cam + 12);
    g_dpHither = hither; g_dpYon = yon; g_dpValid = true;
    if (best >= 0xFFF0) return -1;
    float f = best / 65535.0f, inv = 1.0f / hither - f * (1.0f / hither - 1.0f / yon);
    return inv > 1e-6f ? 1.0f / inv : -1;
}

// Draw a rectangle of a 640x480 game HUD image into an eye image at infinity: each eye pixel is mapped through its
// direction (ship space) back to the game's projection (K = 461.0357 around 320,240). Index 0 = transparent.
static void DrawAtInfinity(uint8_t* buf, int W, int H, float tanH, float tanV, const float h[9],
                           const uint8_t* src, int rx0, int ry0, int rx1, int ry1) {
    if (rx0 < 0) rx0 = 0; if (ry0 < 0) ry0 = 0; if (rx1 > 639) rx1 = 639; if (ry1 > 479) ry1 = 479;
    if (rx1 < rx0 || ry1 < ry0) return;
    const float K = 461.0357f, sx = (W * 0.5f) / tanH, sy = (H * 0.5f) / tanV;
    int ex0 = W, ey0 = H, ex1 = -1, ey1 = -1;                     // bounding box of the rectangle in this eye
    for (int c = 0; c < 4; c++) {
        float gx = (c & 1) ? rx1 + 1.f : (float)rx0, gy = (c & 2) ? ry1 + 1.f : (float)ry0;
        float d[3] = {(gx - 320.f) / K, -(gy - 240.f) / K, -1.f};
        float ex = h[0]*d[0] + h[1]*d[1] + h[2]*d[2], ey = h[3]*d[0] + h[4]*d[1] + h[5]*d[2], ez = h[6]*d[0] + h[7]*d[1] + h[8]*d[2];
        if (ez >= -1e-3f) return;
        int px = (int)floorf(W * 0.5f + ex / -ez * sx), py = (int)floorf(H * 0.5f - ey / -ez * sy);
        if (px < ex0) ex0 = px; if (px > ex1) ex1 = px; if (py < ey0) ey0 = py; if (py > ey1) ey1 = py;
    }
    ex0 -= 2; ey0 -= 2; ex1 += 2; ey1 += 2;
    if (ex0 < 0) ex0 = 0; if (ey0 < 0) ey0 = 0; if (ex1 >= W) ex1 = W - 1; if (ey1 >= H) ey1 = H - 1;
    for (int v = ey0; v <= ey1; v++) {
        float ey = -(v + 0.5f - H * 0.5f) / sy;
        for (int u = ex0; u <= ex1; u++) {
            float ex = (u + 0.5f - W * 0.5f) / sx;
            // eye direction (ex, ey, -1) -> ship space (h rows are the eye axes in ship space)
            float dx = h[0]*ex + h[3]*ey - h[6], dy = h[1]*ex + h[4]*ey - h[7], dz = h[2]*ex + h[5]*ey - h[8];
            if (dz >= -1e-4f) continue;
            int gx = (int)floorf(320.f + dx / -dz * K), gy = (int)floorf(240.f - dy / -dz * K);
            if (gx < rx0 || gx > rx1 || gy < ry0 || gy > ry1) continue;
            uint8_t c = src[gy * 640 + gx];
            if (c) buf[v * W + u] = g_hudSwapLut[c];
        }
    }
}

// Crosshair area in this eye + which pixels a ship covers (depth written by the ship pass) -> headset shader.
static void SubmitCrosshairMask(int e, int W, int H, float tanH, float tanV, const float h[9]) {
    int r = (int)(g_retFracP2 * 640 * 0.5f + 0.5f);
    float gx0 = 320.f - r, gy0 = 240.f - r, gx1 = 320.f + r, gy1 = 240.f + r;
    const float K = 461.0357f, sx = (W * 0.5f) / tanH, sy = (H * 0.5f) / tanV;
    int bx0 = W, by0 = H, bx1 = -1, by1 = -1;
    for (int c = 0; c < 4; c++) {
        float gx = (c & 1) ? gx1 : gx0, gy = (c & 2) ? gy1 : gy0;
        float d[3] = {(gx - 320.f) / K, -(gy - 240.f) / K, -1.f};
        float ex = h[0]*d[0] + h[1]*d[1] + h[2]*d[2], ey = h[3]*d[0] + h[4]*d[1] + h[5]*d[2], ez = h[6]*d[0] + h[7]*d[1] + h[8]*d[2];
        if (ez >= -1e-3f) { Vr_SetEyeCrosshair(e, h, 0, 0, -1, -1, nullptr, 0, 0, 0, 0); return; }
        int px = (int)floorf(W * 0.5f + ex / -ez * sx), py = (int)floorf(H * 0.5f - ey / -ez * sy);
        if (px < bx0) bx0 = px; if (px > bx1) bx1 = px; if (py < by0) by0 = py; if (py > by1) by1 = py;
    }
    bx0 -= 2; by0 -= 2; bx1 += 2; by1 += 2;
    if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0; if (bx1 >= W) bx1 = W - 1; if (by1 >= H) by1 = H - 1;
    if (bx1 < bx0 || by1 < by0) { Vr_SetEyeCrosshair(e, h, 0, 0, -1, -1, nullptr, 0, 0, 0, 0); return; }
    static std::vector<uint8_t> occ; int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1; occ.resize((size_t)bw * bh);
    const uint16_t* dp = (const uint16_t*)g_vrDepth.data();
    for (int y = 0; y < bh; y++) { const uint16_t* row = dp + (size_t)(by0 + y) * W + bx0; uint8_t* o = &occ[(size_t)y * bw];
        for (int x = 0; x < bw; x++) o[x] = row[x] < 0xFFF0 ? 1 : 0; }
    Vr_SetEyeCrosshair(e, h, bx0, by0, bx1, by1, occ.data(), gx0, gy0, gx1, gy1);
}

// LOCAL-space poses of object-linked markers in the current (blended) state; called while the blend is applied.
static void UpdateMarkerPoses() {
    uint8_t* game = g_game; if (!game || !g_nMkLink) return;
    uint8_t* ship = CameraRef(); if (!ship) return;
    const float* sm = (const float*)(ship + ACTOR_T_MAT); const float K = 461.0357f;
    float pos[10][3] = {}, size[10][2] = {}; uint8_t valid[10] = {};
    for (int i = 0; i < g_nMkLink; i++) {
        uint8_t* a = g_mkLink[i].actor; if (!a) continue;
        const float* m = (const float*)(a + ACTOR_T_MAT);
        float d[3] = {m[9] - sm[9], m[10] - sm[10], m[11] - sm[11]}, v[3];
        for (int k = 0; k < 3; k++) v[k] = d[0]*sm[k*3] + d[1]*sm[k*3+1] + d[2]*sm[k*3+2];
        float dist = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        if (dist < 1.0f) continue;
        float z = dist;                                           // angular size is kept relative to the distance
        // object centre + the game's pixel offset of the marker, at the object's depth
        float x = v[0], y = v[1];                                 // brackets: exactly at the object's centre
        if (g_mkLink[i].keepOffset && v[2] < -1.0f) { x += g_mkLink[i].offX / K * -v[2]; y -= g_mkLink[i].offY / K * -v[2]; }   // lead indicator: offset kept
        pos[i][0] = x / g_worldScale; pos[i][1] = y / g_worldScale; pos[i][2] = v[2] / g_worldScale - g_observerBack;
        size[i][0] = g_mkLink[i].w / K * z / g_worldScale; size[i][1] = g_mkLink[i].h / K * z / g_worldScale;
        if (g_mkLink[i].worldW > 0) { size[i][0] = g_mkLink[i].worldW / g_worldScale; size[i][1] = g_mkLink[i].worldH / g_worldScale; }
        valid[i] = 1;
    }
    Vr_SetMarkerPoses(g_mkSerial, pos, size, valid, g_nMkLink, g_posesWithEyes);
}

static void RenderVrCore() {
    VrEyeTarget t; VrView v[2];
    if (!Vr_GetEyeTarget(&t) || !Vr_GetViews(v)) return;
    void* wbg = *(void**)ADDR_WORLD_BG; void* wfg = *(void**)ADDR_WORLD_FG;
    if (!wbg || !wfg || !g_cam[0] || !g_cam[1] || !g_lastCol) return;
    LONGLONG t0 = Now();
    g_lastVrFrame = Vr_FrameCounter();
    ApplyBlend(BlendFactor());
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
    float qh[4], dotq = 0; for (int k = 0; k < 4; k++) dotq += v[0].rawQuat[k] * v[1].rawQuat[k];
    float ql = 0; for (int k = 0; k < 4; k++) { qh[k] = v[0].rawQuat[k] + (dotq < 0 ? -v[1].rawQuat[k] : v[1].rawQuat[k]); ql += qh[k] * qh[k]; }
    ql = sqrtf(ql); for (int k = 0; k < 4; k++) qh[k] /= ql;
    float hh[9]; QuatRows(qh, hh);
    WidenShots();
    uint8_t* ownShip = nullptr;
    if (!g_showOwnShip && g_game) {
        ownShip = *(uint8_t**)(g_game + GAME_SHIPS + *(int*)(g_game + GAME_SHIPIDX) * SHIP_STRIDE);
        if (ownShip && ownShip[0x12] == 1) ownShip[0x12] = 0; else ownShip = nullptr;   // type 0 = not drawn
    }
    // the own ship's engine / afterburner flames are not its children (as on other ships) but a separate top-level actor
    // at the ship's position, with the flame models as children: hidden with the ship (eyes sit behind the ship)
    static uint8_t* hiddenParts[32]; int nHiddenParts = 0;
    if (ownShip) {
        const float* sm = (const float*)(ownShip + ACTOR_T_MAT);
        uint8_t* wfg = *(uint8_t**)ADDR_WORLD_FG;
        for (uint8_t* a = wfg ? *(uint8_t**)(wfg + 8) : nullptr; a; a = *(uint8_t**)a) {
            if (a == ownShip) continue;
            const float* m = (const float*)(a + ACTOR_T_MAT);
            float dx = m[9] - sm[9], dy = m[10] - sm[10], dz = m[11] - sm[11];
            if (dx * dx + dy * dy + dz * dz > 0.25f) continue;          // not at the ship's origin
            if (a[0x12] == 1 && nHiddenParts < 32) { a[0x12] = 0; hiddenParts[nHiddenParts++] = a; }
            for (uint8_t* k = *(uint8_t**)(a + 8); k && nHiddenParts < 32; k = *(uint8_t**)k)
                if (k[0x12] == 1) { k[0x12] = 0; hiddenParts[nHiddenParts++] = k; }
        }
    }
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
        mf[11] += g_observerBack * g_worldScale;                  // ship +Z = backwards
        SetCamProjection((uint8_t*)g_cam[0], fov, aspect); SetCamProjection((uint8_t*)g_cam[1], fov, aspect);

        g_vrPmC.pixels = Vr_EyeBuffer(e);
        memset(g_vrPmC.pixels, 0, (size_t)t.width * t.height);
        DrawStarsVr(g_vrPmC.pixels, t.width, t.height, t.tanHalfH, t.tanHalfV, h);
        memset(g_vrDepth.data(), 0xFF, dsz);
        CallBegin(wbg, g_cam[0], &g_vrPmC, &g_vrPmD); CallAdd(wbg); CallEnd();
        if (g_retInEyes) {   // crosshair and lead indicator at infinity: behind ships and dust, in front of planets
            int r = (int)(g_retFracP2 * 640 * 0.5f + 0.5f);
            if (g_hudCopy.size() == 640 * 480) DrawAtInfinity(g_vrPmC.pixels, t.width, t.height, t.tanHalfH, t.tanHalfV, h, g_hudCopy.data(), 320 - r, 240 - r, 319 + r, 239 + r);
        }
        float eyePos[3] = {v[e].pos[0] * g_worldScale, v[e].pos[1] * g_worldScale, (v[e].pos[2] + g_observerBack) * g_worldScale};
        DrawDustVr(g_vrPmC.pixels, t.width, t.height, t.tanHalfH, t.tanHalfV, h, eyePos);
        memset(g_vrDepth.data(), 0xFF, dsz);
        CallBegin(wfg, g_cam[1], &g_vrPmC, &g_vrPmD); CallAdd(wfg); CallEnd();
        DrawFlareVr(g_vrPmC.pixels, t.width, t.height, t.tanHalfH, t.tanHalfV, h, hh);
        if (g_vrDumpNext) {
            PALETTEENTRY pal[256]; GetPal(pal); char n[64];
            sprintf_s(n, "vr_%02d_%c.bmp", g_dumpCount, e ? 'R' : 'L'); WriteBmp(n, &g_vrPmC, pal);
            if (e == 0 && g_capDir[0]) WriteBmp("view.bmp", &g_vrPmC, pal, 4);   // small preview for the launcher
        }
    }
    g_inEye2 = false;
    UpdateMarkerPoses();
    if (ownShip) ownShip[0x12] = 1;
    for (int i = 0; i < nHiddenParts; i++) hiddenParts[i][0x12] = 1;
    RestoreShots();
    RestoreCam(s0); RestoreCam(s1);
    RestoreBlend();
    uint8_t pal[1024]; GetPalRGBX(pal);
    Vr_SubmitEyes(v, pal);
    double ms = Ms(Now() - t0); g_vrAccMs += ms; g_vrN++; if (ms > g_vrMaxMs) g_vrMaxMs = ms;
    g_renderCostMs = g_renderCostMs * 0.8 + ms * 0.2;
    if (g_vrDumpNext) {
        { uint8_t pp[1024]; bool cap = Capture_GetPalette(pp); if (!cap) GetPalRGBX(pp);
          Log("vr dump palette (%s): c0=%d,%d,%d c8=%d,%d,%d cf=%d,%d,%d fe=%d,%d,%d", cap ? "p2den" : "game6",
              pp[0xc0*4], pp[0xc0*4+1], pp[0xc0*4+2], pp[0xc8*4], pp[0xc8*4+1], pp[0xc8*4+2],
              pp[0xcf*4], pp[0xcf*4+1], pp[0xcf*4+2], pp[0xfe*4], pp[0xfe*4+1], pp[0xfe*4+2]); }
        Log("vr dump %d: head L pos %.3f %.3f %.3f quat %.3f %.3f %.3f %.3f", g_dumpCount,
            v[0].pos[0], v[0].pos[1], v[0].pos[2], v[0].quat[0], v[0].quat[1], v[0].quat[2], v[0].quat[3]);
        g_dumpCount++; g_vrDumpNext = false;
    }
}

// Extra frame while the game waits for its next cycle (called from p2den's spin-wait on the game thread).
static void GroundCheats();
static void IdleTick() {
    if (!g_frameDone || !g_smooth) return;
    LONGLONG now = Now(); g_lastSpin = now;
    if (Vr_FrameCounter() == g_lastVrFrame || !Vr_Active()) return;
    // only if it finishes before the next cycle is due
    if (Ms(now - g_cycleStart) + g_renderCostMs * 1.25 + 1.5 > g_period) { g_idleSkips++; g_lastVrFrame = Vr_FrameCounter(); return; }
    RenderVrCore();
    g_idleRenders++;
}

// BRender keeps the camera/viewport set up by the last BrZbSceneRenderBegin. The game projects its HUD markers
// (target brackets) with that cached state after the 3D passes, so our VR eye renders must not leave theirs behind:
// re-run Begin (no Add) for the game's own foreground camera and buffers.
// The render pass also records each object's on-screen box (used for the target octagon), so the game's own
// foreground pass is re-run (Add) with its camera and buffers: identical depth -> no pixel changes, boxes restored.
static void RestoreGameCamera(bool reRender) {
    void* wfg = *(void**)ADDR_WORLD_FG;
    if (!wfg || !g_cam[1] || !g_lastCol || !g_lastDep) return;
    bool was = g_inEye2; g_inEye2 = true;
    CallBegin(wfg, g_cam[1], g_lastCol, g_lastDep); if (reRender) CallAdd(wfg); CallEnd();
    g_inEye2 = was;
}

// Once per game cycle, after the game's own foreground pass.
static void RenderVrEyes() {
    if (!g_lastCol) return;
    br_pixelmap* gc = (br_pixelmap*)g_lastCol;
    TakeSnapshot();
    RenderVrCore();
    Vr_NoteFlightFrame();
    {   // closest surface in the HUD's view (also used for the optional dynamic crosshair depth)
        float z = HudFovDepth();
        if (g_retDynamic) Vr_SetReticleDistance(z > 0 ? z / g_worldScale * 0.95f : -1.0f);
        g_lastBoreZ = z;
        if (z > 0 && (g_minSurf < 0 || z < g_minSurf)) g_minSurf = z;
        if (z > 0 && (g_minSurf5 < 0 || z < g_minSurf5)) g_minSurf5 = z;
    }
    RestoreGameCamera(true);   // after our extra renders: the game's own camera state and object screen boxes
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

// ---------------------------------------------------------------- VR controllers -> game
// Axes are written straight into the player's flight-control values (pitch +0x12a8, roll +0x12ac, yaw +0x12b0) at
// 0x4503f6: after all of the game's and p2den's input handling, before the game's own damage scaling and invert
// option. The throttle sets the speed setpoint (+0x1298) only while the throttle stick is used. Buttons are sent
// as real key presses (scancodes), so they pass through p2den's input layer like a keyboard.
static float g_throttle = -1; static LONGLONG g_lastCtl; static uint64_t g_prevButtons; static bool g_ctlOn;
static volatile float g_pitchValue, g_yawValue, g_speedValue; static volatile bool g_speedActive;

static void SendKey(uint8_t sc, bool down) {
    INPUT in = {}; in.type = INPUT_KEYBOARD; in.ki.wScan = sc;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP) | (sc == 0x1C && false ? KEYEVENTF_EXTENDEDKEY : 0);
    in.ki.dwExtraInfo = P2VR_INPUT_TAG;   // (recorded by the input recorder like real input)
    SendInput(1, &in, sizeof in);
}
static bool GameInFront() { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); return pid == GetCurrentProcessId(); }
static void CommandKey(int i, bool down) {   // the game's key for a command, with left Alt where needed
    if (!g_keyFor[i]) return;                                   // no game key (cheats)
    if (down) { if (g_altFor[i]) SendKey(0x38, true); SendKey(g_keyFor[i], true); }
    else { SendKey(g_keyFor[i], false); if (g_altFor[i]) SendKey(0x38, false); }
}
static void ReleaseVrKeys() {
    for (int i = 0; i < kVrCommandCount; i++) if (g_prevButtons & (1ull << i)) CommandKey(i, false);
    g_prevButtons = 0;
}
// A joystick / HOTAS stick is deflected: its X, Y or rudder/twist (R) axis away from centre (winmm; the throttle,
// usually Z or a slider, rests anywhere and is not looked at). The game's joystick layer (p2den, set up with Alt+J) feeds
// the stick into the flight controls whatever the game's control method, so this decides when its values pass through.
struct JoyDev { UINT id; JOYCAPSA caps; };
static const std::vector<JoyDev>& JoyDevices() {
    static std::vector<JoyDev> devs; static DWORD lastScan;
    if (!lastScan || GetTickCount() - lastScan > 3000) {   // (re)list the connected devices
        lastScan = GetTickCount(); devs.clear();
        UINT n = joyGetNumDevs();
        for (UINT j = 0; j < n && j < 16; j++) {
            JOYINFOEX ji = {sizeof ji, JOY_RETURNALL}; if (joyGetPosEx(j, &ji) != JOYERR_NOERROR) continue;
            JoyDev d = {j}; if (joyGetDevCapsA(j, &d.caps, sizeof d.caps) == JOYERR_NOERROR) devs.push_back(d);
        }
    }
    return devs;
}
// a connected joystick that has a p2den mapping (controllers\<vid><pid>_*.joy, made with Alt+J): not e.g. a virtual gamepad
static bool JoystickConfigured() {
    static DWORD last; static bool configured;
    if (last && GetTickCount() - last < 3000) return configured;
    last = GetTickCount(); configured = false;
    for (auto& dv : JoyDevices()) {
        char pat[MAX_PATH]; sprintf_s(pat, "%s\\controllers\\%04x%04x_*.joy", g_dir, dv.caps.wMid, dv.caps.wPid);
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) { FindClose(h); configured = true; }
    }
    return configured;
}
static bool JoystickDeflected() {
    const auto& devs = JoyDevices();
    auto off = [](DWORD v, UINT lo, UINT hi) { float h = (hi - lo) * 0.5f; return h > 0 && fabsf(((float)v - lo - h) / h) > 0.12f; };
    for (auto& d : devs) {
        JOYINFOEX ji = {sizeof ji, JOY_RETURNX | JOY_RETURNY | JOY_RETURNR};
        if (joyGetPosEx(d.id, &ji) != JOYERR_NOERROR) continue;
        if (off(ji.dwXpos, d.caps.wXmin, d.caps.wXmax) || off(ji.dwYpos, d.caps.wYmin, d.caps.wYmax)) return true;
        if ((d.caps.wCaps & JOYCAPS_HASR) && off(ji.dwRpos, d.caps.wRmin, d.caps.wRmax)) return true;
    }
    return false;
}
// Once per game cycle (game thread): compute the values the roll-write hook puts into the flight controls.
static void UpdateControlMode() {
    VrControls c = {};
    bool on = g_ctlEnable && g_vrEnable && Vr_Active() && !Macro_Replaying() && Vr_GetControls(&c) && g_game;   // replay: VR controls off
    if (on != g_ctlOn) { Log("[ctl] VR flight controls %s", on ? "on" : "off"); if (!on) { ReleaseVrKeys(); g_rollActive = false; g_speedActive = false; } }
    g_ctlOn = on;
    if (!on) return;
    LONGLONG now = Now(); double dt = g_lastCtl ? Ms(now - g_lastCtl) / 1000.0 : 0; g_lastCtl = now; if (dt > 0.2) dt = 0.2;
    uint8_t* entry = g_game + *(int*)(g_game + GAME_PLAYER) * SHIP_STRIDE;
    float maxSpd = *(float*)(0x4c6924 + (*(int*)(entry + 0x11e6) >> 16) * 0x48);
    // axes in the game's pre-invert joystick convention: pitch = joystick Y (forward negative), yaw = roll = -X
    g_pitchValue = -c.pitch * g_pitchGain * (g_invertPitch ? -1.f : 1.f);
    g_yawValue   = -c.yaw * g_yawGain;
    g_rollValue  = -c.roll * g_rollGain * (g_invertRoll ? -1.f : 1.f);
    g_rollActive = true;
    // Joystick pass-through: an axis is only overridden while its VR stick is deflected, so a joystick configured in p2den
    // (Alt+J) works alongside. With the game's control method set to joystick (0x52e062 low bits 1 or 2) always; with the
    // mouse method while a joystick stick is deflected (otherwise the VR values, centred, keep a parked mouse from steering).
    int method = *(uint8_t*)ADDR_CTRL_METHOD & 3;
    bool joyMoved = g_joyPass && JoystickDeflected();
    bool pass = g_joyPass && (method != 0 || joyMoved);
    g_pitchOn = !pass || c.pitch != 0; g_yawOn = !pass || c.yaw != 0; g_rollOn = !pass || c.roll != 0;
    if (joyMoved) {   // diagnostic: what the game's flight controls hold while the joystick is used
        static DWORD lastJoyLog;
        if (GetTickCount() - lastJoyLog > 2000) { lastJoyLog = GetTickCount();
            Log("[ctl] joystick deflected: game axes pitch %.2f yaw %.2f roll %.2f | VR override pitch %d yaw %d roll %d",
                *(float*)(entry + 0x12a8), *(float*)(entry + 0x12b0), *(float*)(entry + 0x12ac), g_pitchOn, g_yawOn, g_rollOn); }
    }
    {   static int lastPass = -1; if ((int)pass != lastPass) { lastPass = pass;
            Log("[ctl] joystick pass-through %s (control byte %02x: %s, joystick %s)", pass ? "on" : "off", *(uint8_t*)ADDR_CTRL_METHOD,
                method ? "joystick" : "mouse", joyMoved ? "deflected" : "centred"); } }
    // throttle: follow the game's setting unless the throttle stick / speed-zero is used
    if (maxSpd > 0 && c.throttleRate != 0) {   // (speed keys like "speed to zero" are the game's own commands)
        if (!g_speedActive) { g_throttle = *(float*)(entry + 0x1298) / maxSpd; }
        g_throttle += (float)(c.throttleRate * g_throttleRate * dt);
        if (g_throttle < 0) g_throttle = 0; if (g_throttle > 1) g_throttle = 1;
        g_speedValue = g_throttle * maxSpd; g_speedActive = true;
    } else g_speedActive = false;
    // buttons -> key presses (only into the game window)
    bool front = GameInFront();
    for (int i = 0; i < kVrCommandCount; i++) {
        bool down = front && ((c.buttons >> i) & 1), was = (g_prevButtons >> i) & 1;
        if (down != was && g_keyFor[i]) CommandKey(i, down);   // (scancode 0: a cheat, see RunCheats)
    }
    g_prevButtons = front ? c.buttons : 0;
}
extern "C" void __cdecl ApplyStick() {}

// ---------------------------------------------------------------- cheats
// Triggered by Ctrl+1..5 (keyboard), the SteamVR actions "[CHEAT] ..." (commands.h, bindable in SteamVR) or joystick buttons
// (p2vr.ini CHEAT_<NAME>_BUTTON = Windows button number, any connected joystick). Run on the game thread each flight frame.
//   restore:  shields and armour of the 4 sides to the ship type's maximum (0x4c694e / 0x4c6952 + type*0x48), hull armour
//             condition (game+0x27896, 0..255) to 255
//   destroy:  the game's damage function FUN_0044d98c (eax game, edx ship, ebx damage, ecx side or -1) after setting the
//             ship's shields to 0 and armour to 1, with the player as attacker (+0x1222): explosion, kill count and bounty
//             as for a real kill
//   enemies:  ships 29..148 in the player's area (+0x13b2 == 0x52e2fc), targetable type, not dying (+0x12ee & 0x20), and
//             hostile (attitude short +0x12dc >= 4, or 3 while 0x542288 >= 1, as "target nearest hostile") or targeting
//             the player
//   cash:     FUN_004469e0 (eax game, edx amount): adds to the credits (0x52e23c) and shows the game's credit message
//   invincible (toggle): game+0x27d0c held at 0, which the damage function treats as "the player takes no damage"
//   jump direct: the player's "direct jump" flag (short +0x13b6). A jump goes to the next nav point of the route, the routing
//             table 0x4cdadc[here * 0xe2 + destination] (here = short +0x13b2, destination = short +0x13b4, picked on the nav
//             map); with the flag set it goes straight to the destination (the game clears the flag after the jump and
//             sends the escorts the same way)
//   land now: the flight loop lands the player as soon as game+0x1144 (landing location) is not -1 (FUN_00438137: location
//             -> 0x52d280, then the session ends and the base follows). The game sets it after the approach and comms:
//             FUN_0043bf47 (cleared, +0x117c == 2, within 600 of the nearest base +0x278f4 of the area: base + 0x12; base 0:
//             0x12 when 0x52d2a8) and FUN_0044b180 (planet, cleared: +0x278fc). The cheat sets the same value directly.
//   fast forward (toggle, flight only): everything runs 2x. Frames: p2den paces the game at 30 Hz on QueryPerformanceCounter,
//             which runs twice as fast for it (Smooth_SetSpeed). Game clock (0x4f0248, 100 Hz): a Miles timer, set to 200 Hz
//             with the game's own AIL_set_timer_frequency (import 0x570500, timer handle 0x4f04ec). Back to normal when the
//             space session ends (SpaceSessionThunk).
enum { CHEAT_RESTORE, CHEAT_TARGET, CHEAT_ENEMIES, CHEAT_CASH, CHEAT_INVINCIBLE, CHEAT_JUMP, CHEAT_LAND, CHEAT_SPEED, CHEAT_COUNT };
static const char* kCheatName[CHEAT_COUNT] = {"RESTORE", "DESTROY_TARGET", "DESTROY_ENEMIES", "CASH", "INVINCIBLE", "JUMP", "LAND", "SPEED"};
static const char* kCheatAction[CHEAT_COUNT] = {"cheat_restore", "cheat_destroy_target", "cheat_destroy_enemies", "cheat_cash", "cheat_invincible", "cheat_jump_direct", "cheat_land", "cheat_fast_forward"};
// Red confirmation message of a cheat, drawn into the game's HUD image (PostHud), so it shows on the desktop and in the
// headset (on the HUD glass, centred above the crosshair, clear of the HUD panels) for 2.5 s.
static char g_cheatMsg[96]; static DWORD g_cheatMsgUntil; static std::vector<uint8_t> g_cheatMask; static int g_cheatMaskFor = -1;
static int g_cheatTextX0 = 0, g_cheatTextX1 = 640;   // horizontal extent of the text in the mask
static const int CHEAT_MSG_Y = 132, CHEAT_MSG_H = 22;
static void CheatMessage(const char* fmt, ...) {
    va_list a; va_start(a, fmt); vsprintf_s(g_cheatMsg, fmt, a); va_end(a);
    g_cheatMsgUntil = GetTickCount() + 2500; g_cheatMaskFor = -1;
}
// drawn into p2den's 8-bit frame upload (Capture overlay callback): 640x480, or its window-size 2D layer where the game's
// 4:3 picture is centred and scaled
static void DrawCheatOverlay(uint8_t* data, int pitch, int W, int H) {
    if (!data) return;
    bool showing = g_cheatMsg[0] && (int)(GetTickCount() - g_cheatMsgUntil) <= 0;
    if (showing) {   static DWORD lastLog; if (GetTickCount() - lastLog > 2000) { lastLog = GetTickCount(); Log("[cheat] message drawn into a %dx%d frame (pitch %d): %s", W, H, pitch, g_cheatMsg); } }
    static int msgSerial; static char drawn[96];
    if (strcmp(drawn, g_cheatMsg) || g_cheatMaskFor < 0) {   // text -> 640 x 22 mask, through GDI
        strcpy_s(drawn, g_cheatMsg); g_cheatMaskFor = ++msgSerial;
        HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = 640; bi.bmiHeader.biHeight = -CHEAT_MSG_H;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr; HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        g_cheatMask.assign(640 * CHEAT_MSG_H, 0);
        if (bmp) {
            HGDIOBJ ob = SelectObject(dc, bmp);
            HFONT f = CreateFontA(18, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, "Arial");
            HGDIOBJ of = SelectObject(dc, f); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
            RECT r = {0, 0, 640, CHEAT_MSG_H}; DrawTextA(dc, drawn, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            GdiFlush();
            const uint8_t* px = (const uint8_t*)bits;
            for (int i = 0; i < 640 * CHEAT_MSG_H; i++) g_cheatMask[i] = px[i * 4 + 1] > 128;
            g_cheatTextX0 = 640; g_cheatTextX1 = 0;
            for (int i = 0; i < 640 * CHEAT_MSG_H; i++) if (g_cheatMask[i]) { int x = i % 640; if (x < g_cheatTextX0) g_cheatTextX0 = x; if (x + 1 > g_cheatTextX1) g_cheatTextX1 = x + 1; }
            SelectObject(dc, of); DeleteObject(f); SelectObject(dc, ob); DeleteObject(bmp);
        }
        DeleteDC(dc);
    }
    // the colour that shows red: in flight p2den displays the HUD layer with red and blue swapped against the palette
    // (UpdateHudSwapLut); the ground screens (menus, bases, PAD) use the palette as it is
    bool flight = GetTickCount() - g_lastFlightTick < 500;
    uint8_t pal[1024]; GetPalRGBX(pal);
    // Whether p2den shows this screen's palette as it is or with red and blue swapped differs between screens: measured by
    // comparing the frame's indices with what p2den displays (its 640x480 colour readback, refreshed every second).
    static int swapped = -1; static DWORD lastCheck;
    if (flight) swapped = 1;
    else if (W == 640 && H == 480 && GetTickCount() - lastCheck > 500) {
        lastCheck = GetTickCount();
        static std::vector<uint8_t> rgb(640 * 480 * 3);
        if (Capture_GetHudRGB(rgb.data())) {
            long long errN = 0, errS = 0; int n = 0;
            for (int y = 0; y < 480; y += 4) {
                if (y >= CHEAT_MSG_Y - 4 && y < CHEAT_MSG_Y + CHEAT_MSG_H + 4) continue;   // (our own message area)
                for (int x = 0; x < 640; x += 4) {
                    int c = data[(size_t)y * pitch + x]; const uint8_t* q = &pal[c * 4]; const uint8_t* v = &rgb[(y * 640 + x) * 3];
                    if (abs(q[0] - q[2]) < 48) continue;                                // only colours where it matters
                    errN += abs(q[0] - v[0]) + abs(q[2] - v[2]); errS += abs(q[2] - v[0]) + abs(q[0] - v[2]); n++;
                }
            }
            int was = swapped;
            if (n > 50) { if (errS * 10 < errN * 7) swapped = 1; else if (errN * 10 < errS * 7) swapped = 0; }
            if (swapped != was) Log("[cheat] this screen shows the palette %s (%d samples)", swapped ? "with red and blue swapped" : "as it is", n);
        }
    }
    if (!showing) return;                                  // (the palette check above runs on every frame)
    int red = 1, best = 1 << 30, ri = swapped == 1 ? 2 : 0, bi = swapped == 1 ? 0 : 2;
    // the most vivid bright red the palette has (each screen has its own palette; a plain "closest to red" can be dull)
    best = -(1 << 30);
    for (int c = 1; c < 256; c++) { int R = pal[c*4+ri], G = pal[c*4+1], B = pal[c*4+bi], s = 2 * R - G - B + R / 2 - 3 * abs(G - B) / 2; if (s > best) { best = s; red = c; } }
    {   static DWORD lastLog; if (GetTickCount() - lastLog > 2000) { lastLog = GetTickCount();
            Log("[cheat] message colour index %d = %d,%d,%d (%s, palette from %s)", red, pal[red*4], pal[red*4+1], pal[red*4+2], flight ? "flight" : "ground", Capture_GetPalette(pal) ? "p2den" : "game"); } }
    float pw = H * 4.f / 3.f, ph = (float)H; if (pw > W) { pw = (float)W; ph = W * 3.f / 4.f; }   // the game's 4:3 picture
    float ox = (W - pw) * 0.5f, oy = (H - ph) * 0.5f, s = ph / 480.f;
    int y0 = (int)(oy + CHEAT_MSG_Y * s), y1 = (int)(oy + (CHEAT_MSG_Y + CHEAT_MSG_H) * s), x0 = (int)ox, x1 = (int)(ox + pw);
    int sh = s >= 2 ? (int)s : 1;                                                        // shadow offset
    {   // black box behind the text (index 0: black on the screens; see-through on the VR HUD glass), 8 px margin
        int bx0 = (int)(ox + (g_cheatTextX0 - 8) * s), bx1 = (int)(ox + (g_cheatTextX1 + 8) * s);
        for (int y = y0 > 0 ? y0 : 0; y < y1 && y < H; y++) { uint8_t* row = data + (size_t)y * pitch;
            for (int x = bx0 > 0 ? bx0 : 0; x < bx1 && x < W; x++) row[x] = 0; }
    }
    for (int y = y0; y < y1 && y < H; y++) {
        int my = (int)((y - oy) / s) - CHEAT_MSG_Y; if (my < 0 || my >= CHEAT_MSG_H) continue;
        uint8_t* row = data + (size_t)y * pitch;
        for (int x = x0; x < x1 && x < W; x++) {
            int mx = (int)((x - ox) / s); if (mx < 0 || mx >= 640 || !g_cheatMask[my * 640 + mx]) continue;
            if (y + sh < H && x + sh < W) data[(size_t)(y + sh) * pitch + x + sh] = 0;  // shadow (desktop; transparent on the VR HUD)
        }
        for (int x = x0; x < x1 && x < W; x++) {
            int mx = (int)((x - ox) / s); if (mx >= 0 && mx < 640 && g_cheatMask[my * 640 + mx]) row[x] = (uint8_t)red;
        }
    }
}
static bool g_cheats = true; static int g_cheatButton[CHEAT_COUNT]; static bool g_invincible; static int g_savedInvuln;
static int g_gameSpeed = 1;
static void WriteRgbBmp(const char* name, const uint8_t* rgb, int w, int h) {   // top-down RGB -> 24-bit BMP
    char path[MAX_PATH]; sprintf_s(path, "%s\\%s", g_capDir[0] ? g_capDir : g_dir, name);
    FILE* f; if (fopen_s(&f, path, "wb")) return;
    int stride = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh = {0x4D42, (DWORD)(54 + stride * h), 0, 0, 54};
    BITMAPINFOHEADER ih = {40, w, h, 1, 24, BI_RGB, (DWORD)(stride * h)};
    fwrite(&fh, 14, 1, f); fwrite(&ih, 40, 1, f);
    std::vector<uint8_t> row(stride, 0);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) { const uint8_t* s = rgb + (y * w + x) * 3; row[x*3] = s[2]; row[x*3+1] = s[1]; row[x*3+2] = s[0]; }
        fwrite(row.data(), stride, 1, f);
    }
    fclose(f); Log("wrote %s", path);
}
static void BeginCapture(bool flight) {
    char reports[MAX_PATH]; sprintf_s(reports, "%s\\p2vr-reports", g_dir); CreateDirectoryA(reports, nullptr);
    if (!g_capSession[0]) {
        SYSTEMTIME st; GetLocalTime(&st);
        sprintf_s(g_capSession, "%s\\%04d-%02d-%02d_%02d-%02d-%02d", reports, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        CreateDirectoryA(g_capSession, nullptr);
    }
    g_capCount++;
    sprintf_s(g_capDir, "%s\\capture-%02d", g_capSession, g_capCount); CreateDirectoryA(g_capDir, nullptr);
    g_capOpened = GetTickCount();
    char path[MAX_PATH]; sprintf_s(path, "%s\\context.txt", g_capDir);
    FILE* f; if (!fopen_s(&f, path, "w")) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "Privateer 2 VR bug report capture %d\n", g_capCount);
        fprintf(f, "mod version: %s\n", P2VR_VERSION);
        fprintf(f, "time: %04d-%02d-%02d %02d:%02d:%02d\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fprintf(f, "phase: %s\n", flight ? "flight" : "ground (menus / base / PAD / film)");
        fprintf(f, "VR session: %s\n", Vr_Active() ? "running" : "not running");
        fprintf(f, "game speed: %dx, invincible: %s, cheats: %s\n", g_gameSpeed, g_invincible ? "on" : "off", g_cheats ? "enabled" : "disabled");
        fprintf(f, "credits: %d\n", *(int*)0x52e23c);
        fprintf(f, "control method byte: %02x, joystick pass-through: %s\n", *(uint8_t*)ADDR_CTRL_METHOD, g_joyPass ? "on" : "off");
        if (flight && g_game) {
            int player = *(int*)(g_game + GAME_PLAYER); uint8_t* me = g_game + player * SHIP_STRIDE;
            fprintf(f, "player ship: index %d, type %d\n", player, *(int*)(me + 0x11e6) >> 16);
            fprintf(f, "shields: %d %d %d %d, armour: %d %d %d %d\n", *(int16_t*)(me + 0x12de), *(int16_t*)(me + 0x12e0), *(int16_t*)(me + 0x12e2),
                    *(int16_t*)(me + 0x12e4), *(int16_t*)(me + 0x12e6), *(int16_t*)(me + 0x12e8), *(int16_t*)(me + 0x12ea), *(int16_t*)(me + 0x12ec));
            fprintf(f, "target: %d, nav point: %d, destination: %d, area base: %d, planet: %d\n", *(int*)(me + 0x1216) >> 16,
                    *(int16_t*)(me + 0x13b2), *(int16_t*)(me + 0x13b4), *(int*)(g_game + 0x278f4), *(int*)(g_game + 0x278fc));
        }
        fprintf(f, "\nThe session log (p2vr.log) and settings (p2vr.ini) at the time of the capture are in this folder.\n");
        fclose(f);
    }
    Log("[report] capture %d: %s (%s)", g_capCount, g_capDir, flight ? "flight" : "ground");
}
static void FinishCapture() {
    if (!g_capDir[0]) return;
    char src[MAX_PATH], dst[MAX_PATH];
    sprintf_s(src, "%s\\p2vr.ini", g_dir); sprintf_s(dst, "%s\\p2vr.ini", g_capDir); CopyFileA(src, dst, FALSE);
    Log("[report] capture %d complete", g_capCount);
    if (g_log) fflush(g_log);
    sprintf_s(src, "%s\\p2vr.log", g_dir); sprintf_s(dst, "%s\\p2vr.log", g_capDir); CopyFileA(src, dst, FALSE);
    CheatMessage("CAPTURE %d SAVED FOR A BUG REPORT", g_capCount);
    g_capDir[0] = 0;
}
extern "C" void __cdecl SetGameSpeed(int speed) {   // game thread
    if (speed == g_gameSpeed) return;
    g_gameSpeed = speed;
    Smooth_SetSpeed(speed);
    int timer = *(int*)0x4f04ec;
    typedef int (__stdcall *PFN_AIL_set_timer_frequency)(int timer, int hz);
    PFN_AIL_set_timer_frequency setFreq = *(PFN_AIL_set_timer_frequency*)0x570500;
    if (timer >= 0 && setFreq) setFreq(timer, 100 * speed);
    Log("[cheat] game speed %dx (frames and game clock, timer %d)", speed, timer);
}
static void GameDamage(uint8_t* game, int ship, int damage) {
    __asm {
        pushad
        mov eax, game
        mov edx, ship
        mov ebx, damage
        mov ecx, -1
        mov esi, 0x44d98c
        call esi
        popad
    }
}
static void GameAddCredits(uint8_t* game, int amount) {
    __asm {
        pushad
        mov eax, game
        mov edx, amount
        mov esi, 0x4469e0
        call esi
        popad
    }
}
static bool DestroyShip(int i) {
    uint8_t* e = g_game + i * SHIP_STRIDE; int player = *(int*)(g_game + GAME_PLAYER);
    if (i < 0 || i >= 0x95 || i == player || (*(uint8_t*)(e + 0x12ee) & 0x20)) return false;
    for (int k = 0; k < 4; k++) { *(int16_t*)(e + 0x12de + k * 2) = 0; *(int16_t*)(e + 0x12e6 + k * 2) = 1; }
    *(int16_t*)(e + 0x1222) = (int16_t)player;                     // the kill is the player's
    GameDamage(g_game, i, 400);
    return (*(uint8_t*)(e + 0x12ee) & 0x20) != 0;
}
static void DoCheat(int which) {
    int player = *(int*)(g_game + GAME_PLAYER); uint8_t* me = g_game + player * SHIP_STRIDE;
    switch (which) {
    case CHEAT_RESTORE: {
        int type = *(int*)(me + 0x11e6) >> 16;
        for (int k = 0; k < 4; k++) {
            *(int16_t*)(me + 0x12de + k * 2) = *(const int16_t*)(0x4c694e + type * 0x48);
            *(int16_t*)(me + 0x12e6 + k * 2) = *(const int16_t*)(0x4c6952 + type * 0x48);
        }
        *(int16_t*)(g_game + 0x27896) = 255;
        Log("[cheat] shields %d and armour %d restored (ship type %d)", *(const int16_t*)(0x4c694e + type * 0x48), *(const int16_t*)(0x4c6952 + type * 0x48), type);
        CheatMessage("CHEAT: SHIELDS AND ARMOR RESTORED");
        break; }
    case CHEAT_TARGET: {
        int tgt = *(int*)(me + 0x1216) >> 16;
        if (tgt < 0) { Log("[cheat] destroy target: no target"); CheatMessage("CHEAT: NO TARGET"); break; }
        bool ok = DestroyShip(tgt);
        Log("[cheat] destroy target %d: %s", tgt, ok ? "destroyed" : "not destroyable");
        CheatMessage(ok ? "CHEAT: TARGET DESTROYED" : "CHEAT: THIS TARGET CANNOT BE DESTROYED");
        break; }
    case CHEAT_ENEMIES: {
        int zone = *(int*)0x52e2fc, n = 0, tried = 0;
        for (int i = 0x1d; i < 0x95; i++) {
            uint8_t* e = g_game + i * SHIP_STRIDE; int type = *(int*)(e + 0x11e6) >> 16;
            if (i == player || type == 0 || !*(const uint8_t*)(0x4c6918 + type * 0x48) || (*(int*)(e + 0x13b0) >> 16) != zone) continue;
            if (*(uint8_t*)(e + 0x12ee) & 0x20) continue;
            int rel = *(int*)(e + 0x12da) >> 16;
            bool hostile = rel >= 4 || (rel == 3 && *(int*)0x542288 >= 1) || (*(int*)(e + 0x1216) >> 16) == player;
            if (!hostile) continue;
            tried++; if (DestroyShip(i)) n++;
        }
        Log("[cheat] destroy all enemies: %d of %d destroyed", n, tried);
        if (n) CheatMessage(n == 1 ? "CHEAT: 1 ENEMY DESTROYED" : "CHEAT: %d ENEMIES DESTROYED", n); else CheatMessage("CHEAT: NO ENEMIES HERE");
        break; }
    case CHEAT_CASH:
        GameAddCredits(g_game, 100000);
        Log("[cheat] +100000 credits: %d", *(int*)0x52e23c);
        CheatMessage("CHEAT: +100,000 CREDITS");
        break;
    case CHEAT_INVINCIBLE:
        g_invincible = !g_invincible;
        if (g_invincible) g_savedInvuln = *(int*)(g_game + 0x27d0c); else *(int*)(g_game + 0x27d0c) = g_savedInvuln;
        Log("[cheat] invincible %s", g_invincible ? "on" : "off");
        CheatMessage(g_invincible ? "CHEAT: INVINCIBLE ON" : "CHEAT: INVINCIBLE OFF");
        break;
    case CHEAT_SPEED:
        SetGameSpeed(g_gameSpeed == 1 ? 2 : 1);
        CheatMessage(g_gameSpeed == 2 ? "CHEAT: FAST FORWARD 2X" : "CHEAT: NORMAL SPEED");
        break;
    case CHEAT_LAND: {
        int base = *(int*)(g_game + 0x278f4), planet = *(int*)(g_game + 0x278fc), where = -1;
        if (base >= 1 && base <= 0x10) where = base + 0x12;                     // a station / base in this area
        else if (base == 0 && *(int*)0x52d2a8 != 0) where = 0x12;               // the special base 0, when the game allows it
        else if (planet != -1) where = planet;                                  // a planet
        if (where < 0) { Log("[cheat] land now: nowhere to land in this area (base %d, planet %d)", base, planet); CheatMessage("CHEAT: NOWHERE TO LAND HERE"); break; }
        *(int*)(g_game + 0x1144) = where;
        Log("[cheat] land now: landing at location %d (base %d, planet %d)", where, base, planet);
        CheatMessage("CHEAT: LANDING");
        break; }
    case CHEAT_JUMP: {
        int here = *(int16_t*)(me + 0x13b2), dest = *(int16_t*)(me + 0x13b4);
        if (dest < 0 || dest >= 0xe2 || here < 0 || here >= 0xe2 || dest == here) {
            Log("[cheat] jump direct: no destination set (here %d, destination %d)", here, dest); CheatMessage("CHEAT: NO DESTINATION SET"); break; }
        *(int16_t*)(me + 0x13b6) = 1;
        CheatMessage("CHEAT: NEXT JUMP GOES STRAIGHT TO THE DESTINATION");
        Log("[cheat] jump direct: next jump goes from nav point %d straight to %d (the route's next stop was %d)", here, dest,
            *(const uint8_t*)(0x4cdadc + here * 0xe2 + dest));
        break; }
    }
}
// On the ground (bases, menus, PAD: no flight frame for 0.5 s) only "get cash" and "fast forward" apply; run from the idle
// callback, which p2den's frame pacing calls on the game thread in every phase. The other cheat keys are ignored there.
static void GroundCheats() {
    static DWORD last; DWORD now = GetTickCount();
    if (now - last < 20) return;
    last = now;
    {   // debug: a file named 'msgtest' in the game folder shows a test message (checks the message overlay)
        static DWORD lastChk; if (now - lastChk > 500) { lastChk = now; char f[MAX_PATH]; sprintf_s(f, "%s\\msgtest", g_dir);
            if (GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(f); CheatMessage("CHEAT: TEST MESSAGE"); g_cheatMsgUntil = now + 15000; Log("[cheat] test message"); } }
    }
    {   // bug-report capture on the ground (menus, bases, PAD): the displayed screen, context, settings and log
        static bool was; bool f9 = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F9) & 0x8000) && GameInFront();
        if (f9 && !was && now - g_lastFlightTick >= 500 && !g_capDir[0]) {
            BeginCapture(false);
            static std::vector<uint8_t> rgb(640 * 480 * 3);
            if (Capture_GetHudRGB(rgb.data())) WriteRgbBmp("screen.bmp", rgb.data(), 640, 480);
            FinishCapture();
        }
        was = f9;
    }
    if (!g_cheats || now - g_lastFlightTick < 500) return;
    int keys = Macro_TakeCheatKeys();
    bool held[CHEAT_COUNT] = {};
    for (auto& dv : JoyDevices()) {
        JOYINFOEX ji = {sizeof ji, JOY_RETURNBUTTONS};
        if (joyGetPosEx(dv.id, &ji) != JOYERR_NOERROR) continue;
        for (int k = 0; k < CHEAT_COUNT; k++) if (g_cheatButton[k] > 0 && g_cheatButton[k] <= 32 && ((ji.dwButtons >> (g_cheatButton[k] - 1)) & 1)) held[k] = true;
    }
    VrControls c = {};
    if (Vr_Active() && Vr_GetControls(&c))
        for (int i = 0; i < kVrCommandCount; i++) if ((c.buttons >> i) & 1)
            for (int k = 0; k < CHEAT_COUNT; k++) if (!strcmp(kVrCommands[i].action, kCheatAction[k])) held[k] = true;
    static bool was[CHEAT_COUNT]; bool down[CHEAT_COUNT] = {};
    for (int k = 0; k < CHEAT_COUNT; k++) { down[k] = ((keys >> k) & 1) || (held[k] && !was[k]); was[k] = held[k]; }
    if (down[CHEAT_CASH]) {
        *(int*)0x52e23c += 100000;                               // the player's credits (the in-flight routine also needs flight data)
        Log("[cheat] +100000 credits (ground): %d", *(int*)0x52e23c);
        CheatMessage("CHEAT: +100,000 CREDITS");
    }
    if (down[CHEAT_SPEED]) {
        SetGameSpeed(g_gameSpeed == 1 ? 2 : 1);
        CheatMessage(g_gameSpeed == 2 ? "CHEAT: FAST FORWARD 2X" : "CHEAT: NORMAL SPEED");
    }
}
static void RunCheats() {
    int keys = Macro_TakeCheatKeys();
    if (!g_cheats || !g_game) return;
    bool down[CHEAT_COUNT] = {};
    for (int k = 0; k < CHEAT_COUNT; k++) if ((keys >> k) & 1) down[k] = true;   // Ctrl+1..5: already edges
    bool held[CHEAT_COUNT] = {};
    VrControls c = {};
    if (Vr_Active() && Vr_GetControls(&c))
        for (int i = 0; i < kVrCommandCount; i++) if ((c.buttons >> i) & 1)
            for (int k = 0; k < CHEAT_COUNT; k++) if (!strcmp(kVrCommands[i].action, kCheatAction[k])) held[k] = true;
    for (auto& dv : JoyDevices()) {
        JOYINFOEX ji = {sizeof ji, JOY_RETURNBUTTONS};
        if (joyGetPosEx(dv.id, &ji) != JOYERR_NOERROR) continue;
        for (int k = 0; k < CHEAT_COUNT; k++) if (g_cheatButton[k] > 0 && g_cheatButton[k] <= 32 && ((ji.dwButtons >> (g_cheatButton[k] - 1)) & 1)) held[k] = true;
    }
    static bool was[CHEAT_COUNT];
    for (int k = 0; k < CHEAT_COUNT; k++) { if (held[k] && !was[k]) down[k] = true; was[k] = held[k]; }
    for (int k = 0; k < CHEAT_COUNT; k++) if (down[k]) DoCheat(k);
    if (g_invincible) *(int*)(g_game + 0x27d0c) = 0;
}

// "Entering Space in VR" prompt: on the first flight frame after a flat scene, the game thread waits (window kept
// responsive) until the fire button is pressed and released, so the space scene starts only when the player is ready.
// Fire = the VR "fire_guns" action, the mouse button(s) p2den maps to fire in space ([MOUSE_SPACE] BUTTON_0n=119 in
// p2_dark_en_ingame.ini), or the joystick button(s) p2den maps to fire guns (code 0x72 in the device's
// controllers\<vid><pid>_*.joy; without a mapping the game's own joystick handling fires with button 1).
static bool g_spacePrompt = true;
// FUN_00438137 is a whole space session (launch -> flight loop incl. nav map / options -> landing or quit). Its two call
// sites (0x41c0e2: launch from a base, 0x4227bb: other entry) go through SpaceSessionThunk, which flags a new mission.
static volatile bool g_newSpaceSession;
static const uintptr_t ADDR_SPACE_SESSION = 0x438137;
static uintptr_t g_spaceSessionFn = ADDR_SPACE_SESSION;
extern "C" void __cdecl SetGameSpeed(int speed);
extern "C" void __cdecl SpaceSessionEnded() {}   // (fast forward stays as toggled, in flight and on the ground)
__declspec(naked) static void SpaceSessionThunk() {   // eax = game; plain ret: call the session, then reset the game speed
    __asm {
        mov byte ptr [g_newSpaceSession], 1
        call dword ptr [g_spaceSessionFn]
        pushad
        call SpaceSessionEnded
        popad
        ret
    }
}
struct FireJoy { UINT id; DWORD mask; };
static std::vector<FireJoy> g_fireJoys; static int g_fireMouse[5]; static bool g_fireMapsRead;
static DWORD JoyFireMask(WORD vid, WORD pid) {   // p2den .joy: header 16 bytes (axis count at +12), 0x88 per axis,
    char pat[MAX_PATH]; sprintf_s(pat, "%s\\controllers\\%04x%04x_*.joy", g_dir, vid, pid);   // then button count and
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd); DWORD mask = 0;                    // 5 tables of codes
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        char path[MAX_PATH]; sprintf_s(path, "%s\\controllers\\%s", g_dir, fd.cFileName);
        FILE* f; if (!fopen_s(&f, path, "rb")) {
            std::vector<uint8_t> d(64 * 1024); size_t n = fread(d.data(), 1, d.size(), f); fclose(f);
            if (n >= 16 && !memcmp(&d[4], "P2DK", 4)) {
                uint32_t na = *(uint32_t*)&d[12]; size_t o = 16 + (size_t)na * 0x88;
                if (o + 4 <= n) { uint32_t nb = *(uint32_t*)&d[o];
                    // tables of nb dwords each: [0] menus, [1] in flight; values are p2den action numbers (46 = Fire Guns)
                    size_t t1 = o + 4 + (size_t)nb * 4;
                    for (uint32_t k = 0; k < nb && k < 32 && t1 + 4 * k + 4 <= n; k++) if (*(uint32_t*)&d[t1 + 4 * k] == 46) mask |= 1u << k; }
            }
            Log("[vr] joystick %04x:%04x mapping %s: fire buttons %08x", vid, pid, fd.cFileName, mask);
        }
    }
    return mask ? mask : 1u;   // unmapped: button 1
}
static void ReadFireMaps() {
    g_fireMapsRead = true; g_fireJoys.clear();
    char ini[MAX_PATH]; sprintf_s(ini, "%s\\p2_dark_en_ingame.ini", g_dir);
    for (int i = 0; i < 5; i++) { char k[16]; sprintf_s(k, "BUTTON_%02d", i + 1); g_fireMouse[i] = GetPrivateProfileIntA("MOUSE_SPACE", k, i == 0 ? 119 : 0, ini) == 119; }
    UINT n = joyGetNumDevs();
    for (UINT j = 0; j < n && j < 16; j++) {
        JOYINFOEX ji = {sizeof ji, JOY_RETURNBUTTONS}; if (joyGetPosEx(j, &ji) != JOYERR_NOERROR) continue;
        JOYCAPSA jc = {}; if (joyGetDevCapsA(j, &jc, sizeof jc) != JOYERR_NOERROR) continue;
        g_fireJoys.push_back({j, JoyFireMask(jc.wMid, jc.wPid)});
    }
    Log("[vr] fire buttons: mouse %d%d%d%d%d, %zu joystick(s)", g_fireMouse[0], g_fireMouse[1], g_fireMouse[2], g_fireMouse[3], g_fireMouse[4], g_fireJoys.size());
}
static bool FireDown() {
    if (!g_fireMapsRead) ReadFireMaps();
    static const int vks[5] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (int i = 0; i < 5; i++) if (g_fireMouse[i] && (GetAsyncKeyState(vks[i]) & 0x8000)) return true;
    VrControls c = {};
    if (Vr_GetControls(&c)) for (int i = 0; i < kVrCommandCount; i++) if (!strcmp(kVrCommands[i].action, "fire_guns") && ((c.buttons >> i) & 1)) return true;
    for (auto& fj : g_fireJoys) { JOYINFOEX ji = {sizeof ji, JOY_RETURNBUTTONS}; if (joyGetPosEx(fj.id, &ji) == JOYERR_NOERROR && (ji.dwButtons & fj.mask)) return true; }
    return false;
}
static void WaitForSpacePrompt() {
    Log("[vr] entering space: waiting for the fire button");
    g_fireMapsRead = false;   // pick up joysticks / mappings changed since the last time
    Vr_ShowMessage(1);
    int phase = 0;   // 0: wait until nothing is held, 1: wait for a press, 2: wait for its release
    while (phase < 3 && Vr_Active()) {
        MSG m; PeekMessageA(&m, nullptr, 0, 0, PM_NOREMOVE);   // keeps the window responsive (no "not responding")
        bool down = FireDown();
        if (phase == 0 && !down) phase = 1; else if (phase == 1 && down) phase = 2; else if (phase == 2 && !down) phase = 3;
        Sleep(15);
    }
    Vr_ShowMessage(0);
    Log("[vr] entering space: continue");
}

// Rumble when the player's ship is hit: the HUD's shield/armour display reads four shield values (16.16 at entry+0x12dc,
// stride 2 bytes, i.e. shorts at +0x12de..+0x12e4) and four armour values (shorts at +0x12e6..+0x12ec); maxima per ship
// type at 0x4c6950 / 0x4c6954 (+type*0x48). A drop of their total between game frames is a hit.
static void CheckHits() {
    static int lastTotal = -1, lastPlayer = -1; static DWORD lastTick;
    if (!g_rumble || g_rumbleHit <= 0 || !g_game || !Vr_Active()) { lastTotal = -1; return; }
    int player = *(int*)(g_game + GAME_PLAYER);
    const uint8_t* entry = g_game + player * SHIP_STRIDE;
    int total = 0; for (int k = 0; k < 8; k++) total += *(const int16_t*)(entry + 0x12de + k * 2);
    int type = *(const int*)(entry + 0x11e6) >> 16;
    int maxTotal = 4 * (*(const int16_t*)(0x4c6950 + type * 0x48) + *(const int16_t*)(0x4c6954 + type * 0x48));
    DWORD now = GetTickCount();
    if (lastTotal >= 0 && player == lastPlayer && now - lastTick < 1000 && total < lastTotal && maxTotal > 0) {
        float drop = (float)(lastTotal - total) / maxTotal;           // fraction of everything the ship has
        float amp = g_rumbleHit * (0.45f + drop * 8.0f); if (amp > 1) amp = 1;
        Vr_Haptic(2, amp, 0.12f + (drop > 0.05f ? 0.15f : 0.0f));
    }
    lastTotal = total; lastPlayer = player; lastTick = now;
}   // joystick path no longer used (kept as a no-op hook)

// ---------------------------------------------------------------- 2D sprite hook (menus / PAD)
// Both sprite blitters (0x41305a, 0x4144e0; cdecl view, shapes, id, x, y) pass through SpriteFilter, which can move a
// sprite before it is drawn. Used to put the PAD's cursor sprite exactly on the pointer (its drawing is offset).
static int g_cursorId = -1, g_cursorDx = 0, g_cursorDy = 0; static void* g_cursorShapes;
static bool g_spriteLog = false;
static void CursorGamePos(int* gx, int* gy) {   // Windows cursor -> game 640x480 coordinates (4:3 area of the window)
    *gx = *gy = -1;
    HWND w = nullptr; DWORD pid; POINT pt; if (!GetCursorPos(&pt)) return;
    w = WindowFromPoint(pt); if (!w) return; GetWindowThreadProcessId(w, &pid); if (pid != GetCurrentProcessId()) return;
    ScreenToClient(w, &pt); RECT r; GetClientRect(w, &r);
    float vw = r.bottom * 4.f / 3.f, vh = (float)r.bottom; if (vw > r.right) { vw = (float)r.right; vh = r.right * 3.f / 4.f; }
    float ox = (r.right - vw) * 0.5f, oy = (r.bottom - vh) * 0.5f;
    *gx = (int)((pt.x - ox) * 640.f / vw); *gy = (int)((pt.y - oy) * 480.f / vh);
}
// Offset screens: the blitter draws at view origin (view+4, view+8) + (x, y). Some screens (the PAD 0x436390, the nav map)
// draw their mouse cursor at the game's pointer position inside a view whose origin is not (0,0), e.g. the PAD at (60,23):
// the cursor shows right/down of the mouse, and the game's hit-testing follows the drawn cursor. So the drawing is right and
// the VR pointer must move the mouse to (beam - view origin). With CURSOR_FIX, the cursor draw is found as the sprite call
// (per call site: the PAD cursor is animated, its sprite id changes) whose position follows the mouse, and its view origin
// is handed to the VR pointer (Vr_SetCursorOffset).
// A sprite call site is the cursor when its position follows the mouse at a constant distance c (0 on the PAD and menus):
// c the same at two different resting positions. Several such sites (cursor, its shadow at +3): the smallest c wins.
// The VR pointer offset is then c + the view origin.
static bool g_cursorLog = false;   // CURSOR_LOG: every second, the draws that moved (to find a screen's cursor)
static void DetectCursorOffset(uint32_t ret, int x, int y, const int* view) {
    static DWORD posTick; static int mx = -1, my = -1;
    DWORD now = GetTickCount();
    if (now != posTick) { posTick = now; CursorGamePos(&mx, &my); }
    if (!view) return;
    static DWORD bestTick; static int bestMag = 1 << 30;
    // known cursor draws: the PAD (0x4364fd, sprite at the PAD pointer) and the nav map (0x45b6d8, sprite 0x34 at its
    // pointer game+0x13e/0x140): their view origin is the offset, whatever the pointer does (in the Joystick control
    // method the nav pointer also follows the stick, so it is not always exactly on the mouse)
    if (ret == 0x436502 || ret == 0x45b6dd) {
        Vr_SetCursorOffset(view[1], view[2]); bestTick = now; bestMag = 0;
        static uint32_t lastKnown; static int lastKx = -1, lastKy = -1;
        if (ret != lastKnown || view[1] != lastKx || view[2] != lastKy) { lastKnown = ret; lastKx = view[1]; lastKy = view[2];
            Log("[cursor] %s cursor, view at (%d,%d): VR pointer offset (%d,%d)", ret == 0x436502 ? "PAD" : "nav map", view[1], view[2], view[1], view[2]); }
        return;
    }
    if (mx < 0) return;
    struct Site { uint32_t ret; int x, y; DWORD moved; int dx, dy, stable; bool rest; int px, py, pdx, pdy; bool confirmed; int ox, oy; };
    static Site sites[128]; static int nSites;
    Site* s = nullptr;
    for (int i = 0; i < nSites; i++) if (sites[i].ret == ret) { s = &sites[i]; break; }
    if (!s) { if (nSites >= 128) return; s = &sites[nSites++]; *s = {}; s->ret = ret; s->x = x; s->y = y; }
    if (x != s->x || y != s->y) { s->x = x; s->y = y; s->moved = now; }
    s->ox = view[1]; s->oy = view[2];
    if (g_cursorLog) {
        static DWORD lastDiag;
        if (now - lastDiag > 1000) {
            lastDiag = now; char line[700]; int o = sprintf_s(line, "[cursor-diag] mouse (%d,%d), moved:", mx, my), shown = 0;
            for (int i = 0; i < nSites && shown < 8 && o < 600; i++) if (now - sites[i].moved < 1000) {
                o += sprintf_s(line + o, sizeof line - o, " [%08x at (%d,%d) view (%d,%d)]", sites[i].ret, sites[i].x, sites[i].y, sites[i].ox, sites[i].oy); shown++; }
            if (shown) Log("%s", line);
        }
    }
    int xy[2] = {x, y};
    int dx = xy[0] - mx, dy = xy[1] - my;
    if (abs(dx) > 200 || abs(dy) > 200) { s->stable = 0; return; }
    if (dx == s->dx && dy == s->dy) s->stable++; else { s->dx = dx; s->dy = dy; s->stable = 0; }
    if (s->stable < 6 || now - s->moved > 10000) return;          // at rest (the mouse sample may lag while moving)
    bool exact = abs(dx) <= 2 && abs(dy) <= 2;                      // drawn on the mouse: no confirmation needed
    if (!exact && !s->confirmed) {
        if (!s->rest || dx != s->pdx || dy != s->pdy) { s->rest = true; s->px = xy[0]; s->py = xy[1]; s->pdx = dx; s->pdy = dy; return; }
        if (xy[0] == s->px && xy[1] == s->py) return;              // same resting place: wait for a second one
        s->confirmed = true;
    }
    int mag = abs(dx) + abs(dy);
    if (now - bestTick < 3000 && mag > bestMag) return;             // a closer follower (the cursor itself) is active
    bestTick = now; bestMag = mag;
    int ox = view[1] + (exact ? 0 : dx), oy = view[2] + (exact ? 0 : dy);
    if (ox < -200 || ox >= 640 || oy < -200 || oy >= 480) return;
    Vr_SetCursorOffset(ox, oy);
    static int lastOx = 1 << 30, lastOy;
    if (ox != lastOx || oy != lastOy) {
        lastOx = ox; lastOy = oy;
        Log("[cursor] drawn from %08x, view at (%d,%d), %d,%d from the mouse: VR pointer offset (%d,%d)", ret, view[1], view[2], dx, dy, ox, oy);
    }
}
// Flat-screen sprites drawn after the flight HUD was finished: the screens opened during flight (nav map, diary, options,
// hotkeys) present their first frame from the cycle that rendered the 3D view, so that frame must go to the flat screen,
// not the HUD. Only those screens' call sites count (0x45a000..0x460000: nav map 0x45a..0x45b, the others 0x45c592..0x45cc90,
// 0x45d08a..0x45d526, 0x45f400..0x45f622): the flight HUD itself draws sprites late too (0x44c265).
static void NotePostHudDraw(uint32_t ret) {
    if (!g_frameDone || g_inEye2 || GetTickCount() - g_lastFlightTick > 1000) return;
    if (ret < 0x45a000 || ret >= 0x460000) return;
    Vr_NoteFlatDraw();
}
extern "C" void __cdecl SpriteFilter(int id, void* shapes, int* xy, int* view) {
    Smooth_SetGameThread();
    NotePostHudDraw((uint32_t)xy[-4]);                                         // (game thread: menus and bases too, for the ground cheats)
    if (g_cursorFix) DetectCursorOffset((uint32_t)xy[-4], xy[0], xy[1], view);   // also the nav map, opened during flight
    if (GetTickCount() - g_lastFlightTick < 500) return;            // only menus / films / PAD
    if (g_cursorId >= 0 && id == g_cursorId && (!g_cursorShapes || shapes == g_cursorShapes)) { xy[0] += g_cursorDx; xy[1] += g_cursorDy; }
    if (!g_spriteLog) return;
    static int lastX[1024], lastY[1024]; static DWORD lastLog[1024];
    int k = id & 1023;
    if ((xy[0] != lastX[k] || xy[1] != lastY[k]) && GetTickCount() - lastLog[k] > 400) {
        int cx, cy; CursorGamePos(&cx, &cy);
        const int32_t* sh = (const int32_t*)shapes; int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        if (sh) { const int32_t* e = (const int32_t*)((const uint8_t*)sh + sh[id * 2 + 2]); bx0 = e[2]; by0 = e[3]; bx1 = e[4]; by1 = e[5]; }
        Log("[sprite] id %d shapes %p at (%d,%d) bounds (%d,%d)-(%d,%d) | pointer (%d,%d) diff (%d,%d)", id, shapes, xy[0], xy[1], bx0, by0, bx1, by1, cx, cy, cx - xy[0], cy - xy[1]);
        lastLog[k] = GetTickCount();
    }
    lastX[k] = xy[0]; lastY[k] = xy[1];
}
static uintptr_t g_spr1Cont = 0x41305a + 6, g_spr2Cont = 0x4144e0 + 6, g_spr3Cont = 0x413e52 + 6, g_spr4Cont = 0x41368d + 6;
extern "C" void __cdecl SpriteDetect(int id, void* shapes, int* xy, int* view) { NotePostHudDraw((uint32_t)xy[-4]); if (g_cursorFix) DetectCursorOffset((uint32_t)xy[-4], xy[0], xy[1], view); }
__declspec(naked) static void HookSprite3() {
    __asm {
        pushad
        push dword ptr [esp + 36]
        lea eax, [esp + 52]
        push eax
        push dword ptr [esp + 48]
        push dword ptr [esp + 56]
        call SpriteDetect
        add esp, 16
        popad
        push ebp
        mov ebp, esp
        add esp, -0x50
        jmp dword ptr [g_spr3Cont]
    }
}
__declspec(naked) static void HookSprite4() {
    __asm {
        pushad
        push dword ptr [esp + 36]
        lea eax, [esp + 52]
        push eax
        push dword ptr [esp + 48]
        push dword ptr [esp + 56]
        call SpriteDetect
        add esp, 16
        popad
        push ebp
        mov ebp, esp
        add esp, -0x50
        jmp dword ptr [g_spr4Cont]
    }
}
__declspec(naked) static void HookSprite1() {
    __asm {
        pushad
        push dword ptr [esp + 36]    // view (args: view +4, shapes +8, id +12, x +16, y +20; +32 for pushad)
        lea eax, [esp + 52]          // &x
        push eax
        push dword ptr [esp + 48]    // shapes
        push dword ptr [esp + 56]    // id
        call SpriteFilter
        add esp, 16
        popad
        push ebp
        mov ebp, esp
        add esp, -0x50
        jmp dword ptr [g_spr1Cont]
    }
}
__declspec(naked) static void HookSprite2() {
    __asm {
        pushad
        push dword ptr [esp + 36]    // view
        lea eax, [esp + 52]          // &x
        push eax
        push dword ptr [esp + 48]    // shapes
        push dword ptr [esp + 56]    // id
        call SpriteFilter
        add esp, 16
        popad
        push ebp
        mov ebp, esp
        add esp, -0x50
        jmp dword ptr [g_spr2Cont]
    }
}

// ---------------------------------------------------------------- hooks
static LONGLONG g_passStart; static int g_passIdx = -1;

extern "C" void __cdecl PreBegin(void* world, uint8_t* cam, br_pixelmap* col, br_pixelmap* dep) {
    if (g_inEye2) return;
    int idx = (world == *(void**)ADDR_WORLD_BG) ? 0 : (world == *(void**)ADDR_WORLD_FG) ? 1 : -1;
    g_passIdx = idx; g_passStart = Now();
    if (idx == 0) {   // new game cycle: it started when p2den's spin-wait ended
        g_frameDone = false;
        g_cycleStart = (g_lastSpin && Ms(g_passStart - g_lastSpin) < 20.0) ? g_lastSpin : g_passStart;
    }
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
        Smooth_SetGameThread();
        if (g_newSpaceSession) { g_newSpaceSession = false; if (g_spacePrompt && Vr_Active()) WaitForSpacePrompt(); }   // mission start only
        UpdateControlMode();
        RunCheats();
        CheckHits();
        g_lastFlightTick = GetTickCount();
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
        if (g_actorLog) LogNewActors();
        {   // debug: a file named 'dumpnow' in the game folder triggers a dump (lets tools trigger it)
            static DWORD lastChk; DWORD nowt = GetTickCount();
            if (nowt - lastChk > 500) { lastChk = nowt; char f[MAX_PATH]; sprintf_s(f, "%s\\dumpnow", g_dir);
                if (GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(f); g_dumpRequested = true; } }
        }
        if (g_capDir[0] && GetTickCount() - g_capOpened > 2000) FinishCapture();   // (a view without the flight HUD)
        if (g_dumpRequested && !g_capDir[0]) BeginCapture(true);
        if (g_vrEnable && Vr_Active()) {
            if (g_dumpRequested) { g_dumpRequested = false; g_vrDumpNext = true; g_hudDumpNext = true; }
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
            FinishCapture();
        }
    }
    DWORD now = GetTickCount();
    if (now - g_lastReport > 5000 && g_frames) {
        br_pixelmap* c = (br_pixelmap*)g_lastCol;
        Log("[perf] %d frames | bg %.2f ms | fg %.2f ms | right-eye(bg+fg) %.2f ms (max %.2f) @ %ux%u | game buffer %ux%u",
            g_frames, g_accN[0] ? g_accMs[0] / g_accN[0] : 0, g_accN[1] ? g_accMs[1] / g_accN[1] : 0,
            g_accEyeN ? g_accEyeMs / g_accEyeN : 0, g_maxEyeMs, g_pmC.width, g_pmC.height, c ? c->width : 0, c ? c->height : 0);
        g_maxEyeMs = 0;
        {   // game clock and player-ship position: shows whether game speed depends on the cycle rate
            uint8_t* game = g_game; float pos[3] = {0, 0, 0};
            if (game) { uint8_t* ship = *(uint8_t**)(game + GAME_SHIPS + *(int*)(game + GAME_SHIPIDX) * SHIP_STRIDE);
                        if (ship) memcpy(pos, ship + ACTOR_T_MAT + 36, 12); }
            Log("[clock] tick %u game_timer %d ship %.2f %.2f %.2f", GetTickCount(), *(volatile int*)0x4f0248, pos[0], pos[1], pos[2]);
            Smooth_Report((now - g_lastReport) / 1000.0);
        }
        if (g_vrN) { VrEyeTarget t = {}; Vr_GetEyeTarget(&t);
            Log("[perf] VR both eyes %.2f ms avg (max %.2f) @ %dx%d per eye", g_vrAccMs / g_vrN, g_vrMaxMs, t.width, t.height);
            g_vrAccMs = 0; g_vrN = 0; g_vrMaxMs = 0; }
        {   // proximity diagnostics: how close do objects get to the player's ship?
            uint8_t* game = g_game; float nearest = -1; uint8_t* nearA = nullptr;
            if (game) { uint8_t* ship = *(uint8_t**)(game + GAME_SHIPS + *(int*)(game + GAME_SHIPIDX) * SHIP_STRIDE);
                if (ship) { const float* sm = (const float*)(ship + ACTOR_T_MAT);
                    for (auto& c : g_snapCur) {
                        if (c.a == ship || c.a[0x12] != 1 || IsBoltModel(*(void**)(c.a + 0x18))) continue;
                        if (*(uint8_t**)(c.a + 0xC) != *(uint8_t**)ADDR_WORLD_FG) continue;    // direct children of the fg world
                        float d[3] = {c.m[9] - sm[9], c.m[10] - sm[10], c.m[11] - sm[11]};
                        float dist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                        if (nearest < 0 || dist < nearest) { nearest = dist; nearA = c.a; } }
                    static bool dumpedShip; if (!dumpedShip && *(void**)(ship + 0x18)) { dumpedShip = true;
                        const float* f = (const float*)*(uint8_t**)(ship + 0x18);
                        Log("[proximity] player ship model %p floats[8..23]: %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g", (void*)f,
                            f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15], f[16], f[17], f[18], f[19], f[20], f[21], f[22], f[23]); } } }
            if (nearest > 0 && (g_minCentre < 0 || nearest < g_minCentre)) g_minCentre = nearest;
            if (nearA && *(void**)(nearA + 0x18)) {
                static void* lastModel; void* mdl = *(void**)(nearA + 0x18);
                if (mdl != lastModel) { lastModel = mdl; const float* f = (const float*)mdl;
                    Log("[proximity] nearest object model %p floats[8..23]: %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g", mdl,
                        f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15], f[16], f[17], f[18], f[19], f[20], f[21], f[22], f[23]); } }
            Log("[proximity] closest surface in HUD view %.1f units (5s min %.1f, session min %.1f) | nearest object centre %.1f (session min %.1f) | units/m %.0f",
                g_lastBoreZ, g_minSurf5, g_minSurf, nearest, g_minCentre, g_worldScale);
            g_minSurf5 = -1;
        }
        if (g_smooth) { double sec = (now - g_lastReport) / 1000.0;
            Log("[smooth] mode %d: %.1f extra frames/s, %.1f skipped/s (deadline), game period %.2f ms, render %.2f ms",
                g_smooth, g_idleRenders / sec, g_idleSkips / sec, g_period, g_renderCostMs);
            g_idleRenders = g_idleSkips = 0; }
        g_frames = 0; g_accMs[0] = g_accMs[1] = g_accEyeMs = 0; g_accN[0] = g_accN[1] = g_accEyeN = 0;
        g_lastReport = now;
    }
}

static uintptr_t g_beginCont = ADDR_BEGIN + 6;
static uintptr_t g_starsCont = ADDR_STARS_FN + 6;
static uintptr_t g_markersFn = ADDR_MARKERS_FN;
static uintptr_t g_hudFn = ADDR_HUD_FN;

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

static uintptr_t g_spriteFn = ADDR_SPRITE_FN;
__declspec(naked) static void HookPipCall() {    // cdecl (view, shapes, id, x, y): record, then tail-jump to the blit
    __asm {
        pushad
        push dword ptr [esp + 52]   // y
        push dword ptr [esp + 52]   // x
        push dword ptr [esp + 52]   // id
        call RecordSprite
        add esp, 12
        popad
        jmp dword ptr [g_spriteFn]
    }
}

__declspec(naked) static void HookMarkersCall() {
    __asm {
        pushad
        call PreMarkers
        popad
        call dword ptr [g_markersFn]
        pushad
        call PostMarkers
        popad
        ret
    }
}

__declspec(naked) static void HookHudCall() {
    __asm {
        call dword ptr [g_hudFn]
        pushad
        call PostHud
        popad
        ret
    }
}

static uintptr_t g_joyNormFn = ADDR_JOYNORM_FN;
static uint8_t g_fpuSave[108];
__declspec(naked) static void HookJoyNormCall() {   // eax = game: run the game's normaliser, then apply the VR stick
    __asm {
        call dword ptr [g_joyNormFn]
        pushad
        fnsave g_fpuSave
        call ApplyStick
        frstor g_fpuSave
        popad
        ret
    }
}
__declspec(naked) static void HookRollWrite() {     // replaces "cmp byte ptr [0x52e063],0" at 0x4503f6 (ebp = game): VR axes + speed
    __asm {
        cmp byte ptr [g_rollActive], 0
        je done
        push eax
        push edx
        mov eax, dword ptr [ebp + 0x2785c]
        imul eax, eax, 0x3a0
        cmp byte ptr [g_rollOn], 0
        je noroll
        mov edx, dword ptr [g_rollValue]
        mov dword ptr [eax + ebp + 0x12ac], edx
    noroll:
        cmp byte ptr [g_pitchOn], 0
        je nopitch
        mov edx, dword ptr [g_pitchValue]
        mov dword ptr [eax + ebp + 0x12a8], edx
    nopitch:
        cmp byte ptr [g_yawOn], 0
        je noyaw
        mov edx, dword ptr [g_yawValue]
        mov dword ptr [eax + ebp + 0x12b0], edx
    noyaw:
        cmp byte ptr [g_speedActive], 0
        je nospeed
        mov edx, dword ptr [g_speedValue]
        mov dword ptr [eax + ebp + 0x1298], edx
    nospeed:
        pop edx
        pop eax
    done:
        cmp byte ptr ds:[0x52e063], 0
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

// BrZbSceneRenderBegin rejects pixelmaps over 2048 (0x46d5a3: cmp w,0x800; ja fail; cmp h,0x800; jbe ok).
static bool LiftSizeLimit() {
    uint8_t* ja = (uint8_t*)0x46d5a9; uint8_t* jbe = (uint8_t*)0x46d5b1;
    if (!(ja[0] == 0x77 && ja[1] == 0x08 && jbe[0] == 0x76 && jbe[1] == 0x0d)) return false;
    DWORD old; VirtualProtect(ja, 16, PAGE_EXECUTE_READWRITE, &old);
    ja[0] = 0x90; ja[1] = 0x90;     // nop the 'width too big' branch
    jbe[0] = 0xEB;                  // height check -> unconditional jump to ok
    VirtualProtect(ja, 16, old, &old); FlushInstructionCache(GetCurrentProcess(), ja, 16);
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
    bool e = false;
    if (*(uint8_t*)ADDR_HUD_CALL == 0xE8 && ADDR_HUD_CALL + 5 + *(int32_t*)(ADDR_HUD_CALL + 1) == ADDR_HUD_FN) {
        DWORD old; VirtualProtect((void*)ADDR_HUD_CALL, 5, PAGE_EXECUTE_READWRITE, &old);
        *(int32_t*)(ADDR_HUD_CALL + 1) = (int32_t)((uintptr_t)HookHudCall - (ADDR_HUD_CALL + 5));
        VirtualProtect((void*)ADDR_HUD_CALL, 5, old, &old); e = true;
    }
    bool f = false;
    if (*(uint8_t*)ADDR_PIP_CALL == 0xE8 && ADDR_PIP_CALL + 5 + *(int32_t*)(ADDR_PIP_CALL + 1) == ADDR_SPRITE_FN) {
        DWORD old; VirtualProtect((void*)ADDR_PIP_CALL, 5, PAGE_EXECUTE_READWRITE, &old);
        *(int32_t*)(ADDR_PIP_CALL + 1) = (int32_t)((uintptr_t)HookPipCall - (ADDR_PIP_CALL + 5));
        VirtualProtect((void*)ADDR_PIP_CALL, 5, old, &old); f = true;
    }
    Log("hooks: begin=%d end=%d stars=%d markers=%d hud=%d pip=%d", a, b, c, d, e, f);
    bool g1 = false, g2 = false;
    {   const uint8_t* q = (const uint8_t*)ADDR_JOYNORM_CALL;
        Log("[ctl] joystick call site bytes: %02x %02x %02x %02x %02x", q[0], q[1], q[2], q[3], q[4]); }
    // The joystick normaliser call (p2den redirects it to its own joystick code) is left alone: hooking it (an extra
    // return address on the stack) stopped the joystick reaching the flight controls. The VR axes are written later,
    // at 0x4503f6 (HookRollWrite).
    if (*(uint8_t*)ADDR_JOYNORM_CALL == 0xE8) {
        g_joyNormFn = ADDR_JOYNORM_CALL + 5 + *(int32_t*)(ADDR_JOYNORM_CALL + 1);
        Log("[ctl] joystick normaliser call target %08x%s (not hooked)", (unsigned)g_joyNormFn, g_joyNormFn == ADDR_JOYNORM_FN ? " (game)" : " (p2den)");
    }
    static const uint8_t rollBytes[7] = {0x80, 0x3D, 0x63, 0xE0, 0x52, 0x00, 0x00};
    if (!memcmp((void*)ADDR_ROLL_PATCH, rollBytes, 7)) {
        DWORD old; VirtualProtect((void*)ADDR_ROLL_PATCH, 7, PAGE_EXECUTE_READWRITE, &old);
        uint8_t* q = (uint8_t*)ADDR_ROLL_PATCH; q[0] = 0xE8; *(int32_t*)(q + 1) = (int32_t)((uintptr_t)HookRollWrite - (ADDR_ROLL_PATCH + 5));
        q[5] = 0x90; q[6] = 0x90;
        VirtualProtect((void*)ADDR_ROLL_PATCH, 7, old, &old); FlushInstructionCache(GetCurrentProcess(), q, 7); g2 = true;
    }
    Log("hooks: controls joystick=%d roll=%d", g1, g2);
    static const uint8_t sprPro[6] = {0x55, 0x8B, 0xEC, 0x83, 0xC4, 0xB0};   // push ebp; mov ebp,esp; add esp,-0x50
    bool h1 = WriteJmp(0x41305a, (void*)HookSprite1, sprPro, 6), h2 = WriteJmp(0x4144e0, (void*)HookSprite2, sprPro, 6);
    bool h3 = WriteJmp(0x413e52, (void*)HookSprite3, sprPro, 6), h4 = WriteJmp(0x41368d, (void*)HookSprite4, sprPro, 6);
    Log("hooks: sprites %d %d %d %d", h1, h2, h3, h4);
    static const uint8_t linePro[7] = {0x56, 0x57, 0x55, 0x8B, 0x74, 0x24, 0x20};   // push esi; push edi; push ebp; mov esi,[esp+0x20]
    Log("hooks: marker lines %d", WriteJmp(0x4661a0, (void*)HookLine, linePro, 7));
    {   int ok = 0;   // space session entry: redirect both call sites through SpaceSessionThunk
        for (uintptr_t site : {(uintptr_t)0x41c0e2, (uintptr_t)0x4227bb}) {
            uint8_t* q = (uint8_t*)site;
            if (q[0] != 0xE8) continue;
            uintptr_t cur = site + 5 + *(int32_t*)(q + 1);           // p2den already wraps the session: chain to its wrapper
            if (cur == (uintptr_t)SpaceSessionThunk) continue;
            if (cur != ADDR_SPACE_SESSION) Log("hooks: space session call %08x already redirected to %08x (chained)", (unsigned)site, (unsigned)cur);
            g_spaceSessionFn = cur;
            DWORD old; VirtualProtect(q, 5, PAGE_EXECUTE_READWRITE, &old);
            *(int32_t*)(q + 1) = (int32_t)((uintptr_t)SpaceSessionThunk - (site + 5));
            VirtualProtect(q, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), q, 5); ok++;
        }
        Log("hooks: space session entry %d/2", ok); }
    for (uintptr_t slot : {(uintptr_t)0x570194, (uintptr_t)0x570198}) {   // joyGetNumDevs / joyGetPosEx import slots
        void* fn = *(void**)slot; HMODULE m = nullptr; char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fn, &m))
            GetModuleFileNameA(m, name, MAX_PATH);
        Log("[input] import slot %08x -> %p (%s)", (unsigned)slot, fn, name);
    }
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
    if (reason == DLL_PROCESS_DETACH) { Macro_Shutdown(); return TRUE; }   // e.g. a recording ended by quitting the game
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
        g_actorLog = GetPrivateProfileIntA("VR", "ACTOR_LOG", 0, p);
        GetPrivateProfileStringA("VR", "LASER_WIDEN", "3.0", b, 32, p); g_laserWiden = (float)atof(b);
        g_smooth = GetPrivateProfileIntA("VR", "SMOOTH", 1, p);
        g_ctlEnable = GetPrivateProfileIntA("VR", "VR_CONTROLS", 1, p) != 0;
        g_spriteLog = GetPrivateProfileIntA("VR", "SPRITE_LOG", 0, p) != 0;
        g_cursorFix = GetPrivateProfileIntA("VR", "CURSOR_FIX", 1, p) != 0;
        g_cursorLog = GetPrivateProfileIntA("VR", "CURSOR_LOG", 0, p) != 0;
        g_rumble = GetPrivateProfileIntA("VR", "RUMBLE", 1, p) != 0;
        g_joyPass = GetPrivateProfileIntA("VR", "JOYSTICK_PASSTHROUGH", 1, p) != 0;
        g_cheats = GetPrivateProfileIntA("VR", "CHEATS", 1, p) != 0;
        for (int k = 0; k < CHEAT_COUNT; k++) { char key[48]; sprintf_s(key, "CHEAT_%s_BUTTON", kCheatName[k]); g_cheatButton[k] = GetPrivateProfileIntA("VR", key, 0, p); }
        g_spacePrompt = GetPrivateProfileIntA("VR", "SPACE_PROMPT", 1, p) != 0;
        g_hudArrowLayer = GetPrivateProfileIntA("VR", "HUD_ARROW_LAYER", 1, p) != 0;
        GetPrivateProfileStringA("VR", "RUMBLE_GUNS", "0.35", b, 32, p); g_rumbleGuns = (float)atof(b);
        GetPrivateProfileStringA("VR", "RUMBLE_HIT", "0.9", b, 32, p); g_rumbleHit = (float)atof(b);
        g_cursorId = GetPrivateProfileIntA("VR", "PAD_CURSOR_ID", -1, p);
        g_cursorDx = GetPrivateProfileIntA("VR", "PAD_CURSOR_DX", 0, p); g_cursorDy = GetPrivateProfileIntA("VR", "PAD_CURSOR_DY", 0, p);
        g_invertPitch = GetPrivateProfileIntA("VR", "INVERT_PITCH", 0, p) != 0;
        g_invertRoll = GetPrivateProfileIntA("VR", "INVERT_ROLL", 0, p) != 0;
        GetPrivateProfileStringA("VR", "ROLL_GAIN", "2.5", b, 32, p); g_rollGain = (float)atof(b);
        GetPrivateProfileStringA("VR", "THROTTLE_RATE", "0.6", b, 32, p); g_throttleRate = (float)atof(b);
        for (int i = 0; i < kVrCommandCount; i++) {   // KEY_<COMMAND>=scancode (decimal) overrides the game key
            char k[64] = "KEY_"; for (int j = 0; kVrCommands[i].action[j] && j < 58; j++) k[4 + j] = (char)toupper(kVrCommands[i].action[j]);
            g_keyFor[i] = (uint8_t)GetPrivateProfileIntA("VR", k, kVrCommands[i].scancode, p); g_altFor[i] = kVrCommands[i].alt;
        }
        g_retDynamic = GetPrivateProfileIntA("VR", "RETICLE_DYNAMIC", 0, p) != 0;
        GetPrivateProfileStringA("VR", "OBSERVER_BACK", "1.0", b, 32, p); g_observerBack = (float)atof(b);
        g_showOwnShip = GetPrivateProfileIntA("VR", "SHOW_OWN_SHIP", 0, p) != 0;
        g_retInEyes = GetPrivateProfileIntA("VR", "RETICLE_IN_EYES", 1, p) != 0;
        if (GetPrivateProfileIntA("VR", "RETICLE_GLASS", 1, p)) g_retInEyes = false;   // crosshair is on the HUD glass instead
        GetPrivateProfileStringA("VR", "RETICLE_SIZE", "0.20", b, 32, p); g_retFracP2 = (float)atof(b);
        GetPrivateProfileStringA("VR", "LASER_WIDEN_DIST", "60", b, 32, p); g_laserWidenDist = (float)atof(b);
        g_flare = GetPrivateProfileIntA("VR", "LENS_FLARE", 1, p) != 0;
        GetPrivateProfileStringA("VR", "DUST_SCALE", "1.0", b, 32, p); g_dustScale = (float)atof(b);
        GetPrivateProfileStringA("VR", "WORLD_SCALE", "10", b, 32, p); g_worldScale = (float)atof(b);
        QueryPerformanceFrequency(&g_qpf);
        Log("p2vr %s loaded: mode=%d ipd=%.3f sweep=%.1f fovscale=%.2f eye=%dx%d", P2VR_VERSION, g_mode, g_ipd, g_sweepDeg, g_fovScale, g_eyeW, g_eyeH);
        InstallHooks();
        if (GetPrivateProfileIntA("VR", "EYE_MAX", 2048, p) > 2048) Log("BRender 2048 size check lifted: %d", LiftSizeLimit());
        if (g_vrEnable) { Capture_Install(Log); Capture_SetOverlayCallback(DrawCheatOverlay); Vr_Start(p, Log); Vr_SetObserverBack(g_observerBack); }
        Smooth_Install(Log);
        Smooth_SetIdleCallback(IdleTick);
        // ground cheats, ground captures (Ctrl+F9) and the test trigger: own thread, every phase (menus, bases and films do not
        // go through the flight routines or p2den's frame pacing on the game thread)
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD { for (;;) { Sleep(20); GroundCheats(); } }, nullptr, 0, nullptr);
        Macro_Start(g_dir, GetPrivateProfileIntA("VR", "AUTOPLAY", 0, p) != 0, Log);
    }
    return TRUE;
}
