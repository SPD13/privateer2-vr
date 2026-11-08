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
static float s_eyeScale = 1.0f;      // multiplier on the runtime's recommended size
static int   s_eyeMax = 2048;        // largest eye dimension (BRender's own check is 2048; p2vr patches it out above that)
static float s_offQuat[2][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}};   // per-eye rotation to the centre of its (asymmetric) fov
static float s_hudDist = 1.0f, s_hudWidth = 1.5f, s_hudY = 0.0f;
// Crosshair region of the HUD, shown as its own layer far out on the boresight so it lines up with the lasers.
static float s_retDist = 30.0f;      // metres (game units = metres * WORLD_SCALE)
static float s_retFrac = 0.16f;      // size of the crosshair region as a fraction of the HUD width
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

// eye buffers: [set][eye]; game writes s_back, VR thread uploads s_ready
struct EyeSet { std::vector<uint8_t> px[2]; VrView views[2]; uint8_t pal[1024]; };
static EyeSet   s_sets[3];
static int      s_back = 0, s_ready = -1, s_inUse = -1;
static DWORD    s_lastEyeTick;

// HUD markers (8-bit) from the game thread
static std::vector<uint8_t> s_mk; static int s_mkW, s_mkH; static uint8_t s_mkPal[1024]; static bool s_mkNew;
static HANDLE   s_screenHandle; static int s_screenW, s_screenH; static uint32_t s_screenFrame;

// ---------------------------------------------------------------- public API (game thread side)
bool Vr_Active() { return s_active != 0; }
LUID Vr_AdapterLuid() { return s_luid; }
void Vr_RequestRecenter() { InterlockedExchange(&s_recenter, 1); }

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
void Vr_SubmitEyes(const VrView used[2], const uint8_t pal[1024]) {
    EnterCriticalSection(&s_cs);
    EyeSet& s = s_sets[s_back];
    s.views[0] = used[0]; s.views[1] = used[1]; memcpy(s.pal, pal, 1024);
    s_ready = s_back;
    for (int i = 0; i < 3; i++) if (i != s_ready && i != s_inUse) { s_back = i; break; }
    s_lastEyeTick = GetTickCount();
    LeaveCriticalSection(&s_cs);
}
void Vr_SubmitMarkers(const uint8_t* idx, int w, int h, const uint8_t pal[1024]) {
    EnterCriticalSection(&s_cs);
    s_mk.assign(idx, idx + (size_t)w * h); s_mkW = w; s_mkH = h; memcpy(s_mkPal, pal, 1024); s_mkNew = true;
    LeaveCriticalSection(&s_cs);
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
static ID3D11SamplerState* linSamp; static ID3D11Buffer* cbuf;
static ID3D11Texture2D* scrTex; static ID3D11ShaderResourceView* scrSrv; static HANDLE scrOpened;

static const char* kShaders = R"(
Texture2D<uint>   idx : register(t0);
Texture2D<float4> pal : register(t1);
Texture2D<float4> scr : register(t2);
SamplerState      ls  : register(s0);
cbuffer C : register(b0) { float keyGain; float keyOn; float uScale; float uOff; float keyThresh; float holeOn; float2 pad2;
                           float4 map; float4 hole; };   // map: hud uv = map.xy + uv * map.zw ; hole: u0 v0 u1 v1
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o; o.uv = float2((id << 1) & 2, id & 2); o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
float4 psIndex(V i) : SV_Target {
    uint c = idx.Load(int3(i.p.xy, 0));
    return float4(pal.Load(int3(c, 0, 0)).rgb, 1);
}
float4 psMarker(V i) : SV_Target {   // map.xy = index texture size; index 0 = transparent
    uint c = idx.Load(int3(min(int2(i.uv * map.xy), int2(map.xy) - 1), 0));
    if (c == 0) return 0;
    return float4(pal.Load(int3(c, 0, 0)).rgb, 1);
}
float4 psScreen(V i) : SV_Target {
    float2 uv = map.xy + i.uv * map.zw;
    if (holeOn > 0.5 && uv.x > hole.x && uv.x < hole.z && uv.y > hole.y && uv.y < hole.w) return 0;
    float3 c = scr.Sample(ls, float2(uOff + uv.x * uScale, uv.y)).rgb;
    // key on display (gamma) brightness: anything clearly above black is fully opaque
    float g = pow(max(c.r, max(c.g, c.b)), 1.0 / 2.2);
    float a = keyOn > 0.5 ? saturate((g - keyThresh) * keyGain) : 1.0;
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
static Swap eyeSc[2], quadSc, markSc;
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

static bool InitXr() {
    // SteamVR does not register its 32-bit runtime as active; point the loader at it.
    char buf[MAX_PATH];
    if (!GetEnvironmentVariableA("XR_RUNTIME_JSON", buf, MAX_PATH)) {
        const char* def = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\steamxr_win32.json";
        if (GetFileAttributesA(def) != INVALID_FILE_ATTRIBUTES) SetEnvironmentVariableA("XR_RUNTIME_JSON", def);
    }
    const char* exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "Privateer 2 VR");
    strcpy_s(ici.applicationInfo.engineName, "BRender");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1; ici.enabledExtensionNames = exts;
    XRCHECK(xrCreateInstance(&ici, &inst));
    XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO}; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XRCHECK(xrGetSystem(inst, &sgi, &sysId));
    XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES}; xrGetSystemProperties(inst, sysId, &sp);
    Log("[vr] system: %s", sp.systemName);

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
    if (!MakeSwap(markSc, 1280, 960)) return false;                        // HUD markers, 2x the game resolution
    EnterCriticalSection(&s_cs);
    s_target.width = w; s_target.height = h; s_target.tanHalfH = tH; s_target.tanHalfV = tV; s_targetValid = true;
    LeaveCriticalSection(&s_cs);
    return true;
}

