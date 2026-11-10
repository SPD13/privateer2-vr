// vr.cpp - OpenXR (SteamVR, 32-bit runtime) presenter with its own D3D11 device and thread.
//
// Layers submitted each display frame:
//   flight: projection layer (8-bit eye images -> palette shader) + HUD quad (p2den output, black keyed)
//   other : opaque virtual-screen quad (p2den output: menus, base, booth, FMV)
// Eye images are produced at game rate (30 Hz) and re-submitted with the poses they were rendered
// with, so SteamVR reprojects them at display rate.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include "vr.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "openxr_loader.lib")

static LogFn Log;
#define XRCHECK(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { Log("[vr] %s failed: %d (line %d)", #x, (int)r_, __LINE__); return false; } } while (0)

// ---------------------------------------------------------------- settings (p2vr.ini [VR])
// ---------------------------------------------------------------- controller actions
static XrActionSet s_actSet; static XrAction s_actStickR, s_actStickL, s_actRecenter, s_actBtn[kVrCommandCount];
// laser pointer for menus / films (outside flight): aim pose, left and right click
static XrAction s_actAim, s_actClick, s_actRClick, s_actEsc; static XrSpace s_aimSpace;
static bool s_frameExt;   // XR_VALVE_frame_controller_interaction enabled
static XrAction s_actHaptic[2];                                     // vibration output, left / right hand
static float s_hapAmp[2], s_hapDur[2]; static bool s_hapPending[2]; // requests from the game thread (under s_ctlCs)
static int s_maxLayers = 16;
static bool s_maximize = true;   // WINDOW_MAXIMIZE: p2den renders at the window size, so a bigger window = sharper films/menus/HUD                                        // runtime's composition layer limit
// HUD panels: rectangles of the 640x480 HUD shown as separate quads placed around the player (HUD_PANELS=1)
struct HudPanel { char name[16]; float r[4]; float pos[3]; float yaw, pitch, width; };
static HudPanel s_panels[8]; static int s_nPanels; static bool s_panelsOn;
static bool s_actionsReady; static CRITICAL_SECTION s_ctlCs; static VrControls s_ctl;
static float s_deadzone = 0.15f;
static float s_eyeScale = 1.0f;      // multiplier on the runtime's recommended size
static int   s_eyeMax = 2048;        // largest eye dimension (BRender's own check is 2048; p2vr patches it out above that)
static float s_offQuat[2][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}};   // per-eye rotation to the centre of its (asymmetric) fov
static float s_hudDist = 1.0f, s_hudWidth = 1.5f, s_hudY = 0.0f;
// Crosshair region of the HUD, shown as its own layer far out on the boresight so it lines up with the lasers.
static float s_retDist = 30.0f;      // metres (game units = metres * WORLD_SCALE): maximum crosshair distance
static float s_retMin = 1.5f;        // closest the crosshair/marker plane may come
static volatile float s_retWanted = 0;   // from the game thread: distance of what is under the crosshair
static float s_retNow = 30.0f;       // smoothed distance actually used
static float s_retFrac = 0.20f;      // size of the crosshair region as a fraction of the HUD width
static const int HUD_PX_W = 1600, HUD_PX_H = 1200;
static int s_retPx = 256;            // crosshair crop size in swapchain pixels
// Game camera: focal length 461.0357 px on a 640x480 image -> tan of the half angles
static const float GAME_TAN_H = 320.0f / 461.0357f, GAME_TAN_V = 240.0f / 461.0357f;
static float s_screenDist = 2.5f, s_screenWidth = 3.2f;
static float s_keyGain = 40.0f;      // HUD black-key edge steepness
static float s_keyThresh = 0.03f;    // gamma brightness below which HUD pixels are transparent

// ---------------------------------------------------------------- shared state
static CRITICAL_SECTION s_cs;
static volatile LONG s_active;
static VrView   s_views[2];
static bool     s_viewsValid;
static VrEyeTarget s_target;
static bool     s_targetValid;
static LUID     s_luid;
static volatile LONG s_recenter;
static volatile LONG s_frameCounter;

// eye buffers: [set][eye]; game writes s_back, VR thread uploads s_ready
struct EyeCh { bool on; float rows[9]; int bx0, by0, bx1, by1; float g[4]; std::vector<uint8_t> occ; };
static const int MAX_BLOBS = 10;
struct EyeSet { std::vector<uint8_t> px[2]; VrView views[2]; uint8_t pal[1024]; EyeCh ch[2];
                bool mHave; uint32_t mSerial; float mPos[MAX_BLOBS][3], mSize[MAX_BLOBS][2]; uint8_t mValid[MAX_BLOBS]; };
static EyeSet   s_sets[3];
static int      s_back = 0, s_ready = -1, s_inUse = -1;
static DWORD    s_lastEyeTick;

// HUD markers (8-bit) from the game thread
static std::vector<uint8_t> s_mk; static int s_mkW, s_mkH; static uint8_t s_mkPal[1024]; static bool s_mkNew;
static VrMarkerBlob s_blobs[MAX_BLOBS], s_shownBlobs[MAX_BLOBS]; static int s_nBlobs, s_nShownBlobs;
static float s_observerBack = 0;
static bool  s_retInEyes = true;
static bool  s_retGlass = true; static float s_glassAlpha = 0.7f;   // crosshair on the HUD glass, semi-transparent     // crosshair/lead indicator are drawn into the eye images (no quad layers)
static uint32_t s_mkSerial, s_shownSerial; static DWORD s_mkSubmitTick;                       // marker image serials (submitted / on screen)
// exact poses: for the marker set on screen, and for a newly submitted (pending) set
static float s_mPos[MAX_BLOBS][3], s_mSize[MAX_BLOBS][2]; static uint8_t s_mValid[MAX_BLOBS];
static float s_pPos[MAX_BLOBS][3], s_pSize[MAX_BLOBS][2]; static uint8_t s_pValid[MAX_BLOBS]; static bool s_pHave;
static HANDLE   s_screenHandle; static int s_screenW, s_screenH; static uint32_t s_screenFrame;

// ---------------------------------------------------------------- public API (game thread side)
bool Vr_Active() { return s_active != 0; }
void Vr_Haptic(int hand, float amp, float sec) {
    if (!s_actionsReady) return;
    EnterCriticalSection(&s_ctlCs);
    for (int h = 0; h < 2; h++) if (hand == 2 || hand == h) {
        if (!s_hapPending[h] || amp > s_hapAmp[h]) s_hapAmp[h] = amp;
        if (!s_hapPending[h] || sec > s_hapDur[h]) s_hapDur[h] = sec;
        s_hapPending[h] = true;
    }
    LeaveCriticalSection(&s_ctlCs);
}
bool Vr_GetControls(VrControls* c) {
    EnterCriticalSection(&s_ctlCs); *c = s_ctl; LeaveCriticalSection(&s_ctlCs);
    return c->active;
}
LUID Vr_AdapterLuid() { return s_luid; }
void Vr_RequestRecenter() { InterlockedExchange(&s_recenter, 1); }
void Vr_SetReticleDistance(float m) { s_retWanted = m; }
LONG Vr_FrameCounter() { return s_frameCounter; }

bool Vr_GetViews(VrView v[2]) {
    EnterCriticalSection(&s_cs);
    bool ok = s_viewsValid; if (ok) { v[0] = s_views[0]; v[1] = s_views[1]; }
    LeaveCriticalSection(&s_cs); return ok;
}
bool Vr_GetEyeTarget(VrEyeTarget* t) {
    EnterCriticalSection(&s_cs);
    bool ok = s_targetValid; if (ok) *t = s_target;
    LeaveCriticalSection(&s_cs); return ok;
}
uint8_t* Vr_EyeBuffer(int eye) {
    EyeSet& s = s_sets[s_back];
    size_t n = (size_t)s_target.width * s_target.height;
    if (s.px[eye].size() != n) s.px[eye].resize(n);
    return s.px[eye].data();
}
void Vr_SetEyeCrosshair(int e, const float rows[9], int bx0, int by0, int bx1, int by1, const uint8_t* occ,
                        float gx0, float gy0, float gx1, float gy1) {
    EyeCh& c = s_sets[s_back].ch[e];
    c.on = occ != nullptr && bx1 >= bx0 && by1 >= by0;
    if (!c.on) return;
    memcpy(c.rows, rows, sizeof c.rows); c.bx0 = bx0; c.by0 = by0; c.bx1 = bx1; c.by1 = by1;
    c.g[0] = gx0; c.g[1] = gy0; c.g[2] = gx1; c.g[3] = gy1;
    c.occ.assign(occ, occ + (size_t)(bx1 - bx0 + 1) * (by1 - by0 + 1));
}
void Vr_SubmitEyes(const VrView used[2], const uint8_t pal[1024]) {
    EnterCriticalSection(&s_cs);
    EyeSet& s = s_sets[s_back];
    s.views[0] = used[0]; s.views[1] = used[1]; memcpy(s.pal, pal, 1024);
    s_ready = s_back;
    for (int i = 0; i < 3; i++) if (i != s_ready && i != s_inUse) { s_back = i; break; }
    s_lastEyeTick = GetTickCount();
    LeaveCriticalSection(&s_cs);
}
void Vr_SetObserverBack(float m) { s_observerBack = m; }
void Vr_SetMarkerPoses(uint32_t serial, const float (*pos)[3], const float (*size)[2], const uint8_t* valid, int n, bool withEyes) {
    EnterCriticalSection(&s_cs);
    if (n > MAX_BLOBS) n = MAX_BLOBS;
    if (withEyes) {                           // travels with the eye images rendered at the same moment
        EyeSet& es = s_sets[s_back];
        es.mHave = true; es.mSerial = serial;
        memcpy(es.mPos, pos, sizeof(float) * 3 * n); memcpy(es.mSize, size, sizeof(float) * 2 * n);
        memset(es.mValid, 0, sizeof es.mValid); memcpy(es.mValid, valid, n);
    }
    if (serial == s_mkSerial && s_mkNew) {    // pending set: keep until it is adopted
        memcpy(s_pPos, pos, sizeof(float) * 3 * n); memcpy(s_pSize, size, sizeof(float) * 2 * n);
        memset(s_pValid, 0, sizeof s_pValid); memcpy(s_pValid, valid, n); s_pHave = true;
    }
    LeaveCriticalSection(&s_cs);
}
uint32_t Vr_SubmitMarkers(const uint8_t* idx, int w, int h, const uint8_t pal[1024], const VrMarkerBlob* blobs, int n) {
    EnterCriticalSection(&s_cs);
    uint32_t serial = ++s_mkSerial; s_mkSubmitTick = GetTickCount(); s_pHave = false;
    s_mk.assign(idx, idx + (size_t)w * h); s_mkW = w; s_mkH = h; memcpy(s_mkPal, pal, 1024); s_mkNew = true;
    s_nBlobs = n < MAX_BLOBS ? n : MAX_BLOBS; memcpy(s_blobs, blobs, sizeof(VrMarkerBlob) * s_nBlobs);
    LeaveCriticalSection(&s_cs);
    return serial;
}
void Vr_SetScreenSource(HANDLE h, int w, int hgt, uint32_t frame) {
    EnterCriticalSection(&s_cs);
    s_screenHandle = h; s_screenW = w; s_screenH = hgt; s_screenFrame = frame;
    LeaveCriticalSection(&s_cs);
}

