package main

// End-to-end test of install / update / uninstall on a scratch game folder (copies of DARK.EXE and p2den.dll only):
//   set P2VR_TEST_GAME=<folder with DARK.EXE and p2den.dll>  (e.g. the game's "Windows" folder; it is only read)
//   set P2VR_TEST_ARCHIVE=<privateer2-vr-X.Y.Z.zip>
//   go test -run TestInstall -v
// Skipped without those variables. No shortcuts are made.

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func copyFile(t *testing.T, from, to string) {
	data, err := os.ReadFile(from)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(to, data, 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestInstall(t *testing.T) {
	src, archive := os.Getenv("P2VR_TEST_GAME"), os.Getenv("P2VR_TEST_ARCHIVE")
	if src == "" || archive == "" {
		t.Skip("P2VR_TEST_GAME / P2VR_TEST_ARCHIVE not set")
	}
	dir := filepath.Join(t.TempDir(), "Privateer 2 The Darkening", "Windows")
	os.MkdirAll(dir, 0o755)
	copyFile(t, filepath.Join(src, gameExe), filepath.Join(dir, gameExe))
	copyFile(t, filepath.Join(src, "p2den.dll"), filepath.Join(dir, "p2den.dll"))
	original, _ := os.ReadFile(filepath.Join(dir, gameExe))

	// the parent folder is accepted too
	info := inspectGame(filepath.Dir(dir), "chosen")
	if !info.Valid || info.Installed || info.Path != dir {
		t.Fatalf("fresh folder: %+v", info)
	}

	a := newApp("t", dir, archive)
	a.state.Game = info
	a.install(false)
	if a.state.Job.Failed {
		t.Fatalf("install failed: %s %+v", a.state.Job.Message, a.state.Job.Lines)
	}
	for _, l := range a.state.Job.Lines {
		t.Logf("  %s: %s", l.Kind, l.Text)
	}
	info = a.state.Game
	if !info.Installed || info.NeedsRepair != "" || info.Version != a.state.Latest.Version {
		t.Fatalf("after install: %+v", info)
	}
	if now, _ := os.ReadFile(filepath.Join(dir, gameExe)); string(now) != string(original) {
		t.Fatal("DARK.EXE was changed")
	}

	// update: the user's settings are kept, a removed setting is added back
	ini := filepath.Join(dir, settingsFile)
	data, _ := os.ReadFile(ini)
	s := strings.Replace(string(data), "RUMBLE_GUNS=0.35", "RUMBLE_GUNS=0.10", 1)
	s = strings.Replace(s, "HUD_ARROW_LAYER=1", "", 1)
	os.WriteFile(ini, []byte(s), 0o644)
	os.WriteFile(filepath.Join(dir, versionFile), []byte("0.0.1"), 0o644)
	a.refreshGame()
	if !a.state.UpdateAvailable {
		t.Fatalf("update not offered: %+v", a.state.Game)
	}
	a.install(false)
	if a.state.Job.Failed {
		t.Fatalf("update failed: %s", a.state.Job.Message)
	}
	data, _ = os.ReadFile(ini)
	if !strings.Contains(string(data), "RUMBLE_GUNS=0.10") || !strings.Contains(string(data), "HUD_ARROW_LAYER=1") {
		t.Fatalf("settings not merged:\n%s", data)
	}

	// repair: a changed DARK.EXE (game update) is detected
	os.WriteFile(filepath.Join(dir, vrExe), []byte("old"), 0o644)
	a.refreshGame()
	if a.state.Game.NeedsRepair == "" {
		t.Fatal("broken DARKVR.EXE not detected")
	}

	// uninstall keeps the settings unless asked
	a.uninstall(false)
	if a.state.Job.Failed || a.state.Game.Installed {
		t.Fatalf("uninstall: %s %+v", a.state.Job.Message, a.state.Game)
	}
	if _, err := os.Stat(ini); err != nil {
		t.Fatal("settings removed")
	}
	left, _ := os.ReadDir(dir)
	var names []string
	for _, e := range left {
		names = append(names, e.Name())
	}
	t.Logf("left after uninstall: %v", names)
	if len(names) != 3 {
		t.Fatalf("unexpected files left: %v", names)
	}
}

// An update run by the launcher in the game folder replaces that launcher: the new one must take over (it runs the
// new version's steps, e.g. the bHaptics files); an update that does not change the running launcher must not restart.
func TestInstallRestartsReplacedLauncher(t *testing.T) {
	src, archive := os.Getenv("P2VR_TEST_GAME"), os.Getenv("P2VR_TEST_ARCHIVE")
	if src == "" || archive == "" {
		t.Skip("P2VR_TEST_GAME / P2VR_TEST_ARCHIVE not set")
	}
	dir := filepath.Join(t.TempDir(), "Windows")
	os.MkdirAll(dir, 0o755)
	copyFile(t, filepath.Join(src, gameExe), filepath.Join(dir, gameExe))
	copyFile(t, filepath.Join(src, "p2den.dll"), filepath.Join(dir, "p2den.dll"))
	launcher := filepath.Join(dir, launcherExe)
	os.WriteFile(launcher, []byte("the old launcher"), 0o755)

	var started []string
	exited := -1
	selfExe = func() string { return launcher }
	launchLauncher = func(exe, d string) error { started = append(started, exe, d); return nil }
	exitProcess = func(code int) { exited = code }
	defer func() { selfExe, launchLauncher, exitProcess = thisExe, startLauncher, os.Exit }()

	a := newApp("t", dir, archive)
	a.state.Game = inspectGame(dir, "chosen")
	a.install(false)
	if a.state.Job.Failed {
		t.Fatalf("install failed: %s", a.state.Job.Message)
	}
	if len(started) != 2 || !strings.EqualFold(started[0], launcher) || started[1] != dir || exited != 0 {
		t.Fatalf("replaced launcher: started %v, exit %d", started, exited)
	}
	if _, err := os.Stat(launcher + ".old"); err != nil {
		t.Fatalf("the old launcher was not kept as .old: %v", err)
	}

	// the same version again: the running launcher is unchanged, no restart
	started, exited = nil, -1
	a.install(false)
	if a.state.Job.Failed || started != nil || exited != -1 {
		t.Fatalf("unchanged launcher: failed %v, started %v, exit %d", a.state.Job.Failed, started, exited)
	}
}

func TestFindGames(t *testing.T) {
	paths, sources := findGames()
	for _, p := range paths {
		t.Logf("%s (%s): %+v", p, sources[strings.ToLower(p)], inspectGame(p, ""))
	}
}

func TestNewer(t *testing.T) {
	for _, c := range []struct {
		a, b string
		want bool
	}{{"0.1.0", "0.0.9", true}, {"0.1.0", "0.1.0", false}, {"0.10.0", "0.9.9", true}, {"0.1.0", "unknown", true}, {"0.1.0", "dev", true}, {"0.0.9", "0.1.0", false}} {
		if newer(c.a, c.b) != c.want {
			t.Errorf("newer(%s, %s) != %v", c.a, c.b, c.want)
		}
	}
}