static float s_uScale = 1.0f, s_uOff = 0.0f;   // crop of the (pillarboxed) p2den image to its 4:3 centre
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
    float hw = s_retPx / (2.0f * HUD_PX_W), hh = s_retPx / (2.0f * HUD_PX_H);   // crosshair region half-size in HUD uv
    // pass 1: whole HUD (hole over the crosshair region in flight)
    float cb1[16] = {s_keyGain, flight ? 1.f : 0.f, s_uScale, s_uOff, s_keyThresh, flight ? 1.f : 0.f, 0, 0,
                     0, 0, 1, 1, 0.5f - hw, 0.5f - hh, 0.5f + hw, 0.5f + hh};
    ctx->UpdateSubresource(cbuf, 0, nullptr, cb1, 0, 0);
    D3D11_VIEWPORT vp1 = {0, 0, (float)HUD_PX_W, (float)HUD_PX_H, 0, 1}; ctx->RSSetViewports(1, &vp1); ctx->Draw(3, 0);
    if (flight) {   // pass 2: crosshair crop
        float cb2[16] = {s_keyGain, 1.f, s_uScale, s_uOff, s_keyThresh, 0, 0, 0,
                         0.5f - hw, 0.5f - hh, 2 * hw, 2 * hh, 0, 0, 0, 0};
        ctx->UpdateSubresource(cbuf, 0, nullptr, cb2, 0, 0);
        D3D11_VIEWPORT vp2 = {(float)HUD_PX_W, 0, (float)s_retPx, (float)s_retPx, 0, 1}; ctx->RSSetViewports(1, &vp2); ctx->Draw(3, 0);
    }
    ID3D11ShaderResourceView* nulls[3] = {}; ctx->PSSetShaderResources(0, 3, nulls);
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(s.sc, &ri);
}

static void UpdateMarkers() {
    EnterCriticalSection(&s_cs);
    if (!s_mkNew) { LeaveCriticalSection(&s_cs); return; }
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
    for (int e = 0; e < 2; e++) {
        if (es.px[e].size() != (size_t)w * h) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(idxTex[e], 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            for (int y = 0; y < h; y++) memcpy((uint8_t*)m.pData + y * m.RowPitch, es.px[e].data() + y * w, w);
            ctx->Unmap(idxTex[e], 0);
        }
    }
    ctx->UpdateSubresource(palTex, 0, nullptr, es.pal, 1024, 0);
    shownViews[0] = es.views[0]; shownViews[1] = es.views[1];
    EnterCriticalSection(&s_cs); s_inUse = -1; LeaveCriticalSection(&s_cs);
    for (int e = 0; e < 2; e++) { ID3D11ShaderResourceView* srvs[3] = {idxSrv[e], palSrv, nullptr}; DrawTo(eyeSc[e], psIndex, srvs, 0); }
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
        }
        InterlockedExchange(&s_active, targetDone ? 1 : 0);

        bool flight = targetDone && (GetTickCount() - s_lastEyeTick) < 300;
        std::vector<XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad reticle = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad marks = {XR_TYPE_COMPOSITION_LAYER_QUAD};
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
                if (haveMarkers) {
                    marks.space = localSpace; marks.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    marks.subImage.swapchain = markSc.sc; marks.subImage.imageRect = {{0, 0}, {markSc.w, markSc.h}};
                    marks.size = {2 * s_retDist * GAME_TAN_H, 2 * s_retDist * GAME_TAN_V};
                    marks.pose = {{0, 0, 0, 1}, {0, 0, -s_retDist}};
                    marks.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    layers.push_back((XrCompositionLayerBaseHeader*)&marks);
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
                if (flight) {   // crosshair on the boresight, same apparent size as on the HUD panel
                    reticle.space = localSpace; reticle.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    reticle.subImage.swapchain = quadSc.sc; reticle.subImage.imageRect = {{HUD_PX_W, 0}, {s_retPx, s_retPx}};
                    float side = 2 * s_retDist * GAME_TAN_H * ((float)s_retPx / HUD_PX_W);
                    reticle.size = {side, side};
                    reticle.pose = {{0, 0, 0, 1}, {0, 0, -s_retDist}};
                    reticle.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    layers.push_back((XrCompositionLayerBaseHeader*)&reticle);
                }
            }
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
    InitializeCriticalSection(&s_cs);
    s_eyeScale = IniF(ini, "EYE_SCALE", 1.0f);
    s_eyeMax = (int)IniF(ini, "EYE_MAX", 2048.0f); if (s_eyeMax < 512) s_eyeMax = 512; if (s_eyeMax > 8192) s_eyeMax = 8192;
    s_hudDist = IniF(ini, "HUD_DIST", 1.0f); s_hudWidth = IniF(ini, "HUD_WIDTH", 1.5f); s_hudY = IniF(ini, "HUD_Y", 0.0f);
    s_retDist = IniF(ini, "RETICLE_DIST", 30.0f); s_retFrac = IniF(ini, "RETICLE_SIZE", 0.16f);
    s_screenDist = IniF(ini, "SCREEN_DIST", 2.5f); s_screenWidth = IniF(ini, "SCREEN_WIDTH", 3.2f);
    s_keyGain = IniF(ini, "HUD_KEY_GAIN", 40.0f); s_keyThresh = IniF(ini, "HUD_KEY_THRESHOLD", 0.03f);
    CreateThread(nullptr, 0, VrThread, nullptr, 0, nullptr);
}
