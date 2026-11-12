// bHaptics TactSuit support, through bHaptics' C SDK (bhaptics_library.dll, 32-bit, loaded at run time) talking to
// bHaptics Player. No overhead without a suit: nothing is loaded, no thread is started, unless the DLL is in the game
// folder, bHaptics Player is installed, credentials are present and BHAPTICS is not 0; once running, the worker idles
// until a TactSuit is reported connected. Every SDK call is made on the worker thread (they block and are serialized);
// the game thread only queues commands.
//
// The vest is driven with playDot: 32 motor values, front 4x4 then back 4x4, row 0 = shoulders (bHaptics' layout;
// bHaptics Player spreads them over the X40's 2 x 20 motors). Which column is the wearer's left on each side is set by
// the calibration (Ctrl+0) and kept in g_frontLeftCol0 / g_backLeftCol0.
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <vector>
#include "haptics.h"

#ifndef P2VR_BHAPTICS_KEY
#define P2VR_BHAPTICS_KEY ""
#endif
#ifndef P2VR_BHAPTICS_WORKSPACE
#define P2VR_BHAPTICS_WORKSPACE ""
#endif

volatile bool g_hapticsOn;

static void NoLog(const char*, ...) {}
static void (*Log)(const char*, ...) = NoLog;

// ---------------------------------------------------------------- SDK entry points (bhaptics.h, __cdecl)
typedef bool        (__cdecl* PFN_registryAndInit)(const char*, const char*, const char*);
typedef bool        (__cdecl* PFN_boolVoid)(void);
typedef bool        (__cdecl* PFN_boolInt)(int32_t);
typedef bool        (__cdecl* PFN_boolBool)(bool);
typedef void        (__cdecl* PFN_voidVoid)(void);
typedef const char* (__cdecl* PFN_strVoid)(void);
typedef int32_t     (__cdecl* PFN_playDot)(int32_t, int32_t, int32_t, const int32_t*, int32_t);
static struct {
    PFN_registryAndInit registryAndInit; PFN_boolVoid wsIsConnected, isPlayerRunning, stopAll; PFN_boolInt isbHapticsConnected;
    PFN_boolBool launchPlayer; PFN_voidVoid wsClose; PFN_strVoid getVersion, getDeviceInfoJson; PFN_playDot playDot;
} B;
static HMODULE g_lib;

// ---------------------------------------------------------------- settings
static float g_master = 1.0f, g_sShots = 0.35f, g_sHits = 1.0f, g_sMissile = 0.8f, g_sAfter = 0.3f, g_sJump = 0.7f, g_sHeart = 0.5f;
static bool  g_frontLeftCol0 = true, g_backLeftCol0 = false;   // which grid column is the wearer's left (calibration)

// ---------------------------------------------------------------- commands (game thread -> worker)
enum { C_SHOT, C_HIT, C_MISSILE, C_AFTER, C_JUMP, C_LAND, C_DEATH, C_HEART, C_STOP, C_CALIB };
struct Cmd { int type; float a, b, c; };
static CRITICAL_SECTION g_cs; static std::vector<Cmd> g_queue; static HANDLE g_wake, g_thread;
static volatile bool g_stop, g_started;
static void Push(int type, float a = 0, float b = 0, float c = 0) {
    if (!g_started) return;
    EnterCriticalSection(&g_cs); if (g_queue.size() < 256) g_queue.push_back({type, a, b, c}); LeaveCriticalSection(&g_cs);
    SetEvent(g_wake);
}

