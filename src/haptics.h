// bHaptics TactSuit support (haptics.cpp). Everything is a no-op unless a suit is usable: bhaptics_library.dll in the
// game folder, bHaptics Player installed, credentials present and BHAPTICS not 0. All SDK calls run on one worker
// thread; the functions below only queue work and return at once.
#pragma once
#include <stdint.h>

extern volatile bool g_hapticsOn;          // a TactSuit is connected and in use (cheap check before any detection work)

void Haptics_Init(const char* iniPath, const char* gameDir, void (*log)(const char*, ...));
void Haptics_Shutdown();                   // stop everything; never waits (safe from DLL_PROCESS_DETACH)
void Haptics_Calibrate();                  // test sequence to map the suit's motors (Ctrl+0, or a 'hapticstest' file)
void Haptics_StopAll();

// game events (ship space: azimuth + = right, elevation + = up, degrees; 0/0 = straight ahead)
void Haptics_Shot(float strength);
void Haptics_Hit(float azDeg, float elDeg, float strength);
void Haptics_Missile();
void Haptics_Afterburner(bool on);
void Haptics_Jump();
void Haptics_Land();
void Haptics_Death();
void Haptics_Heartbeat(float level);       // 0 = off, 0..1 = faster / stronger as the hull drops
