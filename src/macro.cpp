// macro.cpp - record / replay keyboard + mouse input so test runs can get from game start to space automatically.
//
//   Ctrl+F2 : start / stop recording (saved to macro.txt next to the game)
//   Ctrl+F3 : replay now / abort a running replay
//   p2vr.ini [VR] AUTOPLAY=1 : replay automatically at game start
//
// Times are stored relative to DLL load (game start), so waits for intros and loading are reproduced.
// Mouse positions are stored relative to the game window's client area (0..1), so window moves/resizes are fine.
// Replay uses SendInput with scan codes because the game reads the keyboard through DirectInput.
#include <windows.h>
#include <stdio.h>
#include <vector>
#include "vr.h"

static LogFn Log;
static char s_path[MAX_PATH];
static LARGE_INTEGER s_qpf, s_t0;
static volatile bool s_recording, s_replaying, s_abort;
static double s_lastMove;

struct Ev { double t; char type; int a, b, c; };   // K: scan, flags(1=up,2=ext), vk | M: x*10000, y*10000 | B: button, down | W: delta
static std::vector<Ev> s_rec;
static CRITICAL_SECTION s_cs;

static double NowS() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return double(t.QuadPart - s_t0.QuadPart) / s_qpf.QuadPart; }

static HWND GameWindow() {
    struct F { HWND best; LONG area; } f = {nullptr, 0};
    EnumWindows([](HWND w, LPARAM p) -> BOOL {
        DWORD pid; GetWindowThreadProcessId(w, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(w)) return TRUE;
        RECT r; GetClientRect(w, &r); LONG a = (r.right - r.left) * (r.bottom - r.top);
        F* f = (F*)p; if (a > f->area) { f->area = a; f->best = w; }
        return TRUE;
    }, (LPARAM)&f);
    return f.best;
}
static bool GameHasFocus() {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static void Save() {
    FILE* f; if (fopen_s(&f, s_path, "w")) return;
    fprintf(f, "# p2vr input macro: time_s type a b c\n");
    EnterCriticalSection(&s_cs);
    for (auto& e : s_rec) fprintf(f, "%.4f %c %d %d %d\n", e.t, e.type, e.a, e.b, e.c);
    LeaveCriticalSection(&s_cs);
    fclose(f);
}
static bool Load(std::vector<Ev>& out) {
    FILE* f; if (fopen_s(&f, s_path, "r")) return false;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        Ev e; if (sscanf_s(line, "%lf %c %d %d %d", &e.t, &e.type, 1, &e.a, &e.b, &e.c) == 5) out.push_back(e);
    }
    fclose(f); return !out.empty();
}

static void Add(char type, int a, int b, int c) {
    EnterCriticalSection(&s_cs); s_rec.push_back({NowS(), type, a, b, c}); LeaveCriticalSection(&s_cs);
}

static DWORD WINAPI ReplayThread(LPVOID) {
    std::vector<Ev> ev;
    if (!Load(ev)) { Log("[macro] nothing to replay (%s)", s_path); s_replaying = false; return 0; }
    Log("[macro] replay started: %zu events, %.1f s", ev.size(), ev.back().t);
    bool down[256] = {};
    double shift = 0; int clicks = 0;
    for (auto& e : ev) {
        while (NowS() < e.t + shift) { if (s_abort) break; Sleep(2); }
        if (s_abort) break;
        if (!GameHasFocus()) {   // never inject into another window: pause and shift the remaining timeline
            double p0 = NowS(); Log("[macro] paused at %.1f s: game window not in front", e.t);
            while (!GameHasFocus() && !s_abort) Sleep(20);
            if (s_abort) break;
            Sleep(300); shift += NowS() - p0; Log("[macro] resumed after %.1f s", NowS() - p0);
        }
        if (e.type == 'B' && e.b) Log("[macro] click %d at %.1f s (button %d)", ++clicks, e.t + shift, e.a);
        if (e.type == 'K' && !(e.b & 1)) Log("[macro] key vk %d at %.1f s", e.c, e.t + shift);
        INPUT in = {};
        if (e.type == 'K') {
            in.type = INPUT_KEYBOARD; in.ki.wScan = (WORD)e.a;
            in.ki.dwFlags = KEYEVENTF_SCANCODE | ((e.b & 1) ? KEYEVENTF_KEYUP : 0) | ((e.b & 2) ? KEYEVENTF_EXTENDEDKEY : 0);
            if (e.a < 256) down[e.a] = !(e.b & 1);
        } else if (e.type == 'M') {
            HWND w = GameWindow(); if (!w) continue;
            RECT r; GetClientRect(w, &r); POINT p = {LONG(e.a / 10000.0 * r.right), LONG(e.b / 10000.0 * r.bottom)};
            ClientToScreen(w, &p);
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            in.type = INPUT_MOUSE;
            in.mi.dx = LONG((p.x - vx) * 65535.0 / (vw - 1)); in.mi.dy = LONG((p.y - vy) * 65535.0 / (vh - 1));
            in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        } else if (e.type == 'B') {
            static const DWORD dn[3] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN};
            static const DWORD up[3] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP};
            if (e.a < 0 || e.a > 2) continue;
            in.type = INPUT_MOUSE; in.mi.dwFlags = e.b ? dn[e.a] : up[e.a];
        } else if (e.type == 'W') {
            in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_WHEEL; in.mi.mouseData = (DWORD)e.a;
        } else continue;
        SendInput(1, &in, sizeof in);
    }
    for (int sc = 0; sc < 256; sc++) if (down[sc]) {   // never leave keys stuck
        INPUT in = {}; in.type = INPUT_KEYBOARD; in.ki.wScan = (WORD)sc; in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP; SendInput(1, &in, sizeof in);
    }
    Log("[macro] replay %s", s_abort ? "aborted" : "finished");
    s_replaying = false; s_abort = false;
    return 0;
}