// ---------------------------------------------------------------- frames (worker only)
struct Frame { DWORD at; int ms; int32_t m[32]; char label[64]; };
static std::vector<Frame> g_frames;
static void Schedule(DWORD delay, int ms, const int32_t m[32], const char* label = nullptr) {
    Frame f = {}; f.at = GetTickCount() + delay; f.ms = ms; memcpy(f.m, m, sizeof f.m);
    if (label) strncpy_s(f.label, label, _TRUNCATE);
    g_frames.push_back(f);
}
static int Clamp100(float v) { int i = (int)(v + 0.5f); return i < 0 ? 0 : i > 100 ? 100 : i; }
static int Idx(bool back, int row, int col) { return (back ? 16 : 0) + row * 4 + col; }
// a vibration centred on (x, y) of one side: x 0 = wearer's left .. 1 = right, y 0 = shoulders .. 1 = waist
static void Spot(int32_t m[32], bool back, float x, float y, float strength, float radius = 0.38f) {
    bool leftCol0 = back ? g_backLeftCol0 : g_frontLeftCol0;
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        float cx = (c + 0.5f) / 4.f; if (!leftCol0) cx = 1.f - cx;
        float cy = (r + 0.5f) / 4.f;
        float d = sqrtf((cx - x) * (cx - x) + (cy - y) * (cy - y)), w = 1.f - d / radius;
        if (w <= 0) continue;
        int& v = (int&)m[Idx(back, r, c)]; int nv = Clamp100(strength * w * 100.f); if (nv > v) v = nv;
    }
}
static void Rows(int32_t m[32], bool back, int r0, int r1, float strength) {
    for (int r = r0; r <= r1; r++) for (int c = 0; c < 4; c++) { int& v = (int&)m[Idx(back, r, c)]; int nv = Clamp100(strength * 100.f); if (nv > v) v = nv; }
}

