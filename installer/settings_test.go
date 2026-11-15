package main

import (
	"os"
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
