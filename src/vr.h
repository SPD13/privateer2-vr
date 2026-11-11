// vr.h - OpenXR presenter for the Privateer 2 VR mod (runs on its own thread).
#pragma once
#include <windows.h>
#include <stdint.h>

typedef void (*LogFn)(const char* fmt, ...);

// Eye view in the LOCAL reference space (metres, OpenXR conventions: +Y up, -Z forward).
struct VrView { float pos[3]; float quat[4]; /* render orientation x,y,z,w */ float rawQuat[4]; /* eye orientation */ };

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
// Crosshair drawn by the headset shader from p2den's displayed HUD, at infinity, behind ships. Per eye, before
// Vr_SubmitEyes: eye rows (eye axes in ship space), eye-pixel box of the crosshair area, occlusion mask for that box
// (1 = covered by a ship), and the crosshair rectangle in game pixels (640x480).
void Vr_SetEyeCrosshair(int eye, const float rows[9], int bx0, int by0, int bx1, int by1, const uint8_t* occ,
                        float gx0, float gy0, float gx1, float gy1);
void Vr_SetScreenSource(HANDLE sharedTex, int w, int h, uint32_t frameNo);  // from the p2den Present hook
void Vr_RequestRecenter();

// ---- VR controllers (OpenXR actions; remappable in SteamVR's controller binding screen)
#include "commands.h"   // game commands = SteamVR actions
struct VrControls {
    bool  active;                 // session focused and controller actions available
    float pitch, yaw;             // right stick: -1..1 (pitch +: stick forward)
    float roll, throttleRate;     // left stick: roll -1..1 (+ right), throttle rate -1..1 (+ forward)
    uint64_t buttons;             // bit per kVrCommands entry (held)
    bool  anyButton;              // any button / trigger / grip held (commands and menu actions)
};
bool Macro_Replaying();
// input the mod sends for the VR controllers (pointer, clicks, Esc, command keys) carries this dwExtraInfo, so the input
// recorder records it like real input (other synthetic input, e.g. a replay, is not recorded)
#define P2VR_INPUT_TAG 0x50325652
int Macro_TakeCheatKeys();         // macro.cpp: Ctrl+1..8 pressed since the last call (bit per cheat)
void Macro_Abort(const char* why);   // stop a running replay            // macro.cpp: a recorded input sequence (e.g. AUTOPLAY) is being replayed
void Vr_SetCursorOffset(int x, int y);   // game pixels: the screen draws its cursor this far right/down of the mouse
void Vr_ShowMessage(int which);
int Vr_GetPanelRects(int (*r)[4], int max);   // HUD panel rectangles (640x480 HUD pixels); 0 when panels are off
void Vr_SetPanelMask(uint32_t mask);
void Vr_SetPanelRects(const int (*r)[4], int n);   // this frame's panel rectangles (grown around text crossing an edge)          // bit per panel: something is drawn in it this frame   // 0 none, 1 "Entering space" prompt (world-fixed panel instead of the scene)
bool Vr_GetControls(VrControls* c);
void Vr_Haptic(int hand, float amplitude, float seconds);   // hand 0 left, 1 right, 2 both; amplitude 0..1
void Vr_SetReticleDistance(float metres);   // depth of what is under the crosshair (<= 0: use RETICLE_DIST)
LONG Vr_FrameCounter();                    // increments whenever new predicted eye poses are available
// One HUD marker (bracket, lead indicator, nav marker) found in the 640x480 marker image, with the distance of
// the object it marks (metres along the ship axis from the ship centre; <= 0 = at infinity).
struct VrMarkerBlob { int x0, y0, x1, y1; float depthM; int layer; };   // depthM: >0 metres, -1 infinity, -2 HUD glass   // layer = texture third: 0 markers, 1 lead sprite, 2 cached octagon
uint32_t Vr_SubmitMarkers(const uint8_t* idx, int w, int h, const uint8_t paletteRGBX[1024],
                          const VrMarkerBlob* blobs, int nBlobs);  // 8-bit image, 0 = transparent; returns serial
void Vr_SetObserverBack(float metres);   // eyes sit this far behind the ship centre
// Per rendered frame: LOCAL-space centre/size of markers that follow an object (same blended state as the eyes).
// valid[i] = 0 keeps the game's 30 Hz placement for that marker. serial = value returned by Vr_SubmitMarkers.
void Vr_SetMarkerPoses(uint32_t serial, const float (*pos)[3], const float (*size)[2], const uint8_t* valid, int n,
                       bool withEyes);   // withEyes: rendered together with the eye images about to be submitted
LUID Vr_AdapterLuid();                      // adapter the HMD runtime wants (valid when Active)