static void StartReplay() {
    if (s_replaying) { s_abort = true; return; }
    s_replaying = true; s_abort = false;
    CreateThread(nullptr, 0, ReplayThread, nullptr, 0, nullptr);
}

static LRESULT CALLBACK KbHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        bool injected = (k->flags & LLKHF_INJECTED) != 0, up = (k->flags & LLKHF_UP) != 0;
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (!injected && !up && ctrl && GameHasFocus() && (k->vkCode == VK_F2 || k->vkCode == VK_F3)) {
            if (k->vkCode == VK_F2) {
                if (!s_recording) {
                    EnterCriticalSection(&s_cs); s_rec.clear(); LeaveCriticalSection(&s_cs);
                    s_recording = true; Log("[macro] recording (t=%.1f s since game start)", NowS());
                } else {
                    s_recording = false;
                    // drop the Ctrl press that belongs to this stop hotkey
                    EnterCriticalSection(&s_cs);
                    while (!s_rec.empty() && s_rec.back().type == 'K' && (s_rec.back().c == VK_CONTROL || s_rec.back().c == VK_LCONTROL || s_rec.back().c == VK_RCONTROL)) s_rec.pop_back();
                    size_t n = s_rec.size(); LeaveCriticalSection(&s_cs);
                    Save(); Log("[macro] saved %zu events to %s", n, s_path);
                }
            } else StartReplay();
            return 1;   // swallow the hotkey
        }
        if (s_recording && !injected && GameHasFocus() && k->vkCode != VK_F2 && k->vkCode != VK_F3)
            Add('K', (int)k->scanCode, (up ? 1 : 0) | ((k->flags & LLKHF_EXTENDED) ? 2 : 0), (int)k->vkCode);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && s_recording) {
        auto* m = (MSLLHOOKSTRUCT*)lp;
        if (!(m->flags & LLMHF_INJECTED) && GameHasFocus()) {
            HWND w = GameWindow();
            if (wp == WM_MOUSEMOVE) {
                double t = NowS();
                if (w && t - s_lastMove > 0.015) {
                    s_lastMove = t;
                    POINT p = m->pt; ScreenToClient(w, &p); RECT r; GetClientRect(w, &r);
                    if (r.right > 0 && r.bottom > 0) Add('M', int(p.x * 10000.0 / r.right), int(p.y * 10000.0 / r.bottom), 0);
                }
            } else if (wp == WM_LBUTTONDOWN || wp == WM_LBUTTONUP || wp == WM_RBUTTONDOWN || wp == WM_RBUTTONUP ||
                       wp == WM_MBUTTONDOWN || wp == WM_MBUTTONUP) {
                if (w) {   // make sure the pointer position right before a click is recorded
                    POINT p = m->pt; ScreenToClient(w, &p); RECT r; GetClientRect(w, &r);
                    if (r.right > 0 && r.bottom > 0) Add('M', int(p.x * 10000.0 / r.right), int(p.y * 10000.0 / r.bottom), 0);
                }
                int btn = (wp == WM_LBUTTONDOWN || wp == WM_LBUTTONUP) ? 0 : (wp == WM_RBUTTONDOWN || wp == WM_RBUTTONUP) ? 1 : 2;
                bool dn = wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN;
                Add('B', btn, dn ? 1 : 0, 0);
            } else if (wp == WM_MOUSEWHEEL) {
                Add('W', (short)HIWORD(m->mouseData), 0, 0);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static DWORD WINAPI HookThread(LPVOID autoplay) {
    HHOOK kb = SetWindowsHookExA(WH_KEYBOARD_LL, KbHook, GetModuleHandleA(nullptr), 0);
    HHOOK ms = SetWindowsHookExA(WH_MOUSE_LL, MouseHook, GetModuleHandleA(nullptr), 0);
    Log("[macro] input hooks %s (Ctrl+F2 record, Ctrl+F3 replay)%s", kb && ms ? "ok" : "FAILED", autoplay ? ", autoplay on" : "");
    if (autoplay && GetFileAttributesA(s_path) != INVALID_FILE_ATTRIBUTES) StartReplay();
    MSG msg; while (GetMessageA(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}

void Macro_Start(const char* dir, bool autoplay, LogFn log) {
    Log = log;
    InitializeCriticalSection(&s_cs);
    QueryPerformanceFrequency(&s_qpf); QueryPerformanceCounter(&s_t0);
    sprintf_s(s_path, "%s\\macro.txt", dir);
    CreateThread(nullptr, 0, HookThread, (LPVOID)(intptr_t)(autoplay ? 1 : 0), 0, nullptr);
}
