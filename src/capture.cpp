// capture.cpp - grabs p2den.dll's final D3D11 frame (HUD, menus, FMV) into a shared texture.
// p2den imports CreateDXGIFactory; we patch that import, then hook CreateSwapChain(ForHwnd) on the
// returned factory and Present on the created swapchain.
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <vector>
#include <dxgi1_2.h>
#include <stdint.h>
#include "vr.h"
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3dcompiler.lib")

static LogFn Log;
static ID3D11Texture2D* mPalExclude;   // our own mirror palette texture (excluded from palette capture)
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory)(REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain*, UINT, UINT);

static IUnknown* gP2denDevice;
static PFN_CreateDXGIFactory    oCreateFactory;
static PFN_CreateSwapChain      oCreateSwapChain;
static PFN_CreateSwapChainForHwnd oCreateSwapChainForHwnd;
static PFN_Present              oPresent;
static IDXGISwapChain*          gSwap;
typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static PFN_D3D11CreateDevice    oD3D11CreateDevice;

static ID3D11Texture2D* gShared; static HANDLE gSharedHandle; static UINT gW, gH; static uint32_t gFrame;

// ---------------------------------------------------------------- monitor mirror
// In flight the game buffer is cleared after 3D (so p2den/VR get only the HUD). For the monitor we draw the
// game's own mono scene (snapshotted before the clear) underneath the HUD, on p2den's device, in an isolated
// context state so p2den's pipeline is untouched.
static CRITICAL_SECTION gMirCs; static std::vector<uint8_t> gMir; static int gMirW, gMirH; static uint8_t gMirPal[1024];
static volatile DWORD gMirTick; static bool gMirDirty;
static float gKeyThresh = 0.03f, gKeyGain = 40.f;

void Capture_SetMirror(const uint8_t* scene, int w, int h, const uint8_t* pal) {
    EnterCriticalSection(&gMirCs);
    gMir.assign(scene, scene + (size_t)w * h); gMirW = w; gMirH = h; memcpy(gMirPal, pal, 1024);
    gMirDirty = true; gMirTick = GetTickCount();
    LeaveCriticalSection(&gMirCs);
}

static ID3D11Device* mDev; static ID3DDeviceContextState* mState;
static ID3D11VertexShader* mVs; static ID3D11PixelShader* mPsScene; static ID3D11PixelShader* mPsOver;
static ID3D11Texture2D* mIdx; static ID3D11ShaderResourceView* mIdxSrv; static int mIdxW, mIdxH;
static ID3D11Texture2D* mPal; static ID3D11ShaderResourceView* mPalSrv;
static ID3D11ShaderResourceView* mSharedSrv; static ID3D11Texture2D* mSharedFor;
static ID3D11BlendState* mBlend; static ID3D11SamplerState* mSamp; static ID3D11Buffer* mCb;
static bool mFailed;

static const char* kMirrorHlsl = R"(
Texture2D<uint>   idx : register(t0);
Texture2D<float4> pal : register(t1);
Texture2D<float4> hud : register(t2);
SamplerState      ss  : register(s0);
cbuffer C : register(b0) { float2 idxSize; float keyThresh; float keyGain; };
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o; o.uv = float2((id << 1) & 2, id & 2); o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
float4 psScene(V i) : SV_Target {
    int2 t = min(int2(i.uv * idxSize), int2(idxSize) - 1);
    return float4(pal.Load(int3(idx.Load(int3(t, 0)), 0, 0)).rgb, 1);
}
float4 psOver(V i) : SV_Target {
    float3 c = hud.Sample(ss, i.uv).rgb;
    float a = saturate((max(c.r, max(c.g, c.b)) - keyThresh) * keyGain);
    return float4(c * a, a);
}
)";