// ---------------------------------------------------------------- D3D11
static ID3D11Device* d3d; static ID3D11DeviceContext* ctx;
static ID3D11VertexShader* vsFull; static ID3D11PixelShader* psIndex; static ID3D11PixelShader* psScreen; static ID3D11PixelShader* psMarker;
static ID3D11Texture2D* mkTex; static ID3D11ShaderResourceView* mkSrv; static int mkTexW, mkTexH;
static ID3D11Texture2D* mkPalTex; static ID3D11ShaderResourceView* mkPalSrv;
static ID3D11Texture2D* idxTex[2]; static ID3D11ShaderResourceView* idxSrv[2];
static ID3D11Texture2D* palTex; static ID3D11ShaderResourceView* palSrv;
static ID3D11SamplerState* linSamp; static ID3D11Buffer* cbuf; static ID3D11Buffer* cbufEye[2]; static ID3D11Buffer* cbufPanels;
static ID3D11Texture2D* occTex[2]; static ID3D11ShaderResourceView* occSrv[2]; static int occW[2], occH[2];
static ID3D11Texture2D* scrTex; static ID3D11ShaderResourceView* scrSrv; static HANDLE scrOpened;
static float s_uScale = 1.0f, s_uOff = 0.0f;   // crop of the (pillarboxed) p2den image to its 4:3 centre

static const char* kShaders = R"(
Texture2D<uint>   idx : register(t0);
Texture2D<float4> pal : register(t1);
Texture2D<float4> scr : register(t2);
SamplerState      ls  : register(s0);
cbuffer C : register(b0) { float keyGain; float keyOn; float uScale; float uOff; float keyThresh; float holeOn; float holeAlpha; float pad2;
                           float4 map; float4 hole; };   // map: hud uv = map.xy + uv * map.zw ; hole: u0 v0 u1 v1
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o; o.uv = float2((id << 1) & 2, id & 2); o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
Texture2D<uint>   occ : register(t3);
cbuffer P : register(b2) { float4 prect[8]; float4 pinfo; };   // HUD panels cut out of the main HUD (uv rects), pinfo.x = count
cbuffer E : register(b1) { float4 r0; float4 r1; float4 r2; float4 eye; float4 box; float4 grect; float4 crop; float4 flags; };
float4 psIndex(V i) : SV_Target {
    uint c = idx.Load(int3(i.p.xy, 0));
    float3 base = pal.Load(int3(c, 0, 0)).rgb;
    // crosshair at infinity from p2den's displayed HUD (exact look), hidden where a ship covers it
    if (flags.x > 0.5 && i.p.x >= box.x && i.p.x < box.z + 1 && i.p.y >= box.y && i.p.y < box.w + 1) {
        if (occ.Load(int3(int2(i.p.xy) - int2(box.xy), 0)) == 0) {
            float ex = (i.p.x - eye.x * 0.5) / eye.z, ey = -(i.p.y - eye.y * 0.5) / eye.w;
            float3 d = r0.xyz * ex + r1.xyz * ey - r2.xyz;          // eye direction -> ship space
            if (d.z < -1e-4) {
                float gx = 320.0 + d.x / -d.z * 461.0357, gy = 240.0 - d.y / -d.z * 461.0357;
                if (gx >= grect.x && gx < grect.z && gy >= grect.y && gy < grect.w) {
                    float3 h = scr.Sample(ls, float2(crop.y + gx / 640.0 * crop.x, gy / 480.0)).rgb;
                    float g = pow(max(h.r, max(h.g, h.b)), 1.0 / 2.2);
                    float a = saturate((g - crop.z) * crop.w);
                    base = lerp(base, h, a);
                }
            }
        }
    }
    return float4(base, 1);
}
float4 psMarker(V i) : SV_Target {   // map.xy = index texture size; index 0 = transparent
    uint c = idx.Load(int3(min(int2(i.uv * map.xy), int2(map.xy) - 1), 0));
    if (c == 0) return 0;
    return float4(pal.Load(int3(c, 0, 0)).rgb, 1);
}
float4 psScreen(V i) : SV_Target {
    float2 uv = map.xy + i.uv * map.zw;
    if (keyOn > 0.5) for (int k = 0; k < (int)pinfo.x; k++)
        if (uv.x >= prect[k].x && uv.x < prect[k].z && uv.y >= prect[k].y && uv.y < prect[k].w) return 0;
    bool inHole = holeOn > 0.5 && uv.x > hole.x && uv.x < hole.z && uv.y > hole.y && uv.y < hole.w;
    if (inHole && holeAlpha <= 0.0) return 0;
    float3 c = scr.Sample(ls, float2(uOff + uv.x * uScale, uv.y)).rgb;
    // key on display (gamma) brightness: anything clearly above black is fully opaque
    float g = pow(max(c.r, max(c.g, c.b)), 1.0 / 2.2);
    float a = keyOn > 0.5 ? saturate((g - keyThresh) * keyGain) : 1.0;
    if (inHole) { c *= holeAlpha; a *= holeAlpha; }   // crosshair on the glass: semi-transparent (premultiplied)
    return float4(c, a);   // premultiplied: black stays black
}
)";

static bool CompileShaders() {
    ID3DBlob *b = nullptr, *err = nullptr;
    auto comp = [&](const char* entry, const char* prof) -> ID3DBlob* {
        ID3DBlob* out = nullptr;
        if (FAILED(D3DCompile(kShaders, strlen(kShaders), "p2vr", nullptr, nullptr, entry, prof, 0, 0, &out, &err))) {
            Log("[vr] shader %s: %s", entry, err ? (char*)err->GetBufferPointer() : "?"); return nullptr;
        }
        return out;
    };
    if (!(b = comp("vs", "vs_5_0"))) return false;
    d3d->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vsFull); b->Release();
    if (!(b = comp("psIndex", "ps_5_0"))) return false;
    d3d->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psIndex); b->Release();
    if (!(b = comp("psMarker", "ps_5_0"))) return false;
    d3d->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psMarker); b->Release();
    if (!(b = comp("psScreen", "ps_5_0"))) return false;
    d3d->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psScreen); b->Release();
    D3D11_SAMPLER_DESC sd = {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    d3d->CreateSamplerState(&sd, &linSamp);
    D3D11_BUFFER_DESC bd = {64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    d3d->CreateBuffer(&bd, nullptr, &cbuf);
    D3D11_BUFFER_DESC be = {128, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    d3d->CreateBuffer(&be, nullptr, &cbufEye[0]); d3d->CreateBuffer(&be, nullptr, &cbufEye[1]);
    D3D11_BUFFER_DESC bp = {144, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    d3d->CreateBuffer(&bp, nullptr, &cbufPanels);
    D3D11_TEXTURE2D_DESC td = {256, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    d3d->CreateTexture2D(&td, nullptr, &palTex); d3d->CreateShaderResourceView(palTex, nullptr, &palSrv);
    d3d->CreateTexture2D(&td, nullptr, &mkPalTex); d3d->CreateShaderResourceView(mkPalTex, nullptr, &mkPalSrv);
    return true;
}

static bool CreateDeviceForLuid(LUID luid) {
    IDXGIFactory1* f = nullptr; if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return false;
    IDXGIAdapter1* ad = nullptr; IDXGIAdapter1* chosen = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
            chosen = ad; char n[128]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, n, 128, 0, 0);
            Log("[vr] HMD adapter: %s", n); break;
        }
        ad->Release();
    }
    f->Release();
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                   D3D11_SDK_VERSION, &d3d, nullptr, &ctx);
    if (chosen) chosen->Release();
    if (FAILED(hr)) { Log("[vr] D3D11CreateDevice failed %08x", hr); return false; }
    return CompileShaders();
}

// ---------------------------------------------------------------- OpenXR
static XrInstance inst; static XrSystemId sysId; static XrSession sess; static XrSpace localSpace, viewSpace;
static XrSessionState sessState = XR_SESSION_STATE_UNKNOWN; static bool sessRunning;
struct Swap { XrSwapchain sc; int w, h; std::vector<ID3D11RenderTargetView*> rtv; };
static Swap eyeSc[2], quadSc, markSc, panelSc; static bool havePanels;
static bool haveMarkers;
static int64_t scFormat;
static XrFovf  usedFov;                   // symmetric fov submitted with the eye images
static VrView  shownViews[2]; static bool haveEyeImage;
static uint32_t shownScreenFrame = 0xFFFFFFFF; static bool haveScreen;

