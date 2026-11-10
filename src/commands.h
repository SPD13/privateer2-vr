// commands.h - every game command exposed as a SteamVR action (remappable in SteamVR's controller binding screen).
// Each command presses the game's own key (DirectInput scancode, optionally with left Alt), overridable in
// p2vr.ini as KEY_<NAME>=<scancode>. Default bindings exist only for the most used commands.
#pragma once
#include <stdint.h>

struct VrCommand {
    const char* action;     // OpenXR action name (lowercase)
    const char* label;      // shown in SteamVR's binding screen
    uint8_t scancode;       // game key
    bool alt;               // press with left Alt
    const char* indexPath;  // default binding (Index-style controllers), or nullptr
    const char* touchPath;  // default binding (Touch-style controllers), or nullptr
    const char* framePath;  // default binding (Steam Frame controllers, XR_VALVE_frame_controller_interaction), or nullptr
};

static const VrCommand kVrCommands[] = {
    // --- weapons
    {"fire_guns",          "Fire guns",                        0x39, false, "/user/hand/right/input/trigger/value", "/user/hand/right/input/trigger/value", "/user/hand/right/input/trigger/value"},
    {"fire_missile",       "Fire missile",                     0x1C, false, "/user/hand/left/input/trigger/value",  "/user/hand/left/input/trigger/value", "/user/hand/left/input/trigger/value"},
    {"cycle_weapon",       "Cycle weapons in VDW",             0x23, false, "/user/hand/left/input/a/click",        "/user/hand/left/input/x/click", "/user/hand/left/input/dpad_right/click"},
    {"select_weapon",      "Select / deselect weapon in VDW",  0x31, false, nullptr, nullptr, "/user/hand/left/input/dpad_down/click"},
    {"toggle_guns",        "Toggle full guns / no guns",       0x21, false, nullptr, nullptr, nullptr},
    {"guns_vdw",           "Toggle guns VDW",                  0x22, false, nullptr, nullptr, nullptr},
    {"missile_vdw",        "Toggle missile VDW",               0x32, false, nullptr, nullptr, "/user/hand/right/input/y/click"},
    {"mines_vdw",          "Toggle mines & decoys VDW",        0x30, false, nullptr, nullptr, nullptr},
    {"drop_mines",         "Drop mines / decoys",              0x0E, false, nullptr, nullptr, "/user/hand/left/input/bumper/click"},
    // --- flight
    {"afterburner",        "Afterburner",                      0x0F, false, "/user/hand/right/input/squeeze/value", "/user/hand/right/input/squeeze/value", "/user/hand/right/input/squeeze/value"},
    {"match_speed",        "Match target speed",               0x2D, false, "/user/hand/left/input/squeeze/value",  "/user/hand/left/input/squeeze/value", "/user/hand/left/input/squeeze/value"},
    {"speed_zero",         "Speed to zero",                    0x1A, false, "/user/hand/left/input/thumbstick/click", "/user/hand/left/input/thumbstick/click", "/user/hand/left/input/thumbstick/click"},
    {"speed_max",          "Speed to maximum",                 0x1B, false, nullptr, nullptr, "/user/hand/left/input/dpad_up/click"},
    {"speed_up",           "Increase speed",                   0x0D, false, nullptr, nullptr, nullptr},
    {"speed_down",         "Decrease speed",                   0x0C, false, nullptr, nullptr, nullptr},
    {"jump",               "Jump",                             0x24, false, "/user/hand/left/input/b/click",        "/user/hand/left/input/y/click", "/user/hand/right/input/menu/click"},
    // --- targeting
    {"cycle_target",       "Cycle targets",                    0x1E, false, "/user/hand/right/input/a/click",       "/user/hand/right/input/a/click", "/user/hand/right/input/a/click"},
    {"cycle_target_back",  "Cycle targets backward",           0x2C, false, nullptr, nullptr, "/user/hand/right/input/b/click"},
    {"target_hostile",     "Target nearest hostile",           0x11, false, "/user/hand/right/input/b/click",       "/user/hand/right/input/b/click", "/user/hand/right/input/bumper/click"},
    {"target_friendly",    "Target nearest friendly",          0x1F, false, nullptr, nullptr, nullptr},
    {"radar_range",        "Cycle radar ranges",               0x12, false, nullptr, nullptr, nullptr},
    {"radar_type",         "Toggle radar type",                0x13, true,  nullptr, nullptr, nullptr},
    {"targeting_off",      "Targeting off",                    0x14, true,  nullptr, nullptr, nullptr},
    // --- systems
    {"tractor",            "Tractor targeted object",          0x14, false, nullptr, nullptr, nullptr},
    {"jettison",           "Jettison / drop tractored object", 0x15, false, nullptr, nullptr, nullptr},
    {"sos",                "Toggle SOS receiver",              0x18, false, nullptr, nullptr, nullptr},
    {"warp_shields",       "Activate warp shields",            0x11, true,  nullptr, nullptr, nullptr},
    {"bse",                "Activate BSE system",              0x30, true,  nullptr, nullptr, nullptr},
    {"nuke",               "Drop Nuke 'em",                    0x1F, true,  nullptr, nullptr, nullptr},
    // --- screens
    {"comms",              "Comms VDW",                        0x2E, false, nullptr, nullptr, "/user/hand/right/input/x/click"},
    {"damage_vdw",         "Damage VDW",                       0x20, false, nullptr, nullptr, "/user/hand/left/input/dpad_left/click"},
    {"nav_map",            "Open nav map",                     0x31, true,  nullptr, nullptr, "/user/hand/left/input/view/click"},
    {"diary",              "Open electronic diary",            0x20, true,  nullptr, nullptr, nullptr},
    {"hotkeys",            "Hotkey list",                      0x23, true,  nullptr, nullptr, nullptr},
    {"options",            "Game options",                     0x18, true,  nullptr, nullptr, nullptr},
    {"escape",             "Escape (exit nav map / diary)",    0x01, false, nullptr, nullptr, nullptr},
    // --- views
    {"view_front",         "Front view",                       0x3B, false, nullptr, nullptr, nullptr},
    {"view_left",          "Left view",                        0x3C, false, nullptr, nullptr, nullptr},
    {"view_back",          "Back view",                        0x3D, false, nullptr, nullptr, nullptr},
    {"view_right",         "Right view",                       0x3E, false, nullptr, nullptr, nullptr},
    {"view_ship",          "Exterior ship view",               0x3F, false, nullptr, nullptr, nullptr},
    {"view_target_track",  "Target track camera",              0x40, false, nullptr, nullptr, nullptr},
    {"view_target_ext",    "External view of target",          0x41, false, nullptr, nullptr, nullptr},
    {"view_target_chase",  "Target chase camera",              0x42, false, nullptr, nullptr, nullptr},
    {"view_fixed",         "Fixed camera",                     0x43, false, nullptr, nullptr, nullptr},
    {"view_flyby",         "Fly-by cinematic",                 0x44, false, nullptr, nullptr, nullptr},
    // --- cheats (scancode 0: run by p2vr, see RunCheats; no default binding)
    {"cheat_restore",      "[CHEAT] Restore shield and armor", 0, false, nullptr, nullptr, nullptr},
    {"cheat_destroy_target", "[CHEAT] Destroy target",         0, false, nullptr, nullptr, nullptr},
    {"cheat_destroy_enemies", "[CHEAT] Destroy all enemies",   0, false, nullptr, nullptr, nullptr},
    {"cheat_cash",         "[CHEAT] Get cash (+100,000 credits)", 0, false, nullptr, nullptr, nullptr},
    {"cheat_invincible",   "[CHEAT] Invincible (toggle)",      0, false, nullptr, nullptr, nullptr},
    {"cheat_jump_direct",  "[CHEAT] Jump straight to destination", 0, false, nullptr, nullptr, nullptr},
};
static const int kVrCommandCount = (int)(sizeof kVrCommands / sizeof kVrCommands[0]);
static_assert(sizeof kVrCommands / sizeof kVrCommands[0] <= 64, "commands are a 64-bit mask");