static bool MirrorInit(ID3D11Device* dev) {
    if (mDev == dev) return !mFailed;
    mDev = dev; mFailed = true;
    ID3D11Device1* d1 = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&d1))) { Log("[mirror] no ID3D11Device1"); return false; }
    D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
    HRESULT hr = d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &mState);
    d1->Release();
    if (FAILED(hr)) { Log("[mirror] CreateDeviceContextState failed %08x", hr); return false; }
    auto comp = [&](const char* e, const char* prof) -> ID3DBlob* {
        ID3DBlob *b = nullptr, *err = nullptr;
        if (FAILED(D3DCompile(kMirrorHlsl, strlen(kMirrorHlsl), "mirror", 0, 0, e, prof, 0, 0, &b, &err))) {
            Log("[mirror] shader %s: %s", e, err ? (char*)err->GetBufferPointer() : "?"); return nullptr; }
        return b;
    };
    ID3DBlob* b;
    if (!(b = comp("vs", "vs_4_0"))) return false; dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), 0, &mVs); b->Release();
    if (!(b = comp("psScene", "ps_4_0"))) return false; dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), 0, &mPsScene); b->Release();
    if (!(b = comp("psOver", "ps_4_0"))) return false; dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), 0, &mPsOver); b->Release();
    D3D11_TEXTURE2D_DESC td = {256, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    dev->CreateTexture2D(&td, 0, &mPal); dev->CreateShaderResourceView(mPal, 0, &mPalSrv); mPalExclude = mPal;
    D3D11_BLEND_DESC bd = {}; auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE; rt.SrcBlend = D3D11_BLEND_ONE; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_ZERO; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bd, &mBlend);
    D3D11_SAMPLER_DESC sd = {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &mSamp);
    D3D11_BUFFER_DESC cb = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    dev->CreateBuffer(&cb, 0, &mCb);
    mFailed = false; Log("[mirror] monitor compositor ready");
    return true;
}

static void ComposeMirror(ID3D11Device* dev, ID3D11DeviceContext* c, ID3D11Texture2D* bb, const D3D11_TEXTURE2D_DESC& d) {
    if (GetTickCount() - gMirTick > 300 || !gShared) return;   // not in flight
    if (!MirrorInit(dev)) return;
    ID3D11DeviceContext1* c1 = nullptr;
    if (FAILED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1))) return;
    ID3DDeviceContextState* prev = nullptr;
    c1->SwapDeviceContextState(mState, &prev);

    EnterCriticalSection(&gMirCs);
    if (!mIdx || mIdxW != gMirW || mIdxH != gMirH) {
        if (mIdxSrv) mIdxSrv->Release(); if (mIdx) mIdx->Release(); mIdx = 0; mIdxSrv = 0;
        D3D11_TEXTURE2D_DESC td = {(UINT)gMirW, (UINT)gMirH, 1, 1, DXGI_FORMAT_R8_UINT, {1, 0}, D3D11_USAGE_DYNAMIC,
                                   D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE};
        if (SUCCEEDED(dev->CreateTexture2D(&td, 0, &mIdx))) dev->CreateShaderResourceView(mIdx, 0, &mIdxSrv);
        mIdxW = gMirW; mIdxH = gMirH; gMirDirty = true;
    }
    if (gMirDirty && mIdx) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(c1->Map(mIdx, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            for (int y = 0; y < gMirH; y++) memcpy((uint8_t*)m.pData + y * m.RowPitch, &gMir[(size_t)y * gMirW], gMirW);
            c1->Unmap(mIdx, 0);
        }
        c1->UpdateSubresource(mPal, 0, 0, gMirPal, 1024, 0);
        gMirDirty = false;
    }
    LeaveCriticalSection(&gMirCs);

    if (mSharedFor != gShared) {
        if (mSharedSrv) mSharedSrv->Release(); mSharedSrv = 0;
        D3D11_TEXTURE2D_DESC sdsc; gShared->GetDesc(&sdsc);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        sv.Format = sdsc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        dev->CreateShaderResourceView(gShared, &sv, &mSharedSrv);
        mSharedFor = gShared;
    }
    ID3D11RenderTargetView* rtv = nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = d.Format; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (mIdxSrv && mSharedSrv && SUCCEEDED(dev->CreateRenderTargetView(bb, &rd, &rtv))) {
        float cb[4] = {(float)mIdxW, (float)mIdxH, gKeyThresh, gKeyGain};
        c1->UpdateSubresource(mCb, 0, 0, cb, 0, 0);
        c1->OMSetRenderTargets(1, &rtv, 0);
        c1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c1->VSSetShader(mVs, 0, 0); c1->PSSetConstantBuffers(0, 1, &mCb); c1->PSSetSamplers(0, 1, &mSamp);
        // 1) mono scene into the centred 4:3 game area
        float W = (float)d.Width, H = (float)d.Height, vw = H * 4.f / 3.f, vh = H;
        if (vw > W) { vw = W; vh = W * 3.f / 4.f; }
        D3D11_VIEWPORT vp = {(W - vw) * 0.5f, (H - vh) * 0.5f, vw, vh, 0, 1};
        ID3D11ShaderResourceView* s1[2] = {mIdxSrv, mPalSrv};
        c1->RSSetViewports(1, &vp); c1->PSSetShaderResources(0, 2, s1); c1->PSSetShader(mPsScene, 0, 0);
        c1->OMSetBlendState(nullptr, nullptr, 0xffffffff); c1->Draw(3, 0);
        // 2) the HUD-only frame (copied to gShared just before) keyed on top
        D3D11_VIEWPORT full = {0, 0, W, H, 0, 1};
        c1->RSSetViewports(1, &full); c1->PSSetShaderResources(2, 1, &mSharedSrv); c1->PSSetShader(mPsOver, 0, 0);
        c1->OMSetBlendState(mBlend, nullptr, 0xffffffff); c1->Draw(3, 0);
        ID3D11ShaderResourceView* nulls[3] = {}; c1->PSSetShaderResources(0, 3, nulls);
        rtv->Release();
    }
    c1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
    c1->Release();
}