static bool MakeSwap(Swap& s, int w, int h) {
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = scFormat; ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XRCHECK(xrCreateSwapchain(sess, &ci, &s.sc));
    uint32_t n = 0; xrEnumerateSwapchainImages(s.sc, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XRCHECK(xrEnumerateSwapchainImages(s.sc, n, &n, (XrSwapchainImageBaseHeader*)imgs.data()));
    for (auto& im : imgs) {
        D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = (DXGI_FORMAT)scFormat; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* r = nullptr; d3d->CreateRenderTargetView(im.texture, &rd, &r); s.rtv.push_back(r);
    }
    s.w = w; s.h = h; return true;
}

static bool CreateRecenteredLocal(const XrPosef* pose) {
    if (localSpace) xrDestroySpace(localSpace);
    XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace = pose ? *pose : XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
    XRCHECK(xrCreateReferenceSpace(sess, &rs, &localSpace));
    return true;
}

static XrPath P(const char* s) { XrPath p = XR_NULL_PATH; xrStringToPath(inst, s, &p); return p; }
static bool MakeAction(XrAction* a, const char* name, const char* loc, XrActionType t) {
    XrActionCreateInfo ci = {XR_TYPE_ACTION_CREATE_INFO};
    strcpy_s(ci.actionName, name); strcpy_s(ci.localizedActionName, loc); ci.actionType = t;
    return XR_SUCCEEDED(xrCreateAction(s_actSet, &ci, a));
}
// Flight controls as OpenXR actions. SteamVR lists them in its binding UI so every button can be remapped;
// defaults are suggested for Index-style and Touch-style controllers.
static void CreateActions() {
    XrActionSetCreateInfo si = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(si.actionSetName, "flight"); strcpy_s(si.localizedActionSetName, "Flight"); si.priority = 0;
    if (XR_FAILED(xrCreateActionSet(inst, &si, &s_actSet))) { Log("[ctl] xrCreateActionSet failed"); return; }
    bool ok = MakeAction(&s_actStickR, "pitch_yaw", "Pitch / Yaw", XR_ACTION_TYPE_VECTOR2F_INPUT) &&
              MakeAction(&s_actStickL, "roll_throttle", "Roll / Throttle", XR_ACTION_TYPE_VECTOR2F_INPUT) &&
              MakeAction(&s_actRecenter, "recenter", "Recenter view", XR_ACTION_TYPE_BOOLEAN_INPUT);
    for (int i = 0; i < kVrCommandCount && ok; i++) ok = MakeAction(&s_actBtn[i], kVrCommands[i].action, kVrCommands[i].label, XR_ACTION_TYPE_BOOLEAN_INPUT);
    ok = ok && MakeAction(&s_actAim, "pointer", "Menu pointer", XR_ACTION_TYPE_POSE_INPUT) &&
         MakeAction(&s_actClick, "menu_click", "Menu click", XR_ACTION_TYPE_BOOLEAN_INPUT) &&
         MakeAction(&s_actRClick, "menu_right_click", "Menu right click (cycle hotspots)", XR_ACTION_TYPE_BOOLEAN_INPUT) &&
         MakeAction(&s_actEsc, "menu_escape", "Menu escape / skip film (Esc)", XR_ACTION_TYPE_BOOLEAN_INPUT) &&
         MakeAction(&s_actHaptic[0], "rumble_left", "Rumble left hand", XR_ACTION_TYPE_VIBRATION_OUTPUT) &&
         MakeAction(&s_actHaptic[1], "rumble_right", "Rumble right hand", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    if (!ok) { Log("[ctl] xrCreateAction failed"); return; }

    struct B { XrAction a; const char* path; };
    auto suggest = [&](const char* profile, const B* b, int n) {
        std::vector<XrActionSuggestedBinding> v;
        for (int i = 0; i < n; i++) v.push_back({b[i].a, P(b[i].path)});
        XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = P(profile); sb.suggestedBindings = v.data(); sb.countSuggestedBindings = (uint32_t)v.size();
        XrResult r = xrSuggestInteractionProfileBindings(inst, &sb);
        Log("[ctl] default bindings for %s: %s", profile, XR_SUCCEEDED(r) ? "ok" : "rejected");
        return XR_SUCCEEDED(r);
    };
    for (int prof = 0; prof < 3; prof++) {
        if (prof == 2 && !s_frameExt) break;
        std::vector<B> v = {{s_actStickR, "/user/hand/right/input/thumbstick"}, {s_actStickL, "/user/hand/left/input/thumbstick"},
                            {s_actRecenter, "/user/hand/right/input/thumbstick/click"},
                            {s_actAim, "/user/hand/right/input/aim/pose"}, {s_actClick, "/user/hand/right/input/trigger/value"},
                            {s_actRClick, "/user/hand/right/input/squeeze/value"},
                            {s_actEsc, "/user/hand/left/input/trigger/value"}};
        for (int i = 0; i < kVrCommandCount; i++) {
            const char* path = prof == 0 ? kVrCommands[i].indexPath : prof == 1 ? kVrCommands[i].touchPath : kVrCommands[i].framePath;
            if (path) v.push_back({s_actBtn[i], path});
        }
        const char* prName = prof == 0 ? "/interaction_profiles/valve/index_controller" : prof == 1 ? "/interaction_profiles/oculus/touch_controller"
                                                                                     : "/interaction_profiles/valve/frame_controller_valve";
        std::vector<B> withHaptics = v;
        withHaptics.push_back({s_actHaptic[0], "/user/hand/left/output/haptic"}); withHaptics.push_back({s_actHaptic[1], "/user/hand/right/output/haptic"});
        if (!suggest(prName, withHaptics.data(), (int)withHaptics.size())) suggest(prName, v.data(), (int)v.size());   // retry without rumble
    }

    XrSessionActionSetsAttachInfo ai = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO}; ai.countActionSets = 1; ai.actionSets = &s_actSet;
    s_actionsReady = XR_SUCCEEDED(xrAttachSessionActionSets(sess, &ai));
    XrActionSpaceCreateInfo asi = {XR_TYPE_ACTION_SPACE_CREATE_INFO}; asi.action = s_actAim; asi.poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    xrCreateActionSpace(sess, &asi, &s_aimSpace);
    Log("[ctl] controller actions %s", s_actionsReady ? "attached" : "FAILED to attach");
}

// ---------------------------------------------------------------- menu pointer (outside flight)
static Swap beamSc; static bool beamReady;
static HWND GameWnd();
// Message panel shown instead of the (frozen) game while a desktop tool of the game is open (p2den's Alt+J joystick setup)
static Swap msgSc[2]; static bool msgReady[2]; static const int MSG_W = 1280, MSG_H = 200;
static volatile LONG s_msgShow;   // Vr_ShowMessage: 1 = entering-space prompt
void Vr_ShowMessage(int which) { InterlockedExchange(&s_msgShow, which); }
// cursor offset of the current 2D screen (PAD, nav map): reported while that cursor is drawn, forgotten 1 s after
static volatile LONG s_curOff, s_curOffTick;
void Vr_SetCursorOffset(int x, int y) { InterlockedExchange(&s_curOff, (x & 0xffff) | (y << 16)); InterlockedExchange(&s_curOffTick, (LONG)GetTickCount()); }
static void MakeMessage(int slot, const char* text) {
    Swap& sw = msgSc[slot];
    if (!MakeSwap(sw, MSG_W, MSG_H)) return;
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = MSG_W; bi.bmiHeader.biHeight = -MSG_H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr; HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(dc); return; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    RECT r = {0, 0, MSG_W, MSG_H}; HBRUSH bg = CreateSolidBrush(RGB(16, 22, 30)); FillRect(dc, &r, bg); DeleteObject(bg);
    HFONT font = CreateFontA(56, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(120, 200, 255));
    RECT m = {24, 0, MSG_W - 24, MSG_H}; DrawTextA(dc, text, -1, &m, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);   // centre (multi-line)
    int th = m.bottom - m.top; RECT t = {24, (MSG_H - th) / 2, MSG_W - 24, (MSG_H - th) / 2 + th};
    DrawTextA(dc, text, -1, &t, DT_CENTER | DT_WORDBREAK);
    GdiFlush();
    std::vector<uint8_t> px((size_t)MSG_W * MSG_H * 4); const uint8_t* src = (const uint8_t*)bits;
    for (size_t k = 0; k < (size_t)MSG_W * MSG_H; k++) {   // GDI BGRX -> RGBA (sRGB swapchain), opaque panel
        bool rgba = scFormat == (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        px[k*4] = rgba ? src[k*4+2] : src[k*4]; px[k*4+1] = src[k*4+1]; px[k*4+2] = rgba ? src[k*4] : src[k*4+2]; px[k*4+3] = 255;
    }
    SelectObject(dc, oldFont); DeleteObject(font); SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);
    uint32_t i = 0; XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(sw.sc, &ai, &i))) return;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION; xrWaitSwapchainImage(sw.sc, &wi);
    ID3D11Resource* res = nullptr; sw.rtv[i]->GetResource(&res);
    if (res) { ctx->UpdateSubresource(res, 0, nullptr, px.data(), MSG_W * 4, 0); res->Release(); }
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(sw.sc, &ri);
    msgReady[slot] = true;
}
// another visible window of the game process (besides the main game window): a desktop tool is open
static bool DesktopToolOpen() {
    static DWORD lastCheck; static bool open;
    if (GetTickCount() - lastCheck < 250) return open;
    lastCheck = GetTickCount();
    struct F { HWND game; HWND tool; } f = {GameWnd(), nullptr};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        F* f = (F*)lp; DWORD pid; GetWindowThreadProcessId(w, &pid);
        if (pid != GetCurrentProcessId() || w == f->game || !IsWindowVisible(w) || IsIconic(w)) return TRUE;
        RECT r; GetWindowRect(w, &r); if (r.right - r.left < 100 || r.bottom - r.top < 100) return TRUE;
        f->tool = w; return FALSE;
    }, (LPARAM)&f);
    bool now = f.tool != nullptr;
    if (now != open) {
        char cls[64] = "", title[128] = ""; if (f.tool) { GetClassNameA(f.tool, cls, sizeof cls); GetWindowTextA(f.tool, title, sizeof title); }
        Log("[vr] desktop tool window %s %s %s", now ? "open:" : "closed", cls, title);
    }
    open = now; return open;
}
static bool s_ptrHit; static XrVector3f s_ptrFrom, s_ptrTo; static bool s_clickWas, s_rclickWas;
static HWND GameWnd() {
    struct F { HWND best; LONG area; } f = {nullptr, 0};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        DWORD pid; GetWindowThreadProcessId(w, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(w)) return TRUE;
        RECT r; GetClientRect(w, &r); LONG a = (r.right - r.left) * (r.bottom - r.top);
        F* f = (F*)lp; if (a > f->area) { f->area = a; f->best = w; } return TRUE;
    }, (LPARAM)&f);
    return f.best;
}
static bool GameFront() { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); return pid == GetCurrentProcessId(); }
static void MouseButton(DWORD flag) { INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = flag; SendInput(1, &in, sizeof in); }
// Ray from the right controller onto the virtual screen -> move the real mouse there; trigger/grip = clicks.
static void UpdatePointer(XrTime t, bool flight, float screenDist, float screenW, float screenY) {
    s_ptrHit = false;
    if (s_actionsReady && Macro_Replaying()) {   // replay: no VR pointer/clicks; the Esc action stops the replay
        XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO}; XrActionStateBoolean b = {XR_TYPE_ACTION_STATE_BOOLEAN}; gi.action = s_actEsc;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(sess, &gi, &b)) && b.isActive && b.currentState) Macro_Abort("VR Esc pressed");
    }
    if (flight || !s_actionsReady || !s_aimSpace || Macro_Replaying()) { s_clickWas = s_rclickWas = false; return; }
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(s_aimSpace, localSpace, t, &loc)) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) || !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
    const XrQuaternionf& q = loc.pose.orientation; const XrVector3f& o = loc.pose.position;
    // forward (-Z) of the aim pose
    float dx = -(2 * (q.x * q.z + q.w * q.y)), dy = -(2 * (q.y * q.z - q.w * q.x)), dz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    s_ptrFrom = o;
    float len = 3.0f;
    if (dz < -1e-3f) {                                  // screen plane z = -screenDist
        float tt = (-screenDist - o.z) / dz;
        float hx = o.x + dx * tt, hy = o.y + dy * tt;
        float u = hx / screenW + 0.5f, v = 0.5f - (hy - screenY) / (screenW * 0.75f);
        if (tt > 0 && u >= 0 && u <= 1 && v >= 0 && v <= 1) {
            len = tt; s_ptrHit = true;
            HWND w = GameWnd();
            if (w && GameFront()) {
                RECT r; GetClientRect(w, &r);
                // screens that draw the cursor offset from the mouse (PAD, nav map): move the mouse so the drawn cursor,
                // which is what the game hit-tests, lands where the beam points
                float mu = u, mv = v;
                if (GetTickCount() - (DWORD)s_curOffTick < 1000) { LONG o = s_curOff; mu -= (o & 0xffff) / 640.f; mv -= (o >> 16) / 480.f; }
                POINT pt = {LONG((s_uOff + mu * s_uScale) * r.right), LONG(mv * r.bottom)};
                ClientToScreen(w, &pt);
                static POINT last = {-1, -1};
                if (pt.x != last.x || pt.y != last.y) {
                    last = pt;
                    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                    INPUT in = {}; in.type = INPUT_MOUSE;
                    in.mi.dx = LONG((pt.x - vx) * 65535.0 / (vw - 1)); in.mi.dy = LONG((pt.y - vy) * 65535.0 / (vh - 1));
                    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
                    SendInput(1, &in, sizeof in);
                }
            }
        }
    }
    s_ptrTo = {o.x + dx * len, o.y + dy * len, o.z + dz * len};
    // clicks (only into the game window)
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO}; XrActionStateBoolean b = {XR_TYPE_ACTION_STATE_BOOLEAN};
    gi.action = s_actClick; bool click = XR_SUCCEEDED(xrGetActionStateBoolean(sess, &gi, &b)) && b.isActive && b.currentState;
    b = {XR_TYPE_ACTION_STATE_BOOLEAN}; gi.action = s_actRClick; bool rclick = XR_SUCCEEDED(xrGetActionStateBoolean(sess, &gi, &b)) && b.isActive && b.currentState;
    bool front = GameFront();
    if (front && click != s_clickWas) MouseButton(click ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
    if (front && rclick != s_rclickWas) MouseButton(rclick ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
    s_clickWas = click; s_rclickWas = rclick;
    // Escape (skip films, leave screens)
    static bool escWas;
    b = {XR_TYPE_ACTION_STATE_BOOLEAN}; gi.action = s_actEsc; bool esc = XR_SUCCEEDED(xrGetActionStateBoolean(sess, &gi, &b)) && b.isActive && b.currentState;
    if (front && esc != escWas) { INPUT in = {}; in.type = INPUT_KEYBOARD; in.ki.wScan = 0x01; in.ki.dwFlags = KEYEVENTF_SCANCODE | (esc ? 0 : KEYEVENTF_KEYUP); SendInput(1, &in, sizeof in); }
    escWas = esc;
}
// Beam: a thin quad from the controller to the hit point, turned around its axis to face the viewer.
static bool BeamLayer(XrCompositionLayerQuad& q, const XrVector3f& head) {
    if (!beamReady) return false;
    float ax = s_ptrTo.x - s_ptrFrom.x, ay = s_ptrTo.y - s_ptrFrom.y, az = s_ptrTo.z - s_ptrFrom.z, L = sqrtf(ax*ax + ay*ay + az*az);
    if (L < 0.05f) return false;
    float y[3] = {ax / L, ay / L, az / L};
    float mx = (s_ptrFrom.x + s_ptrTo.x) * 0.5f, my = (s_ptrFrom.y + s_ptrTo.y) * 0.5f, mz = (s_ptrFrom.z + s_ptrTo.z) * 0.5f;
    float t[3] = {head.x - mx, head.y - my, head.z - mz}, d = t[0]*y[0] + t[1]*y[1] + t[2]*y[2];
    float z[3] = {t[0] - d*y[0], t[1] - d*y[1], t[2] - d*y[2]}, zl = sqrtf(z[0]*z[0] + z[1]*z[1] + z[2]*z[2]);
    if (zl < 1e-4f) return false;
    for (float& v : z) v /= zl;
    float x[3] = {y[1]*z[2] - y[2]*z[1], y[2]*z[0] - y[0]*z[2], y[0]*z[1] - y[1]*z[0]};
    // rotation matrix (columns x, y, z) -> quaternion
    float tr = x[0] + y[1] + z[2], qw, qx, qy, qz;
    if (tr > 0) { float s2 = sqrtf(tr + 1) * 2; qw = 0.25f * s2; qx = (y[2] - z[1]) / s2; qy = (z[0] - x[2]) / s2; qz = (x[1] - y[0]) / s2; }
    else if (x[0] > y[1] && x[0] > z[2]) { float s2 = sqrtf(1 + x[0] - y[1] - z[2]) * 2; qw = (y[2] - z[1]) / s2; qx = 0.25f * s2; qy = (y[0] + x[1]) / s2; qz = (z[0] + x[2]) / s2; }
    else if (y[1] > z[2]) { float s2 = sqrtf(1 + y[1] - x[0] - z[2]) * 2; qw = (z[0] - x[2]) / s2; qx = (y[0] + x[1]) / s2; qy = 0.25f * s2; qz = (z[1] + y[2]) / s2; }
    else { float s2 = sqrtf(1 + z[2] - x[0] - y[1]) * 2; qw = (x[1] - y[0]) / s2; qx = (z[0] + x[2]) / s2; qy = (z[1] + y[2]) / s2; qz = 0.25f * s2; }
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.space = localSpace; q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = beamSc.sc; q.subImage.imageRect = {{0, 0}, {beamSc.w, beamSc.h}};
    q.size = {0.004f, L}; q.pose = {{qx, qy, qz, qw}, {mx, my, mz}};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    return true;
}

static void ApplyHaptics(bool focused) {
    float amp[2], dur[2]; bool go[2];
    EnterCriticalSection(&s_ctlCs);
    for (int h = 0; h < 2; h++) { go[h] = s_hapPending[h]; amp[h] = s_hapAmp[h]; dur[h] = s_hapDur[h]; s_hapPending[h] = false; }
    LeaveCriticalSection(&s_ctlCs);
    if (!focused || !s_actionsReady) return;
    for (int h = 0; h < 2; h++) if (go[h]) {
        XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION};
        v.amplitude = amp[h] > 1 ? 1 : amp[h]; v.duration = (XrDuration)(dur[h] * 1e9); v.frequency = XR_FREQUENCY_UNSPECIFIED;
        XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO}; hi.action = s_actHaptic[h];
        xrApplyHapticFeedback(sess, &hi, (XrHapticBaseHeader*)&v);
    }
}
static float Dead(float v) {   // deadzone with rescale, plus a gentle curve for fine aiming
    float a = fabsf(v); if (a < s_deadzone) return 0;
    a = (a - s_deadzone) / (1 - s_deadzone); a = a * a * 0.5f + a * 0.5f;
    return v < 0 ? -a : a;
}
static void PollControls(bool focused) {
    VrControls c = {};
    if (s_actionsReady && focused) {
        XrActiveActionSet as = {s_actSet, XR_NULL_PATH};
        XrActionsSyncInfo sy = {XR_TYPE_ACTIONS_SYNC_INFO}; sy.countActiveActionSets = 1; sy.activeActionSets = &as;
        if (XR_SUCCEEDED(xrSyncActions(sess, &sy))) {
            XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
            XrActionStateVector2f v = {XR_TYPE_ACTION_STATE_VECTOR2F};
            bool any = false;
            gi.action = s_actStickR; if (XR_SUCCEEDED(xrGetActionStateVector2f(sess, &gi, &v)) && v.isActive) { c.yaw = Dead(v.currentState.x); c.pitch = Dead(v.currentState.y); any = true; }
            v = {XR_TYPE_ACTION_STATE_VECTOR2F};
            gi.action = s_actStickL; if (XR_SUCCEEDED(xrGetActionStateVector2f(sess, &gi, &v)) && v.isActive) { c.roll = Dead(v.currentState.x); c.throttleRate = Dead(v.currentState.y); any = true; }
            XrActionStateBoolean b = {XR_TYPE_ACTION_STATE_BOOLEAN};
            for (int i = 0; i < kVrCommandCount; i++) {
                b = {XR_TYPE_ACTION_STATE_BOOLEAN}; gi.action = s_actBtn[i];
                if (XR_SUCCEEDED(xrGetActionStateBoolean(sess, &gi, &b)) && b.isActive) { any = true; if (b.currentState) c.buttons |= 1ull << i; }
            }
            c.anyButton = c.buttons != 0;
            for (XrAction a : {s_actClick, s_actRClick, s_actEsc}) {
                b = {XR_TYPE_ACTION_STATE_BOOLEAN}; gi.action = a;
                if (XR_SUCCEEDED(xrGetActionStateBoolean(sess, &gi, &b)) && b.isActive && b.currentState) c.anyButton = true;
            }
            b = {XR_TYPE_ACTION_STATE_BOOLEAN}; gi.action = s_actRecenter;
            if (XR_SUCCEEDED(xrGetActionStateBoolean(sess, &gi, &b)) && b.isActive && b.currentState && b.changedSinceLastSync) Vr_RequestRecenter();
            c.active = any;
        }
    }
    static bool wasActive; static DWORD lastDiag;
    if (c.active != wasActive) { wasActive = c.active; Log("[ctl] VR controllers %s", c.active ? "active" : "inactive"); }
    if (GetTickCount() - lastDiag > 5000) {
        lastDiag = GetTickCount();
        Log("[ctl] state: session %d focused %d actions %d active %d | R(%.2f %.2f) L(%.2f %.2f) buttons %llx",
            (int)sessState, focused, s_actionsReady, c.active, c.yaw, c.pitch, c.roll, c.throttleRate, c.buttons);
    }
    EnterCriticalSection(&s_ctlCs); s_ctl = c; LeaveCriticalSection(&s_ctlCs);
}

