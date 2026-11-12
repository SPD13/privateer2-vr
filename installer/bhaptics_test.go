package main

import (
	"os"
	"path/filepath"
	"testing"
)

// Without bHaptics Player nothing is downloaded or written.
func TestEnsureBhapticsWithoutPlayer(t *testing.T) {
	t.Setenv("P2VR_NO_BHAPTICS", "1")
	dir := t.TempDir()
	msg, err := ensureBhaptics(dir)
	if err != nil || msg != "" {
		t.Fatalf("ensureBhaptics without bHaptics Player: %q, %v", msg, err)
	}
	entries, _ := os.ReadDir(dir)
	if len(entries) != 0 {
		t.Fatalf("files written without bHaptics Player: %v", entries)
	}
}

// Files already present with the expected checksum are kept (no download).
func TestEnsureBhapticsUpToDate(t *testing.T) {
	if !bhapticsPlayerInstalled() {
		t.Skip("bHaptics Player not installed on this machine")
	}
	dir := t.TempDir()
	for _, f := range bhapticsFiles {
		src := filepath.Join("..", "game", f.to)
		data, err := os.ReadFile(src)
		if err != nil || sha256Of(data) != f.sha256 {
			t.Skip("pinned files not in ../game (run scripts/get_bhaptics.ps1)")
		}
		os.WriteFile(filepath.Join(dir, f.to), data, 0o644)
	}
	msg, err := ensureBhaptics(dir)
	if err != nil || msg != "" {
		t.Fatalf("up-to-date files: %q, %v", msg, err)
	}
}