// ---------------------------------------------------------------- p2den palette capture
// p2den converts the 8-bit game image with a 256-entry palette texture; capture what it uploads (ground truth).
static volatile LONG gPalValid; static uint8_t gPal[1024]; static CRITICAL_SECTION gPalCs;
static ID3D11Texture2D* gPalTex; static bool gPalLogged;
typedef HRESULT (STDMETHODCALLTYPE *PFN_Map)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void (STDMETHODCALLTYPE *PFN_Unmap)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef void (STDMETHODCALLTYPE *PFN_UpdateSub)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
static PFN_Map oMap; static PFN_Unmap oUnmap; static PFN_UpdateSub oUpdateSub;
static void* gPalMapped; static ID3D11Resource* gPalMappedRes; static UINT gPalMappedPitch;


static void DiagResource(ID3D11Resource* r, const char* via) {
    static int n; static uint32_t seen[64];
    if (!r || n >= 40) return;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    uint32_t key = 0; char desc[96] = "";
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d);
        key = (d.Width << 20) ^ (d.Height << 8) ^ d.Format ^ (d.Usage << 30);
        sprintf_s(desc, "tex2d %ux%u fmt %d usage %d", d.Width, d.Height, d.Format, d.Usage);
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d; ((ID3D11Buffer*)r)->GetDesc(&d);
        key = 0x80000000u ^ d.ByteWidth ^ (d.BindFlags << 24);
        sprintf_s(desc, "buffer %u bytes bind %x", d.ByteWidth, d.BindFlags);
    } else return;
    for (int i = 0; i < n; i++) if (seen[i] == key) return;
    seen[n++] = key;
    Log("[pal-diag] %s: %s", via, desc);
}
static bool IsPaletteTex(ID3D11Resource* r, DXGI_FORMAT* fmt) {
    if (!r || r == (ID3D11Resource*)mPalExclude) return false;
    DiagResource(r, "write");
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d);
    if (d.Width * d.Height != 256) return false;
    if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        d.Format != DXGI_FORMAT_B8G8R8X8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return false;
    ID3D11Device* dev = nullptr; r->GetDevice(&dev);
    IUnknown* id = nullptr; if (dev) { dev->QueryInterface(__uuidof(IUnknown), (void**)&id); dev->Release(); }
    bool p2den = gP2denDevice && id == gP2denDevice; if (id) id->Release();
    if (!p2den) return false;
    *fmt = d.Format;
    if (!gPalLogged) { gPalLogged = true; Log("[pal] p2den palette texture %ux%u fmt %d", d.Width, d.Height, d.Format); }
    return true;
}
static void StorePalette(const void* data, UINT rowPitch, DXGI_FORMAT fmt, ID3D11Resource* r) {
    D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d);
    EnterCriticalSection(&gPalCs);
    for (UINT y = 0, i = 0; y < d.Height; y++) {
        const uint8_t* row = (const uint8_t*)data + y * rowPitch;
        for (UINT x = 0; x < d.Width; x++, i++) {
            const uint8_t* px = row + x * 4;
            bool bgr = fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8X8_UNORM;
            gPal[i*4] = bgr ? px[2] : px[0]; gPal[i*4+1] = px[1]; gPal[i*4+2] = bgr ? px[0] : px[2]; gPal[i*4+3] = 255;
        }
    }
    LeaveCriticalSection(&gPalCs);
    if (!gPalValid) Log("[pal] first p2den palette: c0=%d,%d,%d c8=%d,%d,%d cf=%d,%d,%d fe=%d,%d,%d", gPal[0xc0*4], gPal[0xc0*4+1], gPal[0xc0*4+2],
                        gPal[0xc8*4], gPal[0xc8*4+1], gPal[0xc8*4+2], gPal[0xcf*4], gPal[0xcf*4+1], gPal[0xcf*4+2], gPal[0xfe*4], gPal[0xfe*4+1], gPal[0xfe*4+2]);
    InterlockedExchange(&gPalValid, 1);
}
// p2den uploads each finished 8-bit frame through a CPU-written staging R8 texture (640x480, and its window-size 2D layer):
// the overlay callback (p2vr: cheat messages) draws into that copy just before Unmap, so it shows on the desktop and in the
// headset in every game phase, without touching the game's own buffers.
static void (*gOverlayCb)(uint8_t* data, int pitch, int w, int h);
void Capture_SetOverlayCallback(void (*f)(uint8_t*, int, int, int)) { gOverlayCb = f; }
static ID3D11Resource* gIdxMappedRes; static uint8_t* gIdxMapped; static int gIdxPitch, gIdxW, gIdxH;
static HRESULT STDMETHODCALLTYPE HkMap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP t, UINT f, D3D11_MAPPED_SUBRESOURCE* m) {
    HRESULT hr = oMap(c, r, sub, t, f, m);
    DXGI_FORMAT fmt;
    if (SUCCEEDED(hr) && m && t != D3D11_MAP_READ && IsPaletteTex(r, &fmt)) { gPalMapped = m->pData; gPalMappedRes = r; gPalMappedPitch = m->RowPitch; }
    else if (SUCCEEDED(hr) && m && t != D3D11_MAP_READ && gOverlayCb && r) {
        D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d);
            if (d.Format == DXGI_FORMAT_R8_UNORM && d.Usage == D3D11_USAGE_STAGING && d.Width >= 640 && d.Height >= 480) {
                gIdxMappedRes = r; gIdxMapped = (uint8_t*)m->pData; gIdxPitch = (int)m->RowPitch; gIdxW = (int)d.Width; gIdxH = (int)d.Height;
            }
        }
    }
    return hr;
}
static void STDMETHODCALLTYPE HkUnmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    if (r && r == gPalMappedRes && gPalMapped) {
        DXGI_FORMAT fmt; D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d); fmt = d.Format;
        StorePalette(gPalMapped, gPalMappedPitch, fmt, r);
        gPalMapped = nullptr; gPalMappedRes = nullptr;
    }
    if (r && r == gIdxMappedRes && gIdxMapped) {
        if (gOverlayCb) gOverlayCb(gIdxMapped, gIdxPitch, gIdxW, gIdxH);
        gIdxMapped = nullptr; gIdxMappedRes = nullptr;
    }
    oUnmap(c, r, sub);
}
static void STDMETHODCALLTYPE HkUpdateSub(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp) {
    DXGI_FORMAT fmt;
    if (data && !box && IsPaletteTex(r, &fmt)) StorePalette(data, rp ? rp : 1024, fmt, r);
    oUpdateSub(c, r, sub, box, data, rp, dp);
}
static ID3D11Texture2D* gP2denPalTex; static DXGI_FORMAT gP2denPalFmt;
static ID3D11Texture2D* gPalStage[3]; static uint32_t gPalFrame;