static bool InitXr() {
    // SteamVR does not register its 32-bit runtime as active; point the loader at it.
    char buf[MAX_PATH];
    if (!GetEnvironmentVariableA("XR_RUNTIME_JSON", buf, MAX_PATH)) {
        const char* def = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\steamxr_win32.json";
        if (GetFileAttributesA(def) != INVALID_FILE_ATTRIBUTES) SetEnvironmentVariableA("XR_RUNTIME_JSON", def);
    }
    // Steam Frame controllers get their own profile when the runtime offers it (otherwise SteamVR remaps Touch)
    uint32_t nExt = 0; xrEnumerateInstanceExtensionProperties(nullptr, 0, &nExt, nullptr);
    std::vector<XrExtensionProperties> props(nExt, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, nExt, &nExt, props.data());
    s_frameExt = false;
    for (auto& e : props) if (!strcmp(e.extensionName, "XR_VALVE_frame_controller_interaction")) s_frameExt = true;
    const char* exts[2] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, "XR_VALVE_frame_controller_interaction"};
    XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "Privateer 2 VR");
    strcpy_s(ici.applicationInfo.engineName, "BRender");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = s_frameExt ? 2 : 1; ici.enabledExtensionNames = exts;
    Log("[ctl] Steam Frame controller profile extension: %s", s_frameExt ? "available" : "not offered");
    XRCHECK(xrCreateInstance(&ici, &inst));
    XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO}; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XRCHECK(xrGetSystem(inst, &sgi, &sysId));
    XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES}; xrGetSystemProperties(inst, sysId, &sp);
    s_maxLayers = (int)sp.graphicsProperties.maxLayerCount; if (s_maxLayers < 4) s_maxLayers = 4;
    Log("[vr] system: %s (max %d layers)", sp.systemName, s_maxLayers);

    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    XRCHECK(xrGetInstanceProcAddr(inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq));
    XrGraphicsRequirementsD3D11KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XRCHECK(getReq(inst, sysId, &req));
    s_luid = req.adapterLuid;
    if (!CreateDeviceForLuid(req.adapterLuid)) return false;

    XrGraphicsBindingD3D11KHR gb = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR}; gb.device = d3d;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO}; sci.next = &gb; sci.systemId = sysId;
    XRCHECK(xrCreateSession(inst, &sci, &sess));
    if (!CreateRecenteredLocal(nullptr)) return false;
    CreateActions();
    XrReferenceSpaceCreateInfo vs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; vs.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    XRCHECK(xrCreateReferenceSpace(sess, &vs, &viewSpace));

    uint32_t nf = 0; xrEnumerateSwapchainFormats(sess, 0, &nf, nullptr);
    std::vector<int64_t> fmts(nf); xrEnumerateSwapchainFormats(sess, nf, &nf, fmts.data());
    scFormat = 0;
    for (int64_t want : {(int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})
        for (auto f : fmts) if (f == want && !scFormat) scFormat = f;
    if (!scFormat) { Log("[vr] no sRGB swapchain format"); return false; }

    // Eye render target: symmetric frustum covering both eyes' (asymmetric) fov.
    uint32_t nv = 0; xrEnumerateViewConfigurationViews(inst, sysId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nv, nullptr);
    std::vector<XrViewConfigurationView> vcv(nv, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XRCHECK(xrEnumerateViewConfigurationViews(inst, sysId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nv, &nv, vcv.data()));
    Log("[vr] recommended eye size %ux%u", vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight);
    // fov comes from a first xrLocateViews in the frame loop; size finalised there.
    EnterCriticalSection(&s_cs);
    s_target.height = (int)(vcv[0].recommendedImageRectHeight * s_eyeScale);
    LeaveCriticalSection(&s_cs);
    return true;
}

static bool FinaliseEyeTarget(const XrView v[2]) {
    // BRender only does symmetric frusta. Instead of one symmetric frustum covering the asymmetric eye fov (wasting
    // pixels), each eye's render camera is turned to the centre of its own fov and gets a tight symmetric frustum.
    // The projection layer is submitted with that rotated pose, so the runtime places it exactly.
    float tH = 0, tV = 0;
    for (int i = 0; i < 2; i++) {
        float cH = 0.5f * (v[i].fov.angleLeft + v[i].fov.angleRight), cV = 0.5f * (v[i].fov.angleUp + v[i].fov.angleDown);
        float hH = 0.5f * (v[i].fov.angleRight - v[i].fov.angleLeft), hV = 0.5f * (v[i].fov.angleUp - v[i].fov.angleDown);
        tH = fmaxf(tH, tanf(hH)); tV = fmaxf(tV, tanf(hV));
        // q = yaw(-cH) * pitch(cV)  (yaw about +Y turns -Z to the left; pitch about +X turns -Z up)
        float yw = -cH * 0.5f, pt = cV * 0.5f;
        float qy[4] = {0, sinf(yw), 0, cosf(yw)}, qp[4] = {sinf(pt), 0, 0, cosf(pt)};
        float* q = s_offQuat[i];
        q[0] = qy[3]*qp[0] + qy[0]*qp[3] + qy[1]*qp[2] - qy[2]*qp[1];
        q[1] = qy[3]*qp[1] - qy[0]*qp[2] + qy[1]*qp[3] + qy[2]*qp[0];
        q[2] = qy[3]*qp[2] + qy[0]*qp[1] - qy[1]*qp[0] + qy[2]*qp[3];
        q[3] = qy[3]*qp[3] - qy[0]*qp[0] - qy[1]*qp[1] - qy[2]*qp[2];
        Log("[vr] eye %d fov centre yaw %.1f pitch %.1f deg, half %.1f x %.1f deg", i, -cH * 57.29578f, cV * 57.29578f, hH * 57.29578f, hV * 57.29578f);
    }
    int cap = s_eyeMax;
    int h = s_target.height; if (h > cap) h = cap; if (h < 256) h = 256;
    int w = (int)(h * tH / tV + 0.5f);
    if (w > cap) { h = (int)(h * (float)cap / w); w = cap; }
    usedFov.angleLeft = -atanf(tH); usedFov.angleRight = atanf(tH);
    usedFov.angleUp = atanf(tV); usedFov.angleDown = -atanf(tV);
    Log("[vr] eye fov L%.1f R%.1f U%.1f D%.1f -> centred symmetric %.1fx%.1f deg, render %dx%d",
        v[0].fov.angleLeft * 57.29578f, v[0].fov.angleRight * 57.29578f, v[0].fov.angleUp * 57.29578f, v[0].fov.angleDown * 57.29578f,
        2 * atanf(tH) * 57.29578f, 2 * atanf(tV) * 57.29578f, w, h);
    for (int e = 0; e < 2; e++) {
        if (!MakeSwap(eyeSc[e], w, h)) return false;
        D3D11_TEXTURE2D_DESC td = {(UINT)w, (UINT)h, 1, 1, DXGI_FORMAT_R8_UINT, {1, 0}, D3D11_USAGE_DYNAMIC,
                                   D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE};
        d3d->CreateTexture2D(&td, nullptr, &idxTex[e]); d3d->CreateShaderResourceView(idxTex[e], nullptr, &idxSrv[e]);
    }
    s_retPx = (int)(s_retFrac * HUD_PX_W + 0.5f); if (s_retPx < 16) s_retPx = 16; if (s_retPx > HUD_PX_H) s_retPx = HUD_PX_H;
    if (!MakeSwap(quadSc, HUD_PX_W + s_retPx, HUD_PX_H)) return false;   // [ HUD | crosshair crop ]
    if (!MakeSwap(markSc, 5120, 960)) return false;   // 4 x 640x480 at 2x: markers | lead | octagon | lock reticle
    if (s_panelsOn && !MakeSwap(panelSc, HUD_PX_W, HUD_PX_H)) s_panelsOn = false;
    MakeMessage(0, "Use the desktop to configure the joystick");
    MakeMessage(1, "Entering Space in VR.\nPress the fire button to continue");
    if (MakeSwap(beamSc, 8, 64)) {   // beam colour (premultiplied, semi-transparent cyan)
        uint32_t i = 0; XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(beamSc.sc, &ai, &i))) {
            XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION; xrWaitSwapchainImage(beamSc.sc, &wi);
            float col[4] = {0.10f, 0.45f, 0.60f, 0.6f}; ctx->ClearRenderTargetView(beamSc.rtv[i], col);
            XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(beamSc.sc, &ri);
            beamReady = true;
        }
    }                        // [ markers | lead indicator | cached octagon ], 2x
    EnterCriticalSection(&s_cs);
    s_target.width = w; s_target.height = h; s_target.tanHalfH = tH; s_target.tanHalfV = tV; s_targetValid = true;
    LeaveCriticalSection(&s_cs);
    return true;
}

