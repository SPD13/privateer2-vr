package main

import (
	"os"
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