// Called from the Present hook on p2den's thread: async GPU readback of p2den's palette (3-deep ring, never stalls).
static void ReadbackPalette(ID3D11Device* dev, ID3D11DeviceContext* c) {
    if (!gP2denPalTex) return;
    if (!gPalStage[0]) {
        D3D11_TEXTURE2D_DESC d = {256, 1, 1, 1, gP2denPalFmt, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
        for (int i = 0; i < 3; i++) if (FAILED(dev->CreateTexture2D(&d, nullptr, &gPalStage[i]))) return;
    }
    c->CopyResource(gPalStage[gPalFrame % 3], gP2denPalTex);
    if (gPalFrame >= 2) {
        ID3D11Texture2D* rd = gPalStage[(gPalFrame - 2) % 3];
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(c->Map(rd, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
            StorePalette(m.pData, m.RowPitch, gP2denPalFmt, rd);
            c->Unmap(rd, 0);
        }
    }
    gPalFrame++;
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateTex2D)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
static PFN_CreateTex2D oCreateTex2D;
static HRESULT STDMETHODCALLTYPE HkCreateTex2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    HRESULT hr = oCreateTex2D(dev, d, init, out);
    if (SUCCEEDED(hr) && d && out && *out) {
        static int n; static uint32_t seen[64];
        uint32_t key = (d->Width << 20) ^ (d->Height << 8) ^ d->Format ^ (d->Usage << 30) ^ (init ? 0x40000000u : 0);
        bool newKey = true; for (int i = 0; i < n; i++) if (seen[i] == key) newKey = false;
        if (newKey && n < 64) { seen[n++] = key; Log("[pal-diag] CreateTexture2D %ux%u fmt %d usage %d bind %x init %d", d->Width, d->Height, d->Format, d->Usage, d->BindFlags, init != nullptr); }
        if (d->Width == 256 && d->Height == 1 && d->Usage == D3D11_USAGE_DEFAULT && (d->BindFlags & D3D11_BIND_SHADER_RESOURCE) &&
            (d->Format == DXGI_FORMAT_B8G8R8A8_UNORM || d->Format == DXGI_FORMAT_R8G8B8A8_UNORM)) {
            IUnknown* id = nullptr; dev->QueryInterface(__uuidof(IUnknown), (void**)&id);
            if (id && id == gP2denDevice) {
                if (gP2denPalTex) gP2denPalTex->Release();
                gP2denPalTex = *out; gP2denPalTex->AddRef(); gP2denPalFmt = d->Format;
                Log("[pal] p2den palette texture %p fmt %d", *out, d->Format);
            }
            if (id) id->Release();
        }
    }
    return hr;
}

// Displayed HUD frame (what p2den shows), resampled to the game's 640x480 grid, RGB. Read back about once a second
// through a 3-deep staging ring so it never stalls. Used to learn the true on-screen colours of HUD palette indices.
static ID3D11Texture2D* gHudStage[3]; static UINT gHudStageW, gHudStageH; static uint32_t gHudFrame;
static uint8_t gHudRGB[640 * 480 * 3]; static volatile LONG gHudRGBValid;
static void ReadbackHud(ID3D11Device* dev, ID3D11DeviceContext* c) {
    if (!gShared) return;
    D3D11_TEXTURE2D_DESC sd; gShared->GetDesc(&sd);
    if (!gHudStage[0] || gHudStageW != sd.Width || gHudStageH != sd.Height) {
        for (auto& t : gHudStage) if (t) { t->Release(); t = nullptr; }
        D3D11_TEXTURE2D_DESC d = sd; d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        for (auto& t : gHudStage) if (FAILED(dev->CreateTexture2D(&d, nullptr, &t))) return;
        gHudStageW = sd.Width; gHudStageH = sd.Height; gHudFrame = 0;
    }
    uint32_t f = gHudFrame++;
    if (f % 30 == 0) c->CopyResource(gHudStage[(f / 30) % 3], gShared);
    if (f % 30 == 15 && f >= 75) {                            // the copy made two cycles earlier is ready by now
        ID3D11Texture2D* rd = gHudStage[(f / 30 + 1) % 3];
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(c->Map(rd, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
            bool bgr = sd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
            float W = (float)sd.Width, H = (float)sd.Height, vw = H * 4.f / 3.f, vh = H;
            if (vw > W) { vw = W; vh = W * 3.f / 4.f; }
            float ox = (W - vw) * 0.5f, oy = (H - vh) * 0.5f;
            for (int y = 0; y < 480; y++) {
                int sy = (int)(oy + (y + 0.5f) * vh / 480.f); const uint8_t* row = (const uint8_t*)m.pData + (size_t)sy * m.RowPitch;
                for (int x = 0; x < 640; x++) {
                    int sx = (int)(ox + (x + 0.5f) * vw / 640.f); const uint8_t* px = row + sx * 4; uint8_t* o = gHudRGB + (y * 640 + x) * 3;
                    o[0] = bgr ? px[2] : px[0]; o[1] = px[1]; o[2] = bgr ? px[0] : px[2];
                }
            }
            c->Unmap(rd, 0);
            InterlockedExchange(&gHudRGBValid, 1);
        }
    }
}
bool Capture_GetHudRGB(uint8_t* out) {   // 640*480*3
    if (!gHudRGBValid) return false;
    memcpy(out, gHudRGB, sizeof gHudRGB); return true;
}

bool Capture_GetPalette(uint8_t out[1024]) {
    if (!gPalValid) return false;
    EnterCriticalSection(&gPalCs); memcpy(out, gPal, 1024); LeaveCriticalSection(&gPalCs);
    return true;
}

static void PatchVtbl(void** vt, int idx, void* hook, void** orig) {
    if (vt[idx] == hook) return;
    DWORD old; VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old);
    if (!*orig) *orig = vt[idx];
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
}

static void CaptureFrame(IDXGISwapChain* sc) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return;
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    ID3D11Device* dev = nullptr; bb->GetDevice(&dev);
    ID3D11DeviceContext* c = nullptr; dev->GetImmediateContext(&c);
    if (!gShared || d.Width != gW || d.Height != gH) {
        if (gShared) { gShared->Release(); gShared = nullptr; }
        D3D11_TEXTURE2D_DESC s = d;
        s.Format = (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? DXGI_FORMAT_B8G8R8A8_TYPELESS
                                                                                                         : DXGI_FORMAT_R8G8B8A8_TYPELESS;
        s.MipLevels = 1; s.ArraySize = 1; s.SampleDesc = {1, 0}; s.Usage = D3D11_USAGE_DEFAULT;
        s.BindFlags = D3D11_BIND_SHADER_RESOURCE; s.CPUAccessFlags = 0; s.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        if (SUCCEEDED(dev->CreateTexture2D(&s, nullptr, &gShared))) {
            IDXGIResource* r = nullptr; gShared->QueryInterface(__uuidof(IDXGIResource), (void**)&r);
            r->GetSharedHandle(&gSharedHandle); r->Release();
            gW = d.Width; gH = d.Height;
            IDXGIDevice* xd = nullptr; IDXGIAdapter* ad = nullptr; DXGI_ADAPTER_DESC adesc = {};
            if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xd)) && SUCCEEDED(xd->GetAdapter(&ad))) ad->GetDesc(&adesc);
            if (ad) ad->Release(); if (xd) xd->Release();
            char n[128]; WideCharToMultiByte(CP_UTF8, 0, adesc.Description, -1, n, 128, 0, 0);
            LUID vl = Vr_AdapterLuid();
            Log("[cap] p2den backbuffer %ux%u fmt %d on '%s'%s", d.Width, d.Height, d.Format, n,
                (vl.LowPart || vl.HighPart) && (vl.LowPart != adesc.AdapterLuid.LowPart || vl.HighPart != adesc.AdapterLuid.HighPart)
                    ? "  WARNING: different GPU than the HMD - set the game to use the HMD GPU" : "");
        } else {
            Log("[cap] CreateTexture2D(shared) failed"); gShared = nullptr;
        }
    }
    if (gShared && d.SampleDesc.Count == 1) {
        c->CopyResource(gShared, bb); c->Flush();
        Vr_SetScreenSource(gSharedHandle, gW, gH, ++gFrame);
        ReadbackPalette(dev, c);
        ReadbackHud(dev, c);
        ComposeMirror(dev, c, bb, d);
    }
    c->Release(); dev->Release(); bb->Release();
}