static void DrawTo(Swap& s, ID3D11PixelShader* ps, ID3D11ShaderResourceView* const* srvs, float keyOn) {
    uint32_t i = 0; XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(s.sc, &ai, &i))) return;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(s.sc, &wi);
    float cb[16] = {s_keyGain, keyOn, s_uScale, s_uOff, s_keyThresh, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0}; ctx->UpdateSubresource(cbuf, 0, nullptr, cb, 0, 0);
    D3D11_VIEWPORT vp = {0, 0, (float)s.w, (float)s.h, 0, 1};
    ctx->OMSetRenderTargets(1, &s.rtv[i], nullptr); ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vsFull, nullptr, 0); ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 3, srvs); ctx->PSSetSamplers(0, 1, &linSamp); ctx->PSSetConstantBuffers(0, 1, &cbuf);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[3] = {}; ctx->PSSetShaderResources(0, 3, nulls);
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(s.sc, &ri);
}

// HUD/screen swapchain layout: left HUD_PX_W x HUD_PX_H = full image (crosshair cut out in flight),
// right s_retPx square = crosshair crop (flight only).
static void DrawHud(ID3D11ShaderResourceView* src, bool flight) {
    Swap& s = quadSc;
    uint32_t i = 0; XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(s.sc, &ai, &i))) return;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(s.sc, &wi);
    float clear[4] = {0, 0, 0, 0}; ctx->ClearRenderTargetView(s.rtv[i], clear);
    ctx->OMSetRenderTargets(1, &s.rtv[i], nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vsFull, nullptr, 0); ctx->PSSetShader(psScreen, nullptr, 0);
    ID3D11ShaderResourceView* srvs[3] = {nullptr, nullptr, src};
    ctx->PSSetShaderResources(0, 3, srvs); ctx->PSSetSamplers(0, 1, &linSamp); ctx->PSSetConstantBuffers(0, 1, &cbuf);
    float pcb[36] = {};   // panel rectangles (uv) cut out of the main HUD in flight
    bool panels = flight && s_panelsOn;
    if (panels) { for (int k = 0; k < s_nPanels; k++) { pcb[k*4] = s_panels[k].r[0] / 640.f; pcb[k*4+1] = s_panels[k].r[1] / 480.f;
                                                          pcb[k*4+2] = s_panels[k].r[2] / 640.f; pcb[k*4+3] = s_panels[k].r[3] / 480.f; }
                  pcb[32] = (float)s_nPanels; }
    ctx->UpdateSubresource(cbufPanels, 0, nullptr, pcb, 0, 0); ctx->PSSetConstantBuffers(2, 1, &cbufPanels);
    float hw = s_retPx / (2.0f * HUD_PX_W), hh = s_retPx / (2.0f * HUD_PX_H);   // crosshair region half-size in HUD uv
    // pass 1: whole HUD (hole over the crosshair region in flight)
    bool split = flight && s_retDist > 0 && !s_retGlass;   // separate crosshair layer (crop pass)
    bool hole = flight && (s_retDist > 0 || s_retGlass);    // crosshair region on the panel: cut out, or semi-transparent
    float cb1[16] = {s_keyGain, flight ? 1.f : 0.f, s_uScale, s_uOff, s_keyThresh, hole ? 1.f : 0.f, s_retGlass ? s_glassAlpha : 0.f, 0,
                     0, 0, 1, 1, 0.5f - hw, 0.5f - hh, 0.5f + hw, 0.5f + hh};
    ctx->UpdateSubresource(cbuf, 0, nullptr, cb1, 0, 0);
    D3D11_VIEWPORT vp1 = {0, 0, (float)HUD_PX_W, (float)HUD_PX_H, 0, 1}; ctx->RSSetViewports(1, &vp1); ctx->Draw(3, 0);
    if (split) {    // pass 2: crosshair crop
        float cb2[16] = {s_keyGain, 1.f, s_uScale, s_uOff, s_keyThresh, 0, 0, 0,
                         0.5f - hw, 0.5f - hh, 2 * hw, 2 * hh, 0, 0, 0, 0};
        ctx->UpdateSubresource(cbuf, 0, nullptr, cb2, 0, 0);
        D3D11_VIEWPORT vp2 = {(float)HUD_PX_W, 0, (float)s_retPx, (float)s_retPx, 0, 1}; ctx->RSSetViewports(1, &vp2); ctx->Draw(3, 0);
    }
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(s.sc, &ri);
    if (panels) {   // the whole keyed HUD (no cut-outs) for the panel quads to take their rectangles from
        uint32_t j = 0;
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(panelSc.sc, &ai, &j))) {
            xrWaitSwapchainImage(panelSc.sc, &wi);
            ctx->ClearRenderTargetView(panelSc.rtv[j], clear);
            ctx->OMSetRenderTargets(1, &panelSc.rtv[j], nullptr);
            float zero[36] = {}; ctx->UpdateSubresource(cbufPanels, 0, nullptr, zero, 0, 0);
            float cbp[16] = {s_keyGain, 1.f, s_uScale, s_uOff, s_keyThresh, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0};
            ctx->UpdateSubresource(cbuf, 0, nullptr, cbp, 0, 0);
            D3D11_VIEWPORT vp3 = {0, 0, (float)HUD_PX_W, (float)HUD_PX_H, 0, 1}; ctx->RSSetViewports(1, &vp3); ctx->Draw(3, 0);
            xrReleaseSwapchainImage(panelSc.sc, &ri);
            havePanels = true;
        }
    }
    ID3D11ShaderResourceView* nulls[3] = {}; ctx->PSSetShaderResources(0, 3, nulls);
    ID3D11Buffer* nullCb = nullptr; ctx->PSSetConstantBuffers(2, 1, &nullCb);
}

