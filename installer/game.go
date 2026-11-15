package main

// The game on this computer: finding Privateer 2: The Darkening (30th Anniversary Edition) in the Steam libraries (or GOG),
// checking a folder chosen by the user, and what of the VR mod is installed there.
//
// The mod only adds files next to the game's DARK.EXE (in the edition's "Windows" folder); no file of the game is changed:
//   DARKVR.EXE         a copy of DARK.EXE whose import P2DEN.dll is renamed P2VR.dll (made here, from the user's own DARK.EXE)
//   P2VR.dll           the mod; it forwards to the edition's p2den.dll
//   openxr_loader.dll  Khronos' OpenXR loader
//   p2vr.ini           the settings (kept on update; new settings are added to it, p2vr.default.ini has all defaults)
//   P2VR.VERSION       the installed version
//   P2VR-README.md, P2VR-SETTINGS.md
//   P2VR-Launcher.exe  this app, from the release: the "Privateer 2 VR" shortcut opens it to play in VR or flat (and update)

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"unsafe"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

const (
	steamAppID   = "3067130"
	gameExe      = "DARK.EXE"
	vrExe        = "DARKVR.EXE"
	modDll       = "P2VR.dll"
	versionFile  = "P2VR.VERSION"
	settingsFile = "p2vr.ini"
	defaultsFile = "p2vr.default.ini"
	launcherExe  = "P2VR-Launcher.exe"
)

// files the mod adds (removed on uninstall; p2vr.ini only when asked)
var modFiles = []string{vrExe, modDll, "openxr_loader.dll", versionFile, defaultsFile, "P2VR-README.md", "P2VR-SETTINGS.md", "p2vr.log", launcherExe, launcherExe + ".old",
	"bhaptics_library.dll", "bHaptics-LICENSE.txt", "bHaptics-THIRD_PARTY_NOTICES.txt"}

type gameInfo struct {
	Path        string   `json:"path"`              // the folder of DARK.EXE
	Source      string   `json:"source"`            // Steam, GOG, chosen
	Valid       bool     `json:"valid"`             // a usable game folder
	Problem     string   `json:"problem,omitempty"` // why not
	Installed   bool     `json:"installed"`
	Version     string   `json:"version,omitempty"`
	NeedsRepair string   `json:"needsRepair,omitempty"` // installed, but something must be redone
	Running     bool     `json:"running"`
	Candidates  []string `json:"candidates,omitempty"` // every game folder found
}

// ---- finding the game

func regString(root registry.Key, path, name string) string {
	k, err := registry.OpenKey(root, path, registry.QUERY_VALUE|registry.WOW64_32KEY)
	if err != nil {
		return ""
	}
	defer k.Close()
	v, _, _ := k.GetStringValue(name)
	return v
}