// ---------------------------------------------------------------- effects (worker only)
static int  g_shotSide;
static bool g_afterOn; static DWORD g_afterNext;
static float g_heart; static DWORD g_heartNext;
static void FxShot(float s) {   // a light tap on the upper chest, alternating sides like the two guns
    int32_t m[32] = {}; Spot(m, false, (g_shotSide ^= 1) ? 0.3f : 0.7f, 0.15f, s * g_sShots * g_master, 0.3f); Schedule(0, 60, m);
}
static void FxHit(float az, float el, float s) {   // where the shot came from: front or back, left / right, up / down
    bool back = fabsf(az) > 90.f;
    float a = az * 0.0174533f;
    float x = 0.5f + 0.5f * sinf(a);                 // + az = right of the wearer, on both sides (left stays left)
    float y = 0.5f - el / 90.f * 0.5f; if (y < 0.1f) y = 0.1f; if (y > 0.9f) y = 0.9f;
    float k = s * g_sHits * g_master;
    int32_t m[32] = {}; Spot(m, back, x, y, k, 0.45f);
    int ms = 80 + (int)(150 * (s > 1 ? 1 : s));
    Schedule(0, ms, m);
}
static void FxMissile() {
    int32_t m[32] = {}; Spot(m, false, 0.5f, 0.45f, g_sMissile * g_master, 0.5f); Schedule(0, 150, m);
    int32_t m2[32] = {}; Spot(m2, false, 0.5f, 0.45f, 0.5f * g_sMissile * g_master, 0.6f); Schedule(150, 120, m2);
}
static void FxJump() {          // a wave from the chest through to the back
    for (int r = 0; r < 4; r++) { int32_t m[32] = {}; Rows(m, false, r, r, g_sJump * g_master); Schedule(r * 70, 90, m); }
    for (int r = 0; r < 4; r++) { int32_t m[32] = {}; Rows(m, true, r, r, g_sJump * g_master); Schedule(280 + r * 70, 90, m); }
}
static void FxLand() {          // soft, from the waist up, both sides
    for (int r = 3; r >= 0; r--) { int32_t m[32] = {}; Rows(m, false, r, r, 0.6f * g_sJump * g_master); Rows(m, true, r, r, 0.6f * g_sJump * g_master); Schedule((3 - r) * 90, 110, m); }
}
static void FxDeath() {
    const float lv[5] = {1.f, 0.8f, 0.55f, 0.35f, 0.15f};
    for (int i = 0; i < 5; i++) { int32_t m[32] = {}; Rows(m, false, 0, 3, lv[i] * g_master); Rows(m, true, 0, 3, lv[i] * g_master); Schedule(i * 220, 240, m); }
}
static void Continuous(DWORD now) {
    if (g_afterOn && (LONG)(now - g_afterNext) >= 0) {   // afterburner: low rumble on the lower back
        int32_t m[32] = {}; Rows(m, true, 2, 3, g_sAfter * g_master); Schedule(0, 140, m); g_afterNext = now + 120;
    }
    if (g_heart > 0 && (LONG)(now - g_heartNext) >= 0) {   // heartbeat on the left chest: faster and a little stronger as the armour drops
        int32_t m[32] = {}; Spot(m, false, 0.32f, 0.35f, (0.6f + 0.4f * g_heart) * g_sHeart * g_master, 0.3f);
        int32_t m2[32] = {}; Spot(m2, false, 0.32f, 0.35f, (0.4f + 0.3f * g_heart) * g_sHeart * g_master, 0.3f);
        Schedule(0, 70, m); Schedule(170, 60, m2);   // "lub-dub"
        g_heartNext = now + (DWORD)(1300 - 800 * g_heart);
    }
}
static void Calibrate() {
    Log("[haptics] calibration: each step is logged when it plays; note where you feel it");
    struct S { bool back; int r, c; const char* name; } dots[] = {
        {false, 0, 0, "dot 0: FRONT row 0 (shoulders) column 0"}, {false, 0, 3, "dot 3: FRONT row 0 (shoulders) column 3"},
        {false, 3, 0, "dot 12: FRONT row 3 (waist) column 0"},    {true, 0, 0, "dot 16: BACK row 0 (shoulders) column 0"},
        {true, 0, 3, "dot 19: BACK row 0 (shoulders) column 3"},   {true, 3, 0, "dot 28: BACK row 3 (waist) column 0"}};
    DWORD t = 500;
    for (auto& d : dots) { int32_t m[32] = {}; m[Idx(d.back, d.r, d.c)] = 100; Schedule(t, 500, m, d.name); t += 1400; }
    t += 600;
    {   int32_t m[32] = {}; Spot(m, false, 0.15f, 0.4f, 1.f, 0.3f); Schedule(t, 500, m, "spot: FRONT, wearer's LEFT side (mapped)"); t += 1400; }
    {   int32_t m[32] = {}; Spot(m, true, 0.15f, 0.4f, 1.f, 0.3f); Schedule(t, 500, m, "spot: BACK, wearer's LEFT side (mapped)"); t += 1400; }
    for (int i = 0; i < 32; i++) {   // walk of every index
        int32_t m[32] = {}; m[i] = 100; char l[64]; sprintf_s(l, "walk: dot %d (%s row %d column %d)", i, i < 16 ? "front" : "back", (i % 16) / 4, i % 4);
        Schedule(t, 220, m, l); t += 300;
    }
}