static void UpdateMarkers() {
    EnterCriticalSection(&s_cs);
    if (!s_mkNew) { LeaveCriticalSection(&s_cs); return; }
    // wait (briefly) until the exact 3D poses for this marker set have arrived, so markers never jump
    if (!s_pHave && GetTickCount() - s_mkSubmitTick < 100) { LeaveCriticalSection(&s_cs); return; }
    s_mkNew = false;
    if (!mkTex || mkTexW != s_mkW || mkTexH != s_mkH) {
        if (mkSrv) mkSrv->Release(); if (mkTex) mkTex->Release(); mkTex = nullptr; mkSrv = nullptr;
        D3D11_TEXTURE2D_DESC td = {(UINT)s_mkW, (UINT)s_mkH, 1, 1, DXGI_FORMAT_R8_UINT, {1, 0}, D3D11_USAGE_DYNAMIC,
                                   D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE};
        if (SUCCEEDED(d3d->CreateTexture2D(&td, nullptr, &mkTex))) d3d->CreateShaderResourceView(mkTex, nullptr, &mkSrv);
        mkTexW = s_mkW; mkTexH = s_mkH;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (mkTex && SUCCEEDED(ctx->Map(mkTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        for (int y = 0; y < s_mkH; y++) memcpy((uint8_t*)m.pData + y * m.RowPitch, &s_mk[(size_t)y * s_mkW], s_mkW);
        ctx->Unmap(mkTex, 0);
    }
    ctx->UpdateSubresource(mkPalTex, 0, nullptr, s_mkPal, 1024, 0);
    s_nShownBlobs = s_nBlobs; memcpy(s_shownBlobs, s_blobs, sizeof(VrMarkerBlob) * s_nBlobs); s_shownSerial = s_mkSerial;
    memset(s_mValid, 0, sizeof s_mValid);
    if (s_pHave) { memcpy(s_mPos, s_pPos, sizeof s_mPos); memcpy(s_mSize, s_pSize, sizeof s_mSize); memcpy(s_mValid, s_pValid, sizeof s_mValid); }
    LeaveCriticalSection(&s_cs);
    if (!mkSrv) return;
    Swap& sw = markSc;
    uint32_t i = 0; XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(sw.sc, &ai, &i))) return;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sw.sc, &wi);
    float cb[16] = {0, 0, 0, 0, 0, 0, 0, 0, (float)mkTexW, (float)mkTexH, 0, 0, 0, 0, 0, 0};
    ctx->UpdateSubresource(cbuf, 0, nullptr, cb, 0, 0);
    D3D11_VIEWPORT vp = {0, 0, (float)sw.w, (float)sw.h, 0, 1};
    ctx->OMSetRenderTargets(1, &sw.rtv[i], nullptr); ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vsFull, nullptr, 0); ctx->PSSetShader(psMarker, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = {mkSrv, mkPalSrv};
    ctx->PSSetShaderResources(0, 2, srvs); ctx->PSSetConstantBuffers(0, 1, &cbuf);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[2] = {}; ctx->PSSetShaderResources(0, 2, nulls);
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(sw.sc, &ri);
    haveMarkers = true;
}

static void UploadEyes() {
    int set;
    EnterCriticalSection(&s_cs); set = s_ready; s_ready = -1; if (set >= 0) s_inUse = set; LeaveCriticalSection(&s_cs);
    if (set < 0) return;
    EyeSet& es = s_sets[set];
    int w = s_target.width, h = s_target.height;
    EnterCriticalSection(&s_cs);
    if (es.mHave && es.mSerial == s_shownSerial) {
        memcpy(s_mPos, es.mPos, sizeof s_mPos); memcpy(s_mSize, es.mSize, sizeof s_mSize); memcpy(s_mValid, es.mValid, sizeof s_mValid);
    }
    if (es.mHave && es.mSerial == s_mkSerial && s_mkNew) {
        memcpy(s_pPos, es.mPos, sizeof s_pPos); memcpy(s_pSize, es.mSize, sizeof s_pSize); memcpy(s_pValid, es.mValid, sizeof s_pValid); s_pHave = true;
    }
    es.mHave = false;
    LeaveCriticalSection(&s_cs);
    for (int e = 0; e < 2; e++) {
        if (es.px[e].size() != (size_t)w * h) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(idxTex[e], 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            for (int y = 0; y < h; y++) memcpy((uint8_t*)m.pData + y * m.RowPitch, es.px[e].data() + y * w, w);
            ctx->Unmap(idxTex[e], 0);
        }
    }
    ctx->UpdateSubresource(palTex, 0, nullptr, es.pal, 1024, 0);
    float eb[2][32] = {};
    for (int e = 0; e < 2; e++) {
        EyeCh& c = es.ch[e];
        if (!c.on || !scrSrv) continue;
        int bw = c.bx1 - c.bx0 + 1, bh = c.by1 - c.by0 + 1;
        if (!occTex[e] || occW[e] < bw || occH[e] < bh) {
            if (occSrv[e]) occSrv[e]->Release(); if (occTex[e]) occTex[e]->Release(); occTex[e] = nullptr; occSrv[e] = nullptr;
            int nw = bw + 64, nh = bh + 64;
            D3D11_TEXTURE2D_DESC td = {(UINT)nw, (UINT)nh, 1, 1, DXGI_FORMAT_R8_UINT, {1, 0}, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE};
            if (FAILED(d3d->CreateTexture2D(&td, nullptr, &occTex[e]))) continue;
            d3d->CreateShaderResourceView(occTex[e], nullptr, &occSrv[e]); occW[e] = nw; occH[e] = nh;
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(occTex[e], 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) continue;
        for (int y = 0; y < bh; y++) memcpy((uint8_t*)m.pData + y * m.RowPitch, c.occ.data() + (size_t)y * bw, bw);
        ctx->Unmap(occTex[e], 0);
        float* f = eb[e];
        for (int r = 0; r < 3; r++) { f[r*4] = c.rows[r*3]; f[r*4+1] = c.rows[r*3+1]; f[r*4+2] = c.rows[r*3+2]; }
        f[12] = (float)w; f[13] = (float)h; f[14] = (w * 0.5f) / s_target.tanHalfH; f[15] = (h * 0.5f) / s_target.tanHalfV;
        f[16] = (float)c.bx0; f[17] = (float)c.by0; f[18] = (float)c.bx1; f[19] = (float)c.by1;
        f[20] = c.g[0]; f[21] = c.g[1]; f[22] = c.g[2]; f[23] = c.g[3];
        f[24] = s_uScale; f[25] = s_uOff; f[26] = s_keyThresh; f[27] = s_keyGain;
        f[28] = 1.0f;
    }
    shownViews[0] = es.views[0]; shownViews[1] = es.views[1];
    EnterCriticalSection(&s_cs); s_inUse = -1; LeaveCriticalSection(&s_cs);
    for (int e = 0; e < 2; e++) {
        ctx->UpdateSubresource(cbufEye[e], 0, nullptr, eb[e], 0, 0);
        ctx->PSSetConstantBuffers(1, 1, &cbufEye[e]);
        ID3D11ShaderResourceView* o = eb[e][28] > 0 ? occSrv[e] : nullptr;
        ctx->PSSetShaderResources(3, 1, &o);
        ID3D11ShaderResourceView* srvs[3] = {idxSrv[e], palSrv, eb[e][28] > 0 ? scrSrv : nullptr}; DrawTo(eyeSc[e], psIndex, srvs, 0);
        ID3D11ShaderResourceView* n1 = nullptr; ctx->PSSetShaderResources(3, 1, &n1);
    }
    haveEyeImage = true;
}

static void UpdateScreen(bool flight) {
    HANDLE h; uint32_t fr;
    EnterCriticalSection(&s_cs); h = s_screenHandle; fr = s_screenFrame; LeaveCriticalSection(&s_cs);
    if (!h) return;
    if (h != scrOpened) {
        if (scrSrv) { scrSrv->Release(); scrSrv = nullptr; } if (scrTex) { scrTex->Release(); scrTex = nullptr; }
        if (FAILED(d3d->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void**)&scrTex))) {
            Log("[vr] OpenSharedResource failed (p2den on another GPU?)"); scrOpened = h; return;
        }
        D3D11_TEXTURE2D_DESC td; scrTex->GetDesc(&td);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {}; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        sd.Format = (td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        d3d->CreateShaderResourceView(scrTex, &sd, &scrSrv);
        float aspect = (float)td.Width / td.Height;
        s_uScale = aspect > 4.0f / 3.0f ? (4.0f / 3.0f) / aspect : 1.0f; s_uOff = (1.0f - s_uScale) * 0.5f;
        scrOpened = h; Log("[vr] screen source %ux%u (crop u %.3f..%.3f)", td.Width, td.Height, s_uOff, s_uOff + s_uScale);
    }
    static bool lastFlight;
    if (!scrSrv || (fr == shownScreenFrame && flight == lastFlight)) return;
    shownScreenFrame = fr; lastFlight = flight;
    DrawHud(scrSrv, flight);
    haveScreen = true;
}

