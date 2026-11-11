package main

// Bug reports: the mod saves each Ctrl+F9 capture into <game>\p2vr-reports\<session start>\capture-NN (eye images, a preview,
// the HUD, context.txt, and the p2vr.ini / p2vr.log of that moment). The launcher lists them and packs the one the player
// picks, with system information and the installer log, into a zip in the Downloads folder, to attach to a GitHub issue.

import (
	"archive/zip"
	"bufio"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
	"time"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

const githubIssueLimit = 25 * 1000 * 1000 // GitHub's attachment limit

var safeName = regexp.MustCompile(`^[A-Za-z0-9_.-]+$`)

type captureInfo struct {
	Name    string   `json:"name"`
	Summary []string `json:"summary"`
	Thumb   string   `json:"thumb,omitempty"`
	Files   int      `json:"files"`
	Size    int64    `json:"size"`
}

type sessionInfo struct {
	Name     string        `json:"name"`
	Captures []captureInfo `json:"captures"`
}

func reportsDir(game string) string { return filepath.Join(game, "p2vr-reports") }

// listReports: the newest sessions first (folder names are their start time), at most 10
func listReports(game string) []sessionInfo {
	entries, err := os.ReadDir(reportsDir(game))
	if err != nil {
		return nil
	}
	var names []string
	for _, e := range entries {
		if e.IsDir() && safeName.MatchString(e.Name()) {
			names = append(names, e.Name())
		}
	}
	sort.Sort(sort.Reverse(sort.StringSlice(names)))
	var out []sessionInfo
	for _, n := range names {
		if len(out) == 10 {
			break
		}
		s := sessionInfo{Name: n}
		caps, _ := os.ReadDir(filepath.Join(reportsDir(game), n))
		for _, c := range caps {
			if !c.IsDir() || !safeName.MatchString(c.Name()) {
				continue
			}
			dir := filepath.Join(reportsDir(game), n, c.Name())
			ci := captureInfo{Name: c.Name()}
			files, _ := os.ReadDir(dir)
			for _, f := range files {
				if info, err := f.Info(); err == nil && !f.IsDir() {
					ci.Files++
					ci.Size += info.Size()
				}
			}
			for _, t := range []string{"view.bmp", "screen.bmp"} {
				if _, err := os.Stat(filepath.Join(dir, t)); err == nil {
					ci.Thumb = t
					break
				}
			}
			if ci.Thumb == "" {
				if m, _ := filepath.Glob(filepath.Join(dir, "hud_*.bmp")); len(m) > 0 {
					ci.Thumb = filepath.Base(m[0])
				}
			}
			if f, err := os.Open(filepath.Join(dir, "context.txt")); err == nil {
				sc := bufio.NewScanner(f)
				for sc.Scan() {
					l := sc.Text()
					for _, k := range []string{"time:", "phase:", "VR session:", "game speed:"} {
						if strings.HasPrefix(l, k) {
							ci.Summary = append(ci.Summary, l)
						}
					}
				}
				f.Close()
			}
			s.Captures = append(s.Captures, ci)
		}
		if len(s.Captures) > 0 {
			out = append(out, s)
		}
	}
	return out
}

// captureDir: the folder of one capture, only for names the list could have given
func captureDir(game, session, capture string) (string, error) {
	if !safeName.MatchString(session) || !safeName.MatchString(capture) {
		return "", fmt.Errorf("bad name")
	}
	dir := filepath.Join(reportsDir(game), session, capture)
	if st, err := os.Stat(dir); err != nil || !st.IsDir() {
		return "", fmt.Errorf("no such capture")
	}
	return dir, nil
}

func windowsVersion() string {
	k, err := registry.OpenKey(registry.LOCAL_MACHINE, `SOFTWARE\Microsoft\Windows NT\CurrentVersion`, registry.QUERY_VALUE)
	if err != nil {
		return runtime.GOOS
	}
	defer k.Close()
	name, _, _ := k.GetStringValue("ProductName")
	disp, _, _ := k.GetStringValue("DisplayVersion")
	build, _, _ := k.GetStringValue("CurrentBuild")
	return strings.TrimSpace(fmt.Sprintf("%s %s (build %s)", name, disp, build))
}

// makeReportZip packs a capture, system information and the installer log into Downloads\P2VR-bug-report_<session>_<capture>.zip
func makeReportZip(game gameInfo, session, capture string) (string, int64, error) {
	dir, err := captureDir(game.Path, session, capture)
	if err != nil {
		return "", 0, err
	}
	downloads := knownFolder(windows.FOLDERID_Downloads)
	if downloads == "" {
		downloads, _ = os.UserHomeDir()
	}
	out := filepath.Join(downloads, fmt.Sprintf("P2VR-bug-report_%s_%s.zip", session, capture))
	f, err := os.Create(out)
	if err != nil {
		return "", 0, err
	}
	zw := zip.NewWriter(f)
	add := func(name, path string) {
		src, err := os.Open(path)
		if err != nil {
			return
		}
		defer src.Close()
		w, err := zw.Create(name)
		if err == nil {
			io.Copy(w, src)
		}
	}
	files, _ := os.ReadDir(dir)
	for _, e := range files {
		if !e.IsDir() {
			add(capture+"/"+e.Name(), filepath.Join(dir, e.Name()))
		}
	}
	if logPath != "" {
		add("installer.log", logPath)
	}
	if w, err := zw.Create("system.txt"); err == nil {
		fmt.Fprintf(w, "Privateer 2 VR bug report\n")
		fmt.Fprintf(w, "created: %s\n", time.Now().Format("2006-01-02 15:04:05"))
		fmt.Fprintf(w, "launcher / installer: %s\n", version)
		fmt.Fprintf(w, "installed mod version: %s\n", game.Version)
		fmt.Fprintf(w, "game folder: %s (%s)\n", game.Path, game.Source)
		fmt.Fprintf(w, "windows: %s\n", windowsVersion())
		fmt.Fprintf(w, "capture: %s / %s\n", session, capture)
	}
	if err := zw.Close(); err != nil {
		f.Close()
		return "", 0, err
	}
	f.Close()
	st, _ := os.Stat(out)
	logf("bug report zip: %s (%d bytes)", out, st.Size())
	return out, st.Size(), nil
}

func revealFile(path string) {
	exec.Command("explorer", "/select,", path).Start()
}
