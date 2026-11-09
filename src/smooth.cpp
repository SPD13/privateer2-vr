// smooth.cpp - game-thread idle detection (step 1 of the 90 Hz smoothing).
// Diagnostic: hooks the timing/wait functions imported by DARK.EXE and p2den.dll and measures how the game thread
// spends its idle time between 30 Hz cycles.
#include <windows.h>
#include <stdio.h>
#include "vr.h"

bool PatchImport(HMODULE mod, const char* dll, const char* func, void* hook, void** orig);

static LogFn Log;
static volatile DWORD s_gameTid;
static LARGE_INTEGER s_qpf;

struct Stat { const char* name; volatile LONG calls; volatile LONGLONG ticks; };
static Stat s_stats[6] = {{"DARK Sleep"}, {"DARK WaitForSingleObject"}, {"DARK GetTickCount"},
                          {"p2den Sleep"}, {"p2den QueryPerformanceCounter"}, {"p2den SleepConditionVariableSRW"}};

static LONGLONG Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static bool OnGameThread() { return s_gameTid && GetCurrentThreadId() == s_gameTid; }

typedef void (WINAPI *PFN_Sleep)(DWORD);
typedef DWORD (WINAPI *PFN_Wait)(HANDLE, DWORD);
typedef DWORD (WINAPI *PFN_GTC)();
typedef BOOL (WINAPI *PFN_QPC)(LARGE_INTEGER*);
typedef BOOL (WINAPI *PFN_SCV)(PCONDITION_VARIABLE, PSRWLOCK, DWORD, ULONG);
static PFN_Sleep oSleepD, oSleepP; static PFN_Wait oWaitD; static PFN_GTC oGtcD; static PFN_QPC oQpcP; static PFN_SCV oScvP;

static void Acc(int i, LONGLONG t0) { InterlockedIncrement(&s_stats[i].calls); InterlockedAdd64(&s_stats[i].ticks, Qpc() - t0); }

static void WINAPI HkSleepD(DWORD ms) { if (!OnGameThread()) { oSleepD(ms); return; } LONGLONG t = Qpc(); oSleepD(ms); Acc(0, t); }
static DWORD WINAPI HkWaitD(HANDLE h, DWORD ms) { if (!OnGameThread()) return oWaitD(h, ms); LONGLONG t = Qpc(); DWORD r = oWaitD(h, ms); Acc(1, t); return r; }
static DWORD WINAPI HkGtcD() { if (OnGameThread()) InterlockedIncrement(&s_stats[2].calls); return oGtcD(); }
static void WINAPI HkSleepP(DWORD ms) { if (!OnGameThread()) { oSleepP(ms); return; } LONGLONG t = Qpc(); oSleepP(ms); Acc(3, t); }
// p2den waits for the next 30 Hz cycle by spinning on QueryPerformanceCounter on the game thread (~30M calls/s).
// The idle callback runs there: same thread as the game, so BRender and the game's actors are safe to use.
static void (*s_idleCb)(); static bool s_inCb; static LONG s_qpcCalls;
void Smooth_SetIdleCallback(void (*cb)()) { s_idleCb = cb; }
static BOOL WINAPI HkQpcP(LARGE_INTEGER* c) {
    if (OnGameThread()) {
        s_qpcCalls++;
        if (s_idleCb && !s_inCb) { s_inCb = true; s_idleCb(); s_inCb = false; }
    }
    return oQpcP(c);
}
static BOOL WINAPI HkScvP(PCONDITION_VARIABLE cv, PSRWLOCK l, DWORD ms, ULONG f) {
    if (!OnGameThread()) return oScvP(cv, l, ms, f); LONGLONG t = Qpc(); BOOL r = oScvP(cv, l, ms, f); Acc(5, t); return r; }

void Smooth_SetGameThread() { if (!s_gameTid) { s_gameTid = GetCurrentThreadId(); Log("[smooth] game thread %lu", s_gameTid); } }

void Smooth_Report(double seconds) {
    s_stats[4].calls = s_qpcCalls; s_qpcCalls = 0;
    char line[512]; int n = sprintf_s(line, "[idle] per second on game thread:");
    for (auto& s : s_stats) {
        LONG c = InterlockedExchange(&s.calls, 0); LONGLONG t = InterlockedExchange64(&s.ticks, 0);
        if (c) n += sprintf_s(line + n, sizeof line - n, " | %s %.0f calls %.1f ms", s.name, c / seconds, t * 1000.0 / s_qpf.QuadPart / seconds);
    }
    Log("%s", line);
}

void Smooth_Install(LogFn log) {
    Log = log; QueryPerformanceFrequency(&s_qpf);
    HMODULE exe = GetModuleHandleA(nullptr), p2den = GetModuleHandleA("p2den.dll");
    bool a = PatchImport(exe, "kernel32.dll", "Sleep", (void*)HkSleepD, (void**)&oSleepD);
    bool b = PatchImport(exe, "kernel32.dll", "WaitForSingleObject", (void*)HkWaitD, (void**)&oWaitD);
    bool c = PatchImport(exe, "kernel32.dll", "GetTickCount", (void*)HkGtcD, (void**)&oGtcD);
    bool d = p2den && PatchImport(p2den, "kernel32.dll", "Sleep", (void*)HkSleepP, (void**)&oSleepP);
    bool e = p2den && PatchImport(p2den, "kernel32.dll", "QueryPerformanceCounter", (void*)HkQpcP, (void**)&oQpcP);
    bool f = p2den && PatchImport(p2den, "kernel32.dll", "SleepConditionVariableSRW", (void*)HkScvP, (void**)&oScvP);
    Log("[smooth] idle diagnostics hooks: DARK Sleep=%d Wait=%d GTC=%d | p2den Sleep=%d QPC=%d SCV=%d", a, b, c, d, e, f);
}