static HWND gDummyWnd;
static HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!gSwap) {   // adopt the first swapchain presented by this process that is not our dummy
        DXGI_SWAP_CHAIN_DESC d;
        if (SUCCEEDED(sc->GetDesc(&d)) && d.OutputWindow && d.OutputWindow != gDummyWnd) {
            DWORD pid = 0; GetWindowThreadProcessId(d.OutputWindow, &pid);
            if (pid == GetCurrentProcessId()) { gSwap = sc; Log("[cap] adopted swapchain %p (%ux%u) on first Present", sc, d.BufferDesc.Width, d.BufferDesc.Height); }
        }
    }
    if (sc == gSwap && !(flags & DXGI_PRESENT_TEST)) CaptureFrame(sc);
    return oPresent(sc, sync, flags);
}

// Patch Present in the swapchain vtable shared by all DXGI swapchains, using a throwaway device/swapchain.
static DWORD WINAPI DummyHookThread(LPVOID) {
    WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "p2vr_dummy";
    RegisterClassA(&wc);
    gDummyWnd = CreateWindowA("p2vr_dummy", "", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC d = {}; d.BufferCount = 2; d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.BufferDesc.Width = 16; d.BufferDesc.Height = 16;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.OutputWindow = gDummyWnd; d.SampleDesc.Count = 1; d.Windowed = TRUE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* c = nullptr; IDXGISwapChain* sc = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &sc, &dev, nullptr, &c);
    if (FAILED(hr)) { d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; d.BufferCount = 1;
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &sc, &dev, nullptr, &c); }
    if (SUCCEEDED(hr)) {
        PatchVtbl(*(void***)sc, 8, (void*)HkPresent, (void**)&oPresent);
        Log("[cap] Present hooked via dummy swapchain");
        sc->Release(); c->Release(); dev->Release();
    } else Log("[cap] dummy swapchain failed %08x", hr);
    DestroyWindow(gDummyWnd);
    return 0;
}