static void HandleEvents() {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED && sess) {   // which controllers SteamVR reports
            for (const char* hand : {"/user/hand/left", "/user/hand/right"}) {
                XrInteractionProfileState ps = {XR_TYPE_INTERACTION_PROFILE_STATE};
                char name[XR_MAX_PATH_LENGTH] = "none"; uint32_t n = 0;
                if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(sess, P(hand), &ps)) && ps.interactionProfile != XR_NULL_PATH)
                    xrPathToString(inst, ps.interactionProfile, sizeof name, &n, name);
                Log("[ctl] %s controller profile: %s", hand, name);
            }
        }
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            sessState = ((XrEventDataSessionStateChanged*)&ev)->state;
            Log("[vr] session state %d", (int)sessState);
            if (sessState == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO}; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(sess, &bi))) sessRunning = true;
            } else if (sessState == XR_SESSION_STATE_STOPPING) {
                xrEndSession(sess); sessRunning = false; InterlockedExchange(&s_active, 0);
            }
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

static void DoRecenter(XrTime t) {
    // New LOCAL space = runtime LOCAL offset by the current head yaw and position.
    XrSpace raw; XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rs.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    if (XR_FAILED(xrCreateReferenceSpace(sess, &rs, &raw))) return;
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(xrLocateSpace(viewSpace, raw, t, &loc))) {
        XrQuaternionf q = loc.pose.orientation;
        float yaw = atan2f(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.y * q.y + q.x * q.x));
        XrPosef p; p.orientation = {0, sinf(yaw / 2), 0, cosf(yaw / 2)}; p.position = loc.pose.position;
        CreateRecenteredLocal(&p);
        Log("[vr] recentered (yaw %.1f deg)", yaw * 57.29578f);
    }
    xrDestroySpace(raw);
}

static void FrameLoop() {
    bool targetDone = false;
    for (;;) {
        HandleEvents();
        if (!sessRunning) { Sleep(50); continue; }
        XrFrameWaitInfo fwi = {XR_TYPE_FRAME_WAIT_INFO}; XrFrameState fs = {XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(sess, &fwi, &fs))) { Sleep(10); continue; }
        XrFrameBeginInfo fbi = {XR_TYPE_FRAME_BEGIN_INFO}; xrBeginFrame(sess, &fbi);
        if (InterlockedExchange(&s_recenter, 0)) DoRecenter(fs.predictedDisplayTime);
        PollControls(sessState == XR_SESSION_STATE_FOCUSED);
        {   static bool maximized;
            if (s_maximize && !maximized) { HWND gw = GameWnd(); if (gw && IsWindowVisible(gw)) { ShowWindowAsync(gw, SW_MAXIMIZE); maximized = true; Log("[vr] game window maximized"); } } }
        ApplyHaptics(sessState == XR_SESSION_STATE_FOCUSED);

        XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; vli.displayTime = fs.predictedDisplayTime; vli.space = localSpace;
        XrViewState vs = {XR_TYPE_VIEW_STATE}; XrView xv[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}; uint32_t nv = 0;
        bool tracked = XR_SUCCEEDED(xrLocateViews(sess, &vli, &vs, 2, &nv, xv)) &&
                       (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        if (tracked && !targetDone) { targetDone = FinaliseEyeTarget(xv); }
        if (tracked) {
            EnterCriticalSection(&s_cs);
            for (int e = 0; e < 2; e++) {
                s_views[e].pos[0] = xv[e].pose.position.x; s_views[e].pos[1] = xv[e].pose.position.y; s_views[e].pos[2] = xv[e].pose.position.z;
                const XrQuaternionf& a = xv[e].pose.orientation; const float* b = s_offQuat[e];   // render = eye * offset
                s_views[e].rawQuat[0] = a.x; s_views[e].rawQuat[1] = a.y; s_views[e].rawQuat[2] = a.z; s_views[e].rawQuat[3] = a.w;
                s_views[e].quat[0] = a.w*b[0] + a.x*b[3] + a.y*b[2] - a.z*b[1];
                s_views[e].quat[1] = a.w*b[1] - a.x*b[2] + a.y*b[3] + a.z*b[0];
                s_views[e].quat[2] = a.w*b[2] + a.x*b[1] - a.y*b[0] + a.z*b[3];
                s_views[e].quat[3] = a.w*b[3] - a.x*b[0] - a.y*b[1] - a.z*b[2];
            }
            s_viewsValid = true;
            LeaveCriticalSection(&s_cs);
            InterlockedIncrement(&s_frameCounter);
        }
        InterlockedExchange(&s_active, targetDone ? 1 : 0);

        bool flight = targetDone && (GetTickCount() - s_lastEyeTick) < 300;
        if (s_retDist > 0) {
            float want = s_retWanted > 0 ? s_retWanted : s_retDist;
            if (want > s_retDist) want = s_retDist; if (want < s_retMin) want = s_retMin;
            float k = want < s_retNow ? 0.35f : 0.08f;          // per display frame, in log space
            s_retNow = expf(logf(s_retNow) + (logf(want) - logf(s_retNow)) * k);
        }
        std::vector<XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad reticle = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad beam = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad tip = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        bool toolOpen = targetDone && DesktopToolOpen();
        int msgSlot = toolOpen ? 0 : (targetDone && s_msgShow == 1) ? 1 : -1;
        if (targetDone && sessState == XR_SESSION_STATE_FOCUSED && msgSlot < 0) UpdatePointer(fs.predictedDisplayTime, flight, s_screenDist, s_screenWidth, 0.0f);
        else s_ptrHit = false;
        XrCompositionLayerQuad marks[MAX_BLOBS]; XrCompositionLayerQuad pq[8];
        if (fs.shouldRender && targetDone) {
            if (flight) { UploadEyes(); UpdateMarkers(); }
            UpdateScreen(flight);
            if (flight && haveEyeImage) {
                for (int e = 0; e < 2; e++) {
                    pv[e].pose.position = {shownViews[e].pos[0], shownViews[e].pos[1], shownViews[e].pos[2]};
                    pv[e].pose.orientation = {shownViews[e].quat[0], shownViews[e].quat[1], shownViews[e].quat[2], shownViews[e].quat[3]};
                    pv[e].fov = usedFov;
                    pv[e].subImage.swapchain = eyeSc[e].sc;
                    pv[e].subImage.imageRect = {{0, 0}, {eyeSc[e].w, eyeSc[e].h}};
                }
                proj.space = localSpace; proj.viewCount = 2; proj.views = pv;
                layers.push_back((XrCompositionLayerBaseHeader*)&proj);
                // Each HUD marker is its own quad, placed in 3D where the game drew it (direction from the ship
                // centre) at the distance of the object it marks, so the eyes fuse marker and target.
                if (haveMarkers) {
                    float mPos[MAX_BLOBS][3], mSize[MAX_BLOBS][2]; uint8_t mValid[MAX_BLOBS] = {};
                    EnterCriticalSection(&s_cs);
                    memcpy(mPos, s_mPos, sizeof mPos); memcpy(mSize, s_mSize, sizeof mSize); memcpy(mValid, s_mValid, sizeof mValid);
                    LeaveCriticalSection(&s_cs);
                    const float K = 461.0357f, sc = (float)markSc.w / (s_mkW ? s_mkW : 640);
                    float inf = s_retDist > 0 ? s_retDist : 500.0f;
                    int budget = s_maxLayers - 3 - (s_panelsOn ? s_nPanels : 0);   // projection, HUD, crosshair, panels
                    for (int b = 0; b < s_nShownBlobs; b++) {
                        if ((int)layers.size() - 1 >= budget) break;
                        const VrMarkerBlob& bl = s_shownBlobs[b];
                        int ox = bl.layer * 640;                                           // which third of the texture
                        int x0 = (int)((bl.x0 + ox) * sc) - 2, y0 = (int)(bl.y0 * sc) - 2, x1 = (int)((bl.x1 + 1 + ox) * sc) + 2, y1 = (int)((bl.y1 + 1) * sc) + 2;
                        int lo = (int)(ox * sc), hi = (int)((ox + 640) * sc);
                        if (x0 < lo) x0 = lo; if (y0 < 0) y0 = 0; if (x1 > hi) x1 = hi; if (y1 > markSc.h) y1 = markSc.h;
                        if (x1 <= x0 || y1 <= y0) continue;
                        float d = bl.depthM > 0 ? bl.depthM : inf;
                        float cx = (x0 + x1) * 0.5f / sc - ox, cy = (y0 + y1) * 0.5f / sc;     // centre in 640x480 pixels
                        XrCompositionLayerQuad& q = marks[b]; q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                        q.space = localSpace; q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                        q.subImage.swapchain = markSc.sc; q.subImage.imageRect = {{x0, y0}, {x1 - x0, y1 - y0}};
                        q.size = {(x1 - x0) / sc / K * d, (y1 - y0) / sc / K * d};
                        q.pose = {{0, 0, 0, 1}, {(cx - 320.f) / K * d, -(cy - 240.f) / K * d, -d - s_observerBack}};
                        if (bl.depthM == -2.0f) {   // not tied to an object (e.g. direction arrows): on the HUD glass
                            float hd = s_hudDist;
                            q.size = {(x1 - x0) / sc / K * hd, (y1 - y0) / sc / K * hd};
                            q.pose = {{0, 0, 0, 1}, {(cx - 320.f) / K * hd, -(cy - 240.f) / K * hd + s_hudY, -hd}};
                        }
                        if (bl.layer >= 2 && !mValid[b]) continue;          // cached octagon / lock reticle only exist attached to the target
                        if (mValid[b]) {   // follows its object in the same (blended) state as the rendered eyes
                            q.pose.position = {mPos[b][0], mPos[b][1], mPos[b][2]};
                            // face the viewer (works all around, not only in front)
                            float nx = -mPos[b][0], ny = -mPos[b][1], nz = -mPos[b][2], nl = sqrtf(nx*nx + ny*ny + nz*nz);
                            if (nl > 1e-4f) { nx /= nl; ny /= nl; nz /= nl;
                                // shortest rotation from +Z (quad normal) to n
                                float qx = -ny, qy = nx, qz = 0, qw = 1 + nz, ql = sqrtf(qx*qx + qy*qy + qz*qz + qw*qw);
                                if (ql > 1e-4f) q.pose.orientation = {qx / ql, qy / ql, qz / ql, qw / ql}; else q.pose.orientation = {0, 1, 0, 0}; }
                            q.size = {mSize[b][0] * ((x1 - x0) / sc) / (bl.x1 - bl.x0 + 1 + (bl.layer ? 0 : 0)), mSize[b][1] * ((y1 - y0) / sc) / (bl.y1 - bl.y0 + 1)};
                        }
                        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                        layers.push_back((XrCompositionLayerBaseHeader*)&q);
                    }
                }
            }
            if (haveScreen) {
                quad.space = localSpace; quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = quadSc.sc; quad.subImage.imageRect = {{0, 0}, {HUD_PX_W, HUD_PX_H}};
                float wdt = flight ? s_hudWidth : s_screenWidth;
                quad.size = {wdt, wdt * 0.75f};
                quad.pose = {{0, 0, 0, 1}, {0, flight ? s_hudY : 0.0f, -(flight ? s_hudDist : s_screenDist)}};
                if (flight) quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                layers.push_back((XrCompositionLayerBaseHeader*)&quad);
                if (flight && s_panelsOn && havePanels) {
                    const float px = (float)HUD_PX_W / 640.f;
                    for (int k = 0; k < s_nPanels; k++) {
                        const HudPanel& hp = s_panels[k]; XrCompositionLayerQuad& q = pq[k]; q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                        int x0 = (int)(hp.r[0] * px), y0 = (int)(hp.r[1] * px), x1 = (int)(hp.r[2] * px), y1 = (int)(hp.r[3] * px);
                        q.space = localSpace; q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                        q.subImage.swapchain = panelSc.sc; q.subImage.imageRect = {{x0, y0}, {x1 - x0, y1 - y0}};
                        q.size = {hp.width, hp.width * (hp.r[3] - hp.r[1]) / (hp.r[2] - hp.r[0])};
                        // yaw about Y (+ = face turned right), then tilt about X (+ = face turned up)
                        float a = hp.yaw * 0.0087266f, b = -hp.pitch * 0.0087266f;
                        float sy = sinf(a), cy = cosf(a), sx = sinf(b), cx = cosf(b);
                        q.pose = {{cy * sx, sy * cx, -sy * sx, cy * cx}, {hp.pos[0], hp.pos[1], hp.pos[2]}};
                        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                        layers.push_back((XrCompositionLayerBaseHeader*)&q);
                    }
                }
                if (flight && s_retDist > 0 && !s_retInEyes && !s_retGlass) {   // crosshair on the boresight, same apparent size as on the HUD panel
                    reticle.space = localSpace; reticle.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    reticle.subImage.swapchain = quadSc.sc; reticle.subImage.imageRect = {{HUD_PX_W, 0}, {s_retPx, s_retPx}};
                    float side = 2 * s_retNow * GAME_TAN_H * ((float)s_retPx / HUD_PX_W);
                    reticle.size = {side, side};
                    reticle.pose = {{0, 0, 0, 1}, {0, 0, -s_retNow - s_observerBack}};
                    reticle.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    layers.push_back((XrCompositionLayerBaseHeader*)&reticle);
                }
            }
        }
        if (!flight && haveScreen && targetDone && sessState == XR_SESSION_STATE_FOCUSED && fs.shouldRender) {
            XrVector3f head = {(s_views[0].pos[0] + s_views[1].pos[0]) * 0.5f, (s_views[0].pos[1] + s_views[1].pos[1]) * 0.5f, (s_views[0].pos[2] + s_views[1].pos[2]) * 0.5f};
            if (BeamLayer(beam, head)) layers.push_back((XrCompositionLayerBaseHeader*)&beam);
            if (s_ptrHit && beamReady) {   // aiming dot where the beam meets the screen (exact click point, any cursor graphic)
                tip.space = localSpace; tip.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                tip.subImage.swapchain = beamSc.sc; tip.subImage.imageRect = {{0, 0}, {beamSc.w, beamSc.h}};
                tip.size = {0.012f, 0.012f};
                tip.pose = {{0, 0, 0, 1}, {s_ptrTo.x, s_ptrTo.y, s_ptrTo.z + 0.005f}};
                tip.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                layers.push_back((XrCompositionLayerBaseHeader*)&tip);
            }
        }
        XrCompositionLayerQuad msg = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (msgSlot >= 0 && msgReady[msgSlot]) {   // game frozen (desktop tool) or waiting (entering space): world-fixed message only
            layers.clear();
            msg.space = localSpace; msg.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            msg.subImage.swapchain = msgSc[msgSlot].sc; msg.subImage.imageRect = {{0, 0}, {MSG_W, MSG_H}};
            msg.size = {1.3f, 1.3f * MSG_H / MSG_W}; msg.pose = {{0, 0, 0, 1}, {0, 0, -1.5f}};
            layers.push_back((XrCompositionLayerBaseHeader*)&msg);
        }
        XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime; fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = (uint32_t)layers.size(); fei.layers = layers.data();
        xrEndFrame(sess, &fei);
    }
}

