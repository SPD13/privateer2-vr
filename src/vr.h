// vr.h - OpenXR presenter for the Privateer 2 VR mod (runs on its own thread).
#pragma once
#include <windows.h>
#include <stdint.h>

typedef void (*LogFn)(const char* fmt, ...);

// Eye view in the LOCAL reference space (metres, OpenXR conventions: +Y up, -Z forward).
struct VrView { float pos[3]; float quat[4]; /* x,y,z,w */ };

struct VrEyeTarget {
    int   width, height;      // render size of each eye (<= 2048, BRender limit)
    float tanHalfH, tanHalfV; // symmetric frustum covering both eyes
};

void Vr_Start(const char* iniPath, LogFn log);
bool Vr_Active();                           // session running, eyes can be submitted
bool Vr_GetViews(VrView v[2]);              // latest predicted eye poses
bool Vr_GetEyeTarget(VrEyeTarget* t);
uint8_t* Vr_EyeBuffer(int eye);             // 8-bit back buffer (width*height), write then submit
void Vr_SubmitEyes(const VrView used[2], const uint8_t paletteRGBX[1024]);
void Vr_SetScreenSource(HANDLE sharedTex, int w, int h, uint32_t frameNo);  // from the p2den Present hook
void Vr_RequestRecenter();
void Vr_SubmitMarkers(const uint8_t* idx, int w, int h, const uint8_t paletteRGBX[1024]);  // 8-bit, 0 = transparent
LUID Vr_AdapterLuid();                      // adapter the HMD runtime wants (valid when Active)
