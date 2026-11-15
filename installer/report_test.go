package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestReports(t *testing.T) {
	dir := os.Getenv("P2VR_TEST_GAME")
	if dir == "" {
		t.Skip("P2VR_TEST_GAME not set")
	}
	s := listReports(dir)
	t.Logf("%+v", s)
	if len(s) == 0 {
		t.Fatal("no sessions")
	}
	p, n, err := makeReportZip(gameInfo{Path: dir, Version: "test", Source: "chosen"}, s[0].Name, s[0].Captures[0].Name)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("zip %s %d", p, n)
	os.Remove(p)
}

// Deleting one capture keeps the others; the session goes with its last capture; clearing removes everything; names
// outside the reports folder are refused.
func TestDeleteReports(t *testing.T) {
	game := t.TempDir()
	mk := func(session, capture string) {
		d := filepath.Join(reportsDir(game), session, capture)
		os.MkdirAll(d, 0o755)
		os.WriteFile(filepath.Join(d, "context.txt"), []byte("time: x\n"), 0o644)
	}
	mk("2026-11-11_20-02-04", "capture-01")
	mk("2026-11-15_08-18-23", "capture-01")
	mk("2026-11-15_08-18-23", "capture-02")

	if err := deleteCapture(game, "2026-11-15_08-18-23", "capture-01"); err != nil {
		t.Fatal(err)
	}
	if s := listReports(game); len(s) != 2 || len(s[0].Captures) != 1 || s[0].Captures[0].Name != "capture-02" {
		t.Fatalf("after deleting one capture: %+v", s)
	}
	if err := deleteCapture(game, "2026-11-11_20-02-04", "capture-01"); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(reportsDir(game), "2026-11-11_20-02-04")); !os.IsNotExist(err) {
		t.Fatalf("the empty session folder was kept: %v", err)
	}
	for _, bad := range [][2]string{{"..", "x"}, {"2026-11-15_08-18-23", ".."}, {"a/b", "c"}, {"2026-11-15_08-18-23", "capture-09"}} {
		if deleteCapture(game, bad[0], bad[1]) == nil {
			t.Fatalf("deleteCapture(%q, %q) was accepted", bad[0], bad[1])
		}
	}
	if s := listReports(game); len(s) != 1 {
		t.Fatalf("a refused delete changed the list: %+v", s)
	}
	if err := clearReports(game); err != nil {
		t.Fatal(err)
	}
	if s := listReports(game); len(s) != 0 {
		t.Fatalf("after clearing: %+v", s)
	}
	if clearReports("") == nil {
		t.Fatal("clearReports with no game folder was accepted")
	}
}