static DWORD WINAPI VrThread(LPVOID) {
    for (int attempt = 0;; attempt++) {
        if (InitXr()) break;
        if (inst) { xrDestroyInstance(inst); inst = XR_NULL_HANDLE; sess = XR_NULL_HANDLE; localSpace = viewSpace = XR_NULL_HANDLE; }
        if (attempt == 0) Log("[vr] OpenXR not available yet - retrying every 5 s (is SteamVR running and the headset connected?)");
        Sleep(5000);
    }
    Log("[vr] OpenXR session created");
    FrameLoop();
    return 0;
}

static float IniF(const char* ini, const char* key, float def) {
    char b[32], d[32]; sprintf_s(d, "%g", def);
    GetPrivateProfileStringA("VR", key, d, b, 32, ini); return (float)atof(b);
}

void Vr_Start(const char* ini, LogFn log) {
    Log = log;
    InitializeCriticalSection(&s_cs); InitializeCriticalSection(&s_ctlCs);
    s_deadzone = IniF(ini, "STICK_DEADZONE", 0.15f);
    s_eyeScale = IniF(ini, "EYE_SCALE", 1.0f);
    s_eyeMax = (int)IniF(ini, "EYE_MAX", 2048.0f); if (s_eyeMax < 512) s_eyeMax = 512; if (s_eyeMax > 8192) s_eyeMax = 8192;
    s_hudDist = IniF(ini, "HUD_DIST", 1.0f); s_hudWidth = IniF(ini, "HUD_WIDTH", 0.0f); s_hudY = IniF(ini, "HUD_Y", 0.0f);
    if (s_hudWidth <= 0) s_hudWidth = 2 * s_hudDist * GAME_TAN_H;   // auto: exactly the game's field of view
    s_retInEyes = IniF(ini, "RETICLE_IN_EYES", 1.0f) != 0;
    s_retGlass = IniF(ini, "RETICLE_GLASS", 1.0f) != 0; s_glassAlpha = IniF(ini, "RETICLE_GLASS_ALPHA", 0.7f);
    s_retDist = IniF(ini, "RETICLE_DIST", 30.0f); s_retMin = IniF(ini, "RETICLE_MIN_DIST", 1.5f); s_retNow = s_retDist; s_retFrac = IniF(ini, "RETICLE_SIZE", 0.20f);
    s_screenDist = IniF(ini, "SCREEN_DIST", 2.5f); s_screenWidth = IniF(ini, "SCREEN_WIDTH", 3.2f);
    s_keyGain = IniF(ini, "HUD_KEY_GAIN", 40.0f); s_keyThresh = IniF(ini, "HUD_KEY_THRESHOLD", 0.03f);
    // HUD panels on a sphere around the seated head. PANEL_<NAME> = x0,y0,x1,y1 (HUD pixels, 640x480), azimuth (deg, + right),
    // elevation (deg, + up), width (m) [, radius (m)]. Each panel faces the centre of the sphere.
    s_panelsOn = IniF(ini, "HUD_PANELS", 1.0f) != 0;
    s_maximize = IniF(ini, "WINDOW_MAXIMIZE", 1.0f) != 0;
    float radius = IniF(ini, "PANEL_RADIUS", 0.8f);
    static const char* kPanelDefaults[][2] = {
        {"VELOCITY",       "236,4,406,58,     0, 24, 0.36"},
        {"COMMS",          "0,0,124,140,    -34, 22, 0.24"},   // windows only: the direction arrows circle the crosshair at ~100 px
        {"TARGET",         "2,342,120,478,  -44,-22, 0.28"},
        {"TARGET_SHIELDS", "138,382,254,476,-22,-26, 0.24"},
        {"RADAR",          "254,346,388,478,  0,-28, 0.30"},
        {"SHIELDS",        "388,382,504,476, 22,-26, 0.24"},
        {"RIGHT",          "504,342,640,478, 44,-22, 0.28"},
        {"MISSILES",       "516,0,640,140,   34, 22, 0.24"}};   // top right: missile / gun VDWs (max 8 panels)
    s_nPanels = 0;
    for (auto& d : kPanelDefaults) {
        char key[32], b[160]; sprintf_s(key, "PANEL_%s", d[0]);
        GetPrivateProfileStringA("VR", key, d[1], b, sizeof b, ini);
        HudPanel hp = {}; strcpy_s(hp.name, d[0]);
        float az = 0, el = 0, r = radius;
        int n = sscanf_s(b, "%f , %f , %f , %f , %f , %f , %f , %f", &hp.r[0], &hp.r[1], &hp.r[2], &hp.r[3], &az, &el, &hp.width, &r);
        if (n < 7) { if (strcmp(b, "0") && *b) Log("[vr] %s ignored (need 7 or 8 numbers): %s", key, b); continue; }
        if (hp.r[2] <= hp.r[0] || hp.r[3] <= hp.r[1] || hp.width <= 0 || r <= 0 || s_nPanels >= 8) { Log("[vr] %s ignored (bad rectangle/width/radius)", key); continue; }
        float ra = az * 0.0174533f, re = el * 0.0174533f;
        hp.pos[0] = r * sinf(ra) * cosf(re); hp.pos[1] = r * sinf(re); hp.pos[2] = -r * cosf(ra) * cosf(re);
        hp.yaw = -az; hp.pitch = -el;                                  // face the centre
        s_panels[s_nPanels++] = hp;
    }
    Log("[vr] HUD panels %s (%d)", s_panelsOn ? "on" : "off", s_nPanels);
    CreateThread(nullptr, 0, VrThread, nullptr, 0, nullptr);
}