// ---------------------------------------------------------------- worker
static DWORD WINAPI Worker(LPVOID p) {
    const char** cred = (const char**)p;
    bool ok = B.registryAndInit(cred[0], cred[1], "");
    Log("[haptics] bHaptics SDK %s: %s", B.getVersion ? B.getVersion() : "?", ok ? "connecting to bHaptics Player" : "init FAILED");
    DWORD nextCheck = 0, checkEvery = 1000; bool lastWs = false, lastSuit = false, infoLogged = false;
    while (!g_stop) {
        WaitForSingleObject(g_wake, 15);
        DWORD now = GetTickCount();
        if ((LONG)(now - nextCheck) >= 0) {   // connection state: Player, then the suit (every 1 s at first, then 10 s)
            // The effects run while bHaptics Player is connected: isbHapticsConnected(Vest) does not report every suit
            // (a TactSuit X40 that plays fine reads false), so it is only logged, with the Player's device list.
            bool ws = B.wsIsConnected(), suit = ws && B.isbHapticsConnected(0);
            if (ws != lastWs) Log("[haptics] bHaptics Player %s", ws ? "connected: vest effects on" : "not connected (is it running?): vest effects off");
            if (ws && suit != lastSuit) Log("[haptics] SDK reports the vest %s", suit ? "connected" : "not connected (effects are sent anyway)");
            if (ws && !infoLogged && B.getDeviceInfoJson) { infoLogged = true; const char* j = B.getDeviceInfoJson(); Log("[haptics] devices: %.800s", j && *j ? j : "(none reported)"); }
            lastWs = ws; lastSuit = suit; g_hapticsOn = ws;
            nextCheck = now + checkEvery; if (now > 20000 && checkEvery < 10000) checkEvery = 10000;
        }
        std::vector<Cmd> cmds;
        EnterCriticalSection(&g_cs); cmds.swap(g_queue); LeaveCriticalSection(&g_cs);
        for (auto& c : cmds) {
            if (c.type == C_CALIB) { Calibrate(); continue; }   // also without a reported suit (to diagnose)
            if (c.type == C_STOP) { g_frames.clear(); g_afterOn = false; g_heart = 0; if (lastWs) B.stopAll(); continue; }
            if (!g_hapticsOn) continue;
            switch (c.type) {
            case C_SHOT: FxShot(c.a); break;   case C_HIT: FxHit(c.a, c.b, c.c); break;
            case C_MISSILE: FxMissile(); break; case C_JUMP: FxJump(); break; case C_LAND: FxLand(); break;
            case C_DEATH: FxDeath(); break;
            case C_AFTER: g_afterOn = c.a != 0; break;
            case C_HEART: if (g_heart <= 0 && c.a > 0) g_heartNext = now; g_heart = c.a; break;
            }
        }
        if (g_hapticsOn) Continuous(now);
        for (size_t i = 0; i < g_frames.size();) {   // play what is due
            Frame& f = g_frames[i];
            if ((LONG)(now - f.at) < 0) { i++; continue; }
            if (lastWs) B.playDot(-1, 0, f.ms, f.m, 32);
            if (f.label[0]) Log("[haptics] test: %s", f.label);
            g_frames.erase(g_frames.begin() + i);
        }
    }
    if (lastWs) { B.stopAll(); B.wsClose(); }
    return 0;
}