static bool IsP2denDevice(IUnknown* dev) {
    if (!gP2denDevice || !dev) return true;   // unknown -> accept (first swapchain wins)
    IUnknown* id = nullptr; dev->QueryInterface(__uuidof(IUnknown), (void**)&id);
    bool same = id == gP2denDevice; if (id) id->Release(); return same;
}
static void HookSwap(IDXGISwapChain* sc) {
    gSwap = sc;
    PatchVtbl(*(void***)sc, 8, (void*)HkPresent, (void**)&oPresent);
    Log("[cap] hooked p2den swapchain %p", sc);
}

static HRESULT STDMETHODCALLTYPE HkCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) {
    HRESULT hr = oCreateSwapChain(f, dev, d, out);
    if (SUCCEEDED(hr) && out && *out && IsP2denDevice(dev)) HookSwap(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE HkCreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d,
                                                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd, IDXGIOutput* o, IDXGISwapChain1** out) {
    HRESULT hr = oCreateSwapChainForHwnd(f, dev, w, d, fd, o, out);
    if (SUCCEEDED(hr) && out && *out && IsP2denDevice(dev)) HookSwap(*out);
    return hr;
}

static void PatchFactory(IUnknown* fac, const char* how) {
    IDXGIFactory* f = nullptr;
    if (SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory), (void**)&f))) {
        PatchVtbl(*(void***)f, 10, (void*)HkCreateSwapChain, (void**)&oCreateSwapChain); f->Release();
    }
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2))) {
        PatchVtbl(*(void***)f2, 15, (void*)HkCreateSwapChainForHwnd, (void**)&oCreateSwapChainForHwnd); f2->Release();
    }
    Log("[cap] factory hooked via %s", how);
}
static HRESULT WINAPI HkCreateDXGIFactory(REFIID riid, void** out) {
    HRESULT hr = oCreateFactory(riid, out);
    if (SUCCEEDED(hr) && out && *out) PatchFactory((IUnknown*)*out, "CreateDXGIFactory");
    return hr;
}
static HRESULT WINAPI HkD3D11CreateDevice(IDXGIAdapter* a, D3D_DRIVER_TYPE dt, HMODULE sw, UINT fl, const D3D_FEATURE_LEVEL* fls, UINT nfl,
                                          UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* ofl, ID3D11DeviceContext** ctx) {
    HRESULT hr = oD3D11CreateDevice(a, dt, sw, fl, fls, nfl, sdk, dev, ofl, ctx);
    if (SUCCEEDED(hr) && dev && *dev) {
        if (gP2denDevice) gP2denDevice->Release();
        (*dev)->QueryInterface(__uuidof(IUnknown), (void**)&gP2denDevice);
        IDXGIDevice* xd = nullptr; IDXGIAdapter* ad = nullptr; IUnknown* fac = nullptr;
        if (SUCCEEDED((*dev)->QueryInterface(__uuidof(IDXGIDevice), (void**)&xd)) && SUCCEEDED(xd->GetAdapter(&ad)) &&
            SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory), (void**)&fac))) {
            DXGI_ADAPTER_DESC d; ad->GetDesc(&d); char n[128]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, n, 128, 0, 0);
            Log("[cap] p2den D3D11 device on '%s'", n);
            PatchVtbl(*(void***)*dev, 5, (void*)HkCreateTex2D, (void**)&oCreateTex2D);

            PatchFactory(fac, "D3D11CreateDevice");
        }
        if (fac) fac->Release(); if (ad) ad->Release(); if (xd) xd->Release();
    }
    return hr;
}