func steamRoots() []string {
	var roots []string
	seen := map[string]bool{}
	for _, r := range []string{
		regString(registry.CURRENT_USER, `Software\Valve\Steam`, "SteamPath"),
		regString(registry.LOCAL_MACHINE, `SOFTWARE\WOW6432Node\Valve\Steam`, "InstallPath"),
		regString(registry.LOCAL_MACHINE, `SOFTWARE\Valve\Steam`, "InstallPath"),
		`C:\Program Files (x86)\Steam`,
	} {
		if r == "" {
			continue
		}
		r = filepath.Clean(strings.ReplaceAll(r, "/", `\`))
		if !seen[strings.ToLower(r)] {
			seen[strings.ToLower(r)] = true
			roots = append(roots, r)
		}
	}
	return roots
}

var vdfPath = regexp.MustCompile(`"path"\s+"([^"]+)"`)
var acfInstallDir = regexp.MustCompile(`"installdir"\s+"([^"]+)"`)

func steamLibraries() []string {
	var libs []string
	seen := map[string]bool{}
	add := func(p string) {
		p = filepath.Clean(strings.ReplaceAll(p, `\\`, `\`))
		if !seen[strings.ToLower(p)] {
			seen[strings.ToLower(p)] = true
			libs = append(libs, p)
		}
	}
	for _, root := range steamRoots() {
		if _, err := os.Stat(filepath.Join(root, "steamapps")); err != nil {
			continue
		}
		add(root)
		data, err := os.ReadFile(filepath.Join(root, "steamapps", "libraryfolders.vdf"))
		if err != nil {
			continue
		}
		for _, m := range vdfPath.FindAllStringSubmatch(string(data), -1) {
			add(m[1])
		}
	}
	return libs
}

// findGames: every folder with the edition's DARK.EXE, Steam first
func findGames() (paths []string, sources map[string]string) {
	sources = map[string]string{}
	add := func(dir, source string) {
		if dir, ok := normalizeGameDir(dir); ok {
			if _, dup := sources[strings.ToLower(dir)]; !dup {
				sources[strings.ToLower(dir)] = source
				paths = append(paths, dir)
			}
		}
	}
	for _, lib := range steamLibraries() {
		installDir := "Privateer 2 The Darkening"
		if data, err := os.ReadFile(filepath.Join(lib, "steamapps", "appmanifest_"+steamAppID+".acf")); err == nil {
			if m := acfInstallDir.FindStringSubmatch(string(data)); m != nil {
				installDir = m[1]
			}
		}
		add(filepath.Join(lib, "steamapps", "common", installDir), "Steam")
	}
	// GOG Galaxy / GOG installers
	if k, err := registry.OpenKey(registry.LOCAL_MACHINE, `SOFTWARE\WOW6432Node\GOG.com\Games`, registry.ENUMERATE_SUB_KEYS|registry.WOW64_32KEY); err == nil {
		names, _ := k.ReadSubKeyNames(-1)
		k.Close()
		for _, id := range names {
			base := `SOFTWARE\WOW6432Node\GOG.com\Games\` + id
			if name := regString(registry.LOCAL_MACHINE, base, "gameName"); strings.Contains(strings.ToLower(name), "privateer 2") {
				add(regString(registry.LOCAL_MACHINE, base, "path"), "GOG")
			}
		}
	}
	return paths, sources
}

// normalizeGameDir: the folder of DARK.EXE for a folder the user points at (the game's folder or its "Windows" folder)
func normalizeGameDir(dir string) (string, bool) {
	if dir == "" {
		return "", false
	}
	dir = filepath.Clean(dir)
	for _, d := range []string{filepath.Join(dir, "Windows"), dir} {
		if st, err := os.Stat(filepath.Join(d, gameExe)); err == nil && !st.IsDir() {
			return d, true
		}
	}
	return dir, false
}

// ---- what is in a folder

func inspectGame(dir, source string) gameInfo {
	info := gameInfo{Path: dir, Source: source}
	d, ok := normalizeGameDir(dir)
	if !ok {
		info.Problem = "No DARK.EXE in this folder. Choose the folder of Privateer 2: The Darkening (or its \"Windows\" folder)."
		return info
	}
	info.Path = d
	if _, err := os.Stat(filepath.Join(d, "p2den.dll")); err != nil {
		info.Problem = "This is not the 30th Anniversary Edition (p2den.dll is missing). The VR mod needs the Steam edition of Privateer 2: The Darkening."
		return info
	}
	exe, err := os.ReadFile(filepath.Join(d, gameExe))
	if err != nil {
		info.Problem = "DARK.EXE cannot be read: " + err.Error()
		return info
	}
	if _, err := patchExe(exe); err != nil {
		info.Problem = err.Error()
		return info
	}
	info.Valid = true
	info.Running = processRunning(gameExe, vrExe)
	_, dllErr := os.Stat(filepath.Join(d, modDll))
	vr, vrErr := os.ReadFile(filepath.Join(d, vrExe))
	if dllErr != nil && vrErr != nil {
		return info
	}
	info.Installed = true
	if v, err := os.ReadFile(filepath.Join(d, versionFile)); err == nil {
		info.Version = strings.TrimSpace(string(v))
	} else {
		info.Version = "unknown"
	}
	switch {
	case dllErr != nil:
		info.NeedsRepair = "P2VR.dll is missing."
	case vrErr != nil:
		info.NeedsRepair = "DARKVR.EXE is missing."
	default:
		if want, _ := patchExe(exe); !bytes.Equal(want, vr) {
			info.NeedsRepair = "The game was updated since the mod was installed: DARKVR.EXE must be made again."
		} else if _, err := os.Stat(filepath.Join(d, "openxr_loader.dll")); err != nil {
			info.NeedsRepair = "openxr_loader.dll is missing."
		}
	}
	return info
}

// patchExe: DARKVR.EXE = DARK.EXE with its import "P2DEN.dll" renamed "P2VR.dll" (same slot, NUL padded)
func patchExe(exe []byte) ([]byte, error) {
	from, to := []byte("P2DEN.dll\x00"), []byte("P2VR.dll\x00\x00")
	switch bytes.Count(exe, from) {
	case 1:
		return bytes.Replace(exe, from, to, 1), nil
	case 0:
		if bytes.Contains(exe, []byte("P2VR.dll\x00\x00")) {
			return nil, errors.New("DARK.EXE is already patched (it loads P2VR.dll): restore the original file, e.g. with Steam's \"Verify integrity of game files\".")
		}
		return nil, errors.New("This DARK.EXE does not load p2den.dll: it is not the 30th Anniversary Edition the mod was made for.")
	default:
		return nil, errors.New("This DARK.EXE is not the version the mod was made for.")
	}
}

// ---- processes

func processRunning(names ...string) bool {
	snap, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return false
	}
	defer windows.CloseHandle(snap)
	var e windows.ProcessEntry32
	e.Size = uint32(unsafe.Sizeof(e))
	for err = windows.Process32First(snap, &e); err == nil; err = windows.Process32Next(snap, &e) {
		exe := windows.UTF16ToString(e.ExeFile[:])
		for _, n := range names {
			if strings.EqualFold(exe, n) {
				return true
			}
		}
	}
	return false
}

// ---- settings file: keep the user's values, add settings that are new in this version

var iniKey = regexp.MustCompile(`^\s*([A-Za-z0-9_]+)\s*=`)

// Defaults shipped by earlier releases and changed since (e.g. a panel moved so that it no longer cuts "OBSERVER VIEW").
// A player's value equal to one of these was never chosen by them: it is replaced by the new default on update. Values
// are compared without spaces. Add the old value here whenever a default in dist/p2vr.ini changes.
var oldDefaults = map[string][]string{
	"PANEL_VELOCITY":      {"236,4,406,58,0,24,0.36"},     // v0.1.0
	"PANEL_COMMS":         {"0,0,124,140,-34,22,0.24"},    // v0.1.0 - v0.1.1
	"PANEL_RADAR":         {"254,346,388,478,0,-28,0.30"}, // v0.1.0 - v0.1.1
	"PANEL_MISSILES":      {"516,0,640,140,34,22,0.24"},   // v0.1.0
	"RETICLE_GLASS_ALPHA": {"0.7"},                        // v0.1.0 - v0.1.2
}

// iniValue: a line's value without its comment and spaces
func iniValue(l string) string {
	v := l[strings.Index(l, "=")+1:]
	if i := strings.Index(v, ";"); i >= 0 {
		v = v[:i]
	}
	return strings.Join(strings.Fields(v), "")
}

// updateOldDefaults replaces the values of old that are an earlier release's default by fresh's line for that setting
func updateOldDefaults(old, fresh string) (string, []string) {
	freshLine := map[string]string{}
	for _, l := range strings.Split(strings.ReplaceAll(fresh, "\r\n", "\n"), "\n") {
		if m := iniKey.FindStringSubmatch(l); m != nil {
			freshLine[strings.ToUpper(m[1])] = strings.TrimSpace(l)
		}
	}
	var changed []string
	lines := strings.Split(old, "\n")
	for i, l := range lines {
		m := iniKey.FindStringSubmatch(l)
		if m == nil {
			continue
		}
		k := strings.ToUpper(m[1])
		nl, ok := freshLine[k]
		if !ok || iniValue(nl) == iniValue(l) {
			continue
		}
		for _, d := range oldDefaults[k] {
			if iniValue(l) == d {
				cr := strings.HasSuffix(l, "\r")
				lines[i] = nl
				if cr {
					lines[i] += "\r"
				}
				changed = append(changed, k)
				break
			}
		}
	}
	return strings.Join(lines, "\n"), changed
}

// refreshOldDefaults applies updateOldDefaults to the game folder's p2vr.ini, against the p2vr.default.ini of the
// installed version. Run on "Play in VR" too: an update done by an older launcher (which replaces itself and restarts
// only at the end) does not have this step. Returns the settings it changed.
func refreshOldDefaults(dir string) ([]string, error) {
	fresh, err := os.ReadFile(filepath.Join(dir, defaultsFile))
	if err != nil {
		return nil, nil // (installed by a version without p2vr.default.ini: nothing to compare with)
	}
	path := filepath.Join(dir, settingsFile)
	old, err := os.ReadFile(path)
	if err != nil {
		return nil, nil
	}
	updated, changed := updateOldDefaults(string(old), string(fresh))
	if len(changed) == 0 {
		return nil, nil
	}
	if err := writeFile(path, []byte(updated)); err != nil {
		return nil, err
	}
	return changed, nil
}

func mergeSettings(old, fresh, version string) (string, int) {
	have := map[string]bool{}
	lastSection := ""
	for _, l := range strings.Split(old, "\n") {
		if m := iniKey.FindStringSubmatch(l); m != nil {
			have[strings.ToUpper(m[1])] = true
		}
		if t := strings.TrimSpace(l); strings.HasPrefix(t, "[") {
			lastSection = strings.ToUpper(t)
		}
	}
	var add []string
	var comments []string
	for _, l := range strings.Split(strings.ReplaceAll(fresh, "\r\n", "\n"), "\n") {
		t := strings.TrimSpace(l)
		switch {
		case strings.HasPrefix(t, ";"):
			comments = append(comments, l)
		case iniKey.MatchString(l):
			k := strings.ToUpper(iniKey.FindStringSubmatch(l)[1])
			if !have[k] {
				add = append(add, comments...)
				add = append(add, l)
			}
			comments = nil
		default:
			comments = nil
		}
	}
	added := 0
	for _, l := range add {
		if iniKey.MatchString(l) {
			added++
		}
	}
	if added == 0 {
		return old, 0
	}
	out := strings.TrimRight(old, "\r\n") + "\r\n"
	if lastSection != "[VR]" {
		out += "[VR]\r\n"
	}
	out += fmt.Sprintf("; ---- settings added by the installer (version %s) ----\r\n", version)
	out += strings.Join(add, "\r\n") + "\r\n"
	return out, added
}