// ---------------------------------------------------------------- public
static bool PlayerInstalled() {
    char p[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", p, MAX_PATH)) {
        strcat_s(p, "\\bHapticsPlayer\\BhapticsPlayer.exe");
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) return true;
    }
    const struct { HKEY root; const char* path; } keys[] = {
        {HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
        {HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
        {HKEY_LOCAL_MACHINE, "Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"}};
    for (auto& k : keys) {
        HKEY h; if (RegOpenKeyExA(k.root, k.path, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
        char sub[256]; bool found = false;
        for (DWORD i = 0; !found; i++) {
            DWORD n = sizeof sub; if (RegEnumKeyExA(h, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            char name[256] = ""; DWORD sz = sizeof name;
            if (RegGetValueA(h, sub, "DisplayName", RRF_RT_REG_SZ, nullptr, name, &sz) == ERROR_SUCCESS && strstr(name, "bHaptics")) found = true;
        }
        RegCloseKey(h); if (found) return true;
    }
    return false;
}
static float IniF(const char* ini, const char* key, float def) {
    char b[32], d[32]; sprintf_s(d, "%g", def); GetPrivateProfileStringA("VR", key, d, b, sizeof b, ini); return (float)atof(b);
}
void Haptics_Init(const char* ini, const char* gameDir, void (*log)(const char*, ...)) {
    if (log) Log = log;
    if (GetPrivateProfileIntA("VR", "BHAPTICS", 1, ini) == 0) { Log("[haptics] off: BHAPTICS=0"); return; }
    char dll[MAX_PATH]; sprintf_s(dll, "%s\\bhaptics_library.dll", gameDir);
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) { Log("[haptics] off: no bhaptics_library.dll (installed only with bHaptics Player)"); return; }
    if (!PlayerInstalled()) { Log("[haptics] off: bHaptics Player is not installed"); return; }
    static char key[128], ws[128];
    GetPrivateProfileStringA("VR", "BHAPTICS_API_KEY", P2VR_BHAPTICS_KEY, key, sizeof key, ini);
    GetPrivateProfileStringA("VR", "BHAPTICS_WORKSPACE_ID", P2VR_BHAPTICS_WORKSPACE, ws, sizeof ws, ini);
    if (!key[0] || !ws[0]) { Log("[haptics] off: no bHaptics API key / workspace ID in this build"); return; }
    g_lib = LoadLibraryA(dll);
    if (!g_lib) { Log("[haptics] off: bhaptics_library.dll did not load (error %lu)", GetLastError()); return; }
    #define GET(field, name) B.field = (decltype(B.field))GetProcAddress(g_lib, name)
    GET(registryAndInit, "registryAndInit"); GET(wsIsConnected, "wsIsConnected"); GET(isPlayerRunning, "isPlayerRunning");
    GET(stopAll, "stopAll"); GET(isbHapticsConnected, "isbHapticsConnected"); GET(launchPlayer, "launchPlayer");
    GET(wsClose, "wsClose"); GET(getVersion, "bHapticsGetVersion"); GET(getDeviceInfoJson, "getDeviceInfoJson"); GET(playDot, "playDot");
    #undef GET
    if (!B.registryAndInit || !B.wsIsConnected || !B.stopAll || !B.isbHapticsConnected || !B.wsClose || !B.playDot) {
        Log("[haptics] off: bhaptics_library.dll lacks expected functions"); return;
    }
    g_master = IniF(ini, "BHAPTICS_INTENSITY", 1.0f); g_sShots = IniF(ini, "BHAPTICS_SHOTS", 0.35f); g_sHits = IniF(ini, "BHAPTICS_HITS", 1.0f);
    g_sMissile = IniF(ini, "BHAPTICS_MISSILE", 0.8f); g_sAfter = IniF(ini, "BHAPTICS_AFTERBURNER", 0.3f); g_sJump = IniF(ini, "BHAPTICS_JUMP", 0.7f);
    g_sHeart = IniF(ini, "BHAPTICS_HEARTBEAT", 0.5f);
    if (B.isPlayerRunning && !B.isPlayerRunning()) {
        if (GetPrivateProfileIntA("VR", "BHAPTICS_LAUNCH_PLAYER", 0, ini) && B.launchPlayer) { Log("[haptics] starting bHaptics Player"); B.launchPlayer(true); }
        else Log("[haptics] bHaptics Player is not running: the vest stays off until it is started");
    }
    InitializeCriticalSection(&g_cs);
    g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    static const char* cred[2] = {key, ws};
    g_started = true;
    g_thread = CreateThread(nullptr, 0, Worker, (LPVOID)cred, 0, nullptr);
}
void Haptics_Shutdown() { if (!g_started) return; g_hapticsOn = false; g_stop = true; SetEvent(g_wake); }
void Haptics_Calibrate() { Push(C_CALIB); }
void Haptics_StopAll() { if (g_started) Push(C_STOP); }
void Haptics_Shot(float s) { if (g_hapticsOn) Push(C_SHOT, s); }
void Haptics_Hit(float az, float el, float s) { if (g_hapticsOn) Push(C_HIT, az, el, s); }
void Haptics_Missile() { if (g_hapticsOn) Push(C_MISSILE); }
void Haptics_Afterburner(bool on) { if (g_hapticsOn || !on) Push(C_AFTER, on ? 1.f : 0.f); }
void Haptics_Jump() { if (g_hapticsOn) Push(C_JUMP); }
void Haptics_Land() { if (g_hapticsOn) Push(C_LAND); }
void Haptics_Death() { if (g_hapticsOn) Push(C_DEATH); }
void Haptics_Heartbeat(float level) { if (g_hapticsOn || level <= 0) Push(C_HEART, level); }