// Patch one import of a loaded module.
bool PatchImport(HMODULE mod, const char* dll, const char* func, void* hook, void** orig) {
    uint8_t* b = (uint8_t*)mod; bool found = false;
    auto nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(b + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char*)(b + imp->Name), dll)) continue;
        auto names = (IMAGE_THUNK_DATA*)(b + imp->OriginalFirstThunk);
        auto iat = (IMAGE_THUNK_DATA*)(b + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(b + names->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, func)) continue;
            DWORD old; VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            if (!*orig) *orig = (void*)iat->u1.Function;
            iat->u1.Function = (uintptr_t)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            found = true;
        }
    }
    return found;
}

void Capture_Install(LogFn log) {
    Log = log;
    InitializeCriticalSection(&gMirCs);
    InitializeCriticalSection(&gPalCs);
    HMODULE p2den = GetModuleHandleA("p2den.dll");
    bool a = p2den && PatchImport(p2den, "dxgi.dll", "CreateDXGIFactory", (void*)HkCreateDXGIFactory, (void**)&oCreateFactory);
    bool b = p2den && PatchImport(p2den, "d3d11.dll", "D3D11CreateDevice", (void*)HkD3D11CreateDevice, (void**)&oD3D11CreateDevice);
    Log("[cap] p2den import patch: dxgi=%d d3d11=%d", a, b);
    CreateThread(nullptr, 0, DummyHookThread, nullptr, 0, nullptr);
}
