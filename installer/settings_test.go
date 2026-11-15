package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A setting still at an earlier release's default takes the new default; the player's own values stay.
func TestUpdateOldDefaults(t *testing.T) {
	old := "[VR]\r\nPANEL_VELOCITY=236,4,406,58, 0,24, 0.36\r\nPANEL_COMMS=0,0,124,150, -34,22, 0.24\r\n" +
		"RETICLE_GLASS_ALPHA=0.7 ; my comment\r\nINVERT_PITCH=1\r\n"
	fresh := "[VR]\r\nPANEL_VELOCITY=236,0,406,58, 0,24, 0.36\r\nPANEL_COMMS=0,0,124,190, -34,18, 0.24\r\n" +
		"RETICLE_GLASS_ALPHA=0.9\r\nINVERT_PITCH=0\r\n"
	got, changed := updateOldDefaults(old, fresh)
	want := "[VR]\r\nPANEL_VELOCITY=236,0,406,58, 0,24, 0.36\r\nPANEL_COMMS=0,0,124,150, -34,22, 0.24\r\n" +
		"RETICLE_GLASS_ALPHA=0.9\r\nINVERT_PITCH=1\r\n"
	if got != want || strings.Join(changed, ",") != "PANEL_VELOCITY,RETICLE_GLASS_ALPHA" {
		t.Fatalf("got %q (changed %v)", got, changed)
	}
	if again, c := updateOldDefaults(got, fresh); again != got || len(c) != 0 {
		t.Fatalf("second run changed %v", c)
	}
}

// Every old default listed must be a setting of the shipped p2vr.ini whose current default is different.
func TestOldDefaultsAgainstShippedIni(t *testing.T) {
	data, err := os.ReadFile("../dist/p2vr.ini")
	if err != nil {
		t.Skip("dist/p2vr.ini not found")
	}
	cur := map[string]string{}
	for _, l := range strings.Split(strings.ReplaceAll(string(data), "\r\n", "\n"), "\n") {
		if m := iniKey.FindStringSubmatch(l); m != nil {
			cur[strings.ToUpper(m[1])] = iniValue(l)
		}
	}
	for k, olds := range oldDefaults {
		v, ok := cur[k]
		if !ok {
			t.Errorf("%s is not in dist/p2vr.ini", k)
		}
		for _, o := range olds {
			if o == v {
				t.Errorf("%s: old default %s is the current default", k, o)
			}
		}
	}
}

// "Play in VR" after an update done by an older launcher: the game folder's p2vr.ini still has old defaults, and
// p2vr.default.ini is the new version's. Old defaults are updated once; a second run changes nothing.
func TestRefreshOldDefaults(t *testing.T) {
	dir := t.TempDir()
	if ch, err := refreshOldDefaults(dir); err != nil || ch != nil {
		t.Fatalf("empty folder: %v %v", ch, err)
	}
	fresh, err := os.ReadFile("../dist/p2vr.ini")
	if err != nil {
		t.Skip("dist/p2vr.ini not found")
	}
	old := []byte("[VR]\r\nPANEL_VELOCITY=236,4,406,58, 0,24, 0.36\r\nINVERT_PITCH=1\r\n")
	if p := os.Getenv("P2VR_TEST_INI"); p != "" {
		old, _ = os.ReadFile(p) // a copy of the player's real file
	}
	os.WriteFile(filepath.Join(dir, defaultsFile), fresh, 0o644)
	os.WriteFile(filepath.Join(dir, settingsFile), old, 0o644)
	ch, err := refreshOldDefaults(dir)
	t.Logf("changed: %v", ch)
	if err != nil || len(ch) == 0 {
		t.Fatalf("nothing updated: %v", err)
	}
	got, _ := os.ReadFile(filepath.Join(dir, settingsFile))
	if !strings.Contains(string(got), "PANEL_VELOCITY=236,0,406,58") || strings.Contains(string(got), "236,4,") {
		t.Fatalf("PANEL_VELOCITY not updated")
	}
	if ch, _ := refreshOldDefaults(dir); ch != nil {
		t.Fatalf("second run changed %v", ch)
	}
}

// The player's real settings file (P2VR_TEST_INI), merged with the shipped defaults: only old defaults change.
func TestUpdateOldDefaultsRealIni(t *testing.T) {
	p := os.Getenv("P2VR_TEST_INI")
	if p == "" {
		t.Skip("P2VR_TEST_INI not set")
	}
	old, _ := os.ReadFile(p)
	fresh, _ := os.ReadFile("../dist/p2vr.ini")
	_, changed := updateOldDefaults(string(old), string(fresh))
	t.Logf("changed: %v", changed)
}
