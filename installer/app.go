package main

// The installer's state and work. The page polls GET /api/state and starts the work with POST requests; every API request
// carries the random token of the page's address, so other web pages cannot drive the installer.

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

var archiveName = regexp.MustCompile(`^privateer2-vr-(\d+\.\d+\.\d+)\.zip$`)

type releaseInfo struct {
	Loading bool   `json:"loading"`
	Error   string `json:"error,omitempty"`
	Version string `json:"version,omitempty"`
	Date    string `json:"date,omitempty"`
	Size    int64  `json:"size,omitempty"`
	Notes   string `json:"notes,omitempty"`
	Page    string `json:"page,omitempty"`
	Local   bool   `json:"local,omitempty"` // --archive
	url     string
	digest  string
	path    string
}

type jobLine struct {
	Text string `json:"text"`
	Kind string `json:"kind"` // step, note, error, ok
}

type jobState struct {
	Kind       string    `json:"kind"` // install, uninstall
	Running    bool      `json:"running"`
	Failed     bool      `json:"failed"`
	Message    string    `json:"message,omitempty"`
	NeedsAdmin bool      `json:"needsAdmin,omitempty"`
	Progress   float64   `json:"progress"` // 0..1 while downloading, -1 otherwise
	Lines      []jobLine `json:"lines"`
}

type launchState struct {
	Busy   bool   `json:"busy"`
	Status string `json:"status,omitempty"`
	Error  string `json:"error,omitempty"`
}

type appState struct {
	Title             string      `json:"title"`
	Version           string      `json:"version"` // of this installer
	Searching         bool        `json:"searching"`
	Game              gameInfo    `json:"game"`
	Latest            releaseInfo `json:"latest"`
	UpdateAvailable   bool        `json:"updateAvailable"`
	InstallerOutdated bool        `json:"installerOutdated"`
	Job               *jobState   `json:"job"`
	Launch            launchState `json:"launch"`
	LogPath           string      `json:"logPath"`
	Browser           bool        `json:"browser"`
	Admin             bool        `json:"admin"`
	RepoURL           string      `json:"repoURL"`
}

type app struct {
	token   string
	mu      sync.Mutex
	state   appState
	busy    bool
	sources map[string]string

	lastPoll atomic.Int64
}

func newApp(token, dir, archive string) *app {
	a := &app{token: token}
	a.state = appState{Title: appTitle, Version: version, LogPath: logPath, Latest: releaseInfo{Loading: true}, Admin: isAdmin(), RepoURL: repoURL}
	if dir != "" {
		a.state.Game = gameInfo{Path: dir, Source: "chosen"}
	}
	if archive != "" {
		a.state.Latest = localRelease(archive)
	}
	return a
}

func (a *app) update(change func(*appState)) {
	a.mu.Lock()
	defer a.mu.Unlock()
	change(&a.state)
	a.refreshFlags()
}

func (a *app) refreshFlags() {
	s := &a.state
	s.UpdateAvailable = s.Game.Installed && s.Latest.Version != "" && newer(s.Latest.Version, s.Game.Version)
	s.InstallerOutdated = !s.Latest.Local && s.Latest.Version != "" && version != "dev" && newer(s.Latest.Version, version)
}

// newer: a > b for X.Y.Z versions (anything else counts as 0.0.0)
func newer(a, b string) bool {
	pa, pb := parseVersion(a), parseVersion(b)
	for i := 0; i < 3; i++ {
		if pa[i] != pb[i] {
			return pa[i] > pb[i]
		}
	}
	return false
}

func parseVersion(v string) [3]int {
	var out [3]int
	parts := strings.Split(strings.TrimPrefix(strings.TrimSpace(v), "v"), ".")
	if len(parts) != 3 {
		return out
	}
	for i, p := range parts {
		n, err := strconv.Atoi(p)
		if err != nil {
			return [3]int{}
		}
		out[i] = n
	}
	return out
}

// ---- settings of the installer (the chosen game folder)

type savedSettings struct {
	GameDir string `json:"gameDir"`
}

func settingsPath() string {
	dir, err := os.UserCacheDir()
	if err != nil {
		return ""
	}
	return filepath.Join(dir, "p2vr-installer", "settings.json")
}

func loadSettings() savedSettings {
	var s savedSettings
	if data, err := os.ReadFile(settingsPath()); err == nil {
		json.Unmarshal(data, &s)
	}
	return s
}

func saveSettings(s savedSettings) {
	if p := settingsPath(); p != "" {
		data, _ := json.MarshalIndent(s, "", "  ")
		os.WriteFile(p, data, 0o600)
	}
}

// ---- start: latest release, then the game

func (a *app) start() {
	if !a.state.Latest.Local {
		go a.fetchLatest()
	}
	a.findGame()
}

func (a *app) findGame() {
	a.update(func(s *appState) { s.Searching = true })
	paths, sources := findGames()
	logf("game folders found: %v", paths)
	a.mu.Lock()
	a.sources = sources
	chosen := a.state.Game.Path
	a.mu.Unlock()
	dir, source := "", ""
	switch {
	case chosen != "":
		dir, source = chosen, "chosen"
	case loadSettings().GameDir != "":
		if d, ok := normalizeGameDir(loadSettings().GameDir); ok {
			dir, source = d, sources[strings.ToLower(d)]
			if source == "" {
				source = "chosen"
			}
		}
	}
	if dir == "" && len(paths) > 0 {
		dir, source = paths[0], sources[strings.ToLower(paths[0])]
	}
	info := gameInfo{Problem: "Privateer 2: The Darkening was not found on this computer. Choose its folder."}
	if dir != "" {
		info = inspectGame(dir, source)
	}
	info.Candidates = paths
	logf("game: %+v", info)
	a.update(func(s *appState) { s.Searching = false; s.Game = info })
}

func (a *app) setGameDir(dir string) {
	a.mu.Lock()
	source := a.sources[strings.ToLower(dir)]
	candidates := a.state.Game.Candidates
	a.mu.Unlock()
	if d, ok := normalizeGameDir(dir); ok {
		if s := a.sources[strings.ToLower(d)]; s != "" {
			source = s
		}
	}
	if source == "" {
		source = "chosen"
	}
	info := inspectGame(dir, source)
	info.Candidates = candidates
	if info.Valid {
		saveSettings(savedSettings{GameDir: info.Path})
	}
	logf("game folder set: %+v", info)
	a.update(func(s *appState) { s.Game = info })
}

func (a *app) refreshGame() {
	a.mu.Lock()
	g := a.state.Game
	a.mu.Unlock()
	if g.Path == "" {
		return
	}
	info := inspectGame(g.Path, g.Source)
	info.Candidates = g.Candidates
	a.update(func(s *appState) { s.Game = info })
}

// fetchLatest reads the latest release on GitHub: its version and the mod's .zip
func (a *app) fetchLatest() {
	a.update(func(s *appState) { s.Latest = releaseInfo{Loading: true} })
	info := func() releaseInfo {
		request, _ := http.NewRequest("GET", releaseAPI, nil)
		request.Header.Set("Accept", "application/vnd.github+json")
		request.Header.Set("User-Agent", "p2vr-installer")
		client := &http.Client{Timeout: 20 * time.Second}
		response, err := client.Do(request)
		if err != nil {
			return releaseInfo{Error: "Cannot reach GitHub to find the latest version. Check the internet connection."}
		}
		defer response.Body.Close()
		if response.StatusCode == 404 {
			return releaseInfo{Error: "No release is published on GitHub yet."}
		}
		if response.StatusCode != 200 {
			return releaseInfo{Error: fmt.Sprintf("GitHub did not give the latest version (%s). Try again in a few minutes.", response.Status)}
		}
		var release struct {
			TagName     string    `json:"tag_name"`
			HTMLURL     string    `json:"html_url"`
			Body        string    `json:"body"`
			PublishedAt time.Time `json:"published_at"`
			Assets      []struct {
				Name   string `json:"name"`
				Size   int64  `json:"size"`
				URL    string `json:"browser_download_url"`
				Digest string `json:"digest"`
			} `json:"assets"`
		}
		if err := json.NewDecoder(response.Body).Decode(&release); err != nil {
			return releaseInfo{Error: "GitHub's answer could not be read: " + err.Error()}
		}
		for _, asset := range release.Assets {
			if m := archiveName.FindStringSubmatch(asset.Name); m != nil {
				return releaseInfo{Version: m[1], Date: release.PublishedAt.Local().Format("2 January 2006"), Size: asset.Size,
					Notes: release.Body, Page: release.HTMLURL, url: asset.URL, digest: asset.Digest}
			}
		}
		return releaseInfo{Error: "The latest release (" + release.TagName + ") has no mod archive."}
	}()
	logf("latest release: %s %s", info.Version, info.Error)
	a.update(func(s *appState) { s.Latest = info })
}

func localRelease(path string) releaseInfo {
	stat, err := os.Stat(path)
	if err != nil {
		return releaseInfo{Error: "Cannot open " + path + ": " + err.Error()}
	}
	v := filepath.Base(path)
	if m := archiveName.FindStringSubmatch(v); m != nil {
		v = m[1]
	}
	return releaseInfo{Version: v, Date: stat.ModTime().Format("2 January 2006"), Size: stat.Size(), Local: true, path: path}
}

// ---- jobs

func (a *app) beginJob(kind string) (*jobState, gameInfo, releaseInfo, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.busy {
		return nil, gameInfo{}, releaseInfo{}, errors.New("busy")
	}
	a.busy = true
	job := &jobState{Kind: kind, Running: true, Progress: -1}
	a.state.Job = job
	return job, a.state.Game, a.state.Latest, nil
}

func (a *app) endJob(job *jobState, err error) {
	a.update(func(s *appState) {
		job.Running = false
		job.Progress = -1
		if err != nil {
			job.Failed = true
			job.Message = err.Error()
			job.NeedsAdmin = isPermission(err) && !s.Admin
			job.Lines = append(job.Lines, jobLine{Text: err.Error(), Kind: "error"})
		}
	})
	logf("%s finished: %v", job.Kind, err)
	a.mu.Lock()
	a.busy = false
	a.mu.Unlock()
	a.refreshGame()
}

func (a *app) line(job *jobState, kind, format string, args ...any) {
	text := fmt.Sprintf(format, args...)
	logf("%s %s: %s", job.Kind, kind, text)
	a.update(func(s *appState) { job.Lines = append(job.Lines, jobLine{Text: text, Kind: kind}) })
}

func isPermission(err error) bool {
	var errno syscall.Errno
	return errors.Is(err, os.ErrPermission) || (errors.As(err, &errno) && (errno == 5 || errno == 32))
}

type permissionError struct{ err error }

func (e *permissionError) Error() string {
	return "Windows did not allow writing to the game folder (" + e.err.Error() + "). Close the game if it is running, or restart the installer as administrator."
}
func (e *permissionError) Unwrap() error { return os.ErrPermission }

func writeErr(err error) error {
	if isPermission(err) {
		return &permissionError{err}
	}
	return err
}

// writeFile: through a temporary file and a rename, so a failure never leaves half a file
func writeFile(path string, data []byte) error {
	tmp := path + ".p2vr-new"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		os.Remove(tmp)
		return writeErr(err)
	}
	if err := os.Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return writeErr(err)
	}
	return nil
}

// the running launcher, starting the new one and leaving after a self-update (replaced in tests)
var (
	selfExe        = thisExe
	launchLauncher = startLauncher
	exitProcess    = os.Exit
)

func (a *app) install(shortcut bool) {
	job, game, latest, err := a.beginJob("install")
	if err != nil {
		return
	}
	restart := "" // the running launcher was replaced by a different version: hand over to it at the end
	err = func() error {
		if !game.Valid {
			return errors.New("Choose the game folder first.")
		}
		if latest.Version == "" {
			if latest.Error != "" {
				return errors.New(latest.Error)
			}
			return errors.New("The latest version is not known yet.")
		}
		if processRunning(gameExe, vrExe) {
			return errors.New("Privateer 2 is running: close the game, then install again.")
		}
		dir := game.Path

		// 1. the release archive
		archive := latest.path
		if archive == "" {
			a.line(job, "step", "Downloading version %s", latest.Version)
			tmp, err := a.download(job, latest)
			if err != nil {
				return err
			}
			defer os.Remove(tmp)
			archive = tmp
		}
		zr, err := zip.OpenReader(archive)
		if err != nil {
			return errors.New("The downloaded archive is damaged: " + err.Error())
		}
		defer zr.Close()
		files := map[string][]byte{}
		for _, f := range zr.File {
			if f.FileInfo().IsDir() {
				continue
			}
			rc, err := f.Open()
			if err != nil {
				return err
			}
			data, err := io.ReadAll(rc)
			rc.Close()
			if err != nil {
				return errors.New("The downloaded archive is damaged: " + err.Error())
			}
			files[strings.ToLower(filepath.Base(f.Name))] = data
		}
		for _, need := range []string{"p2vr.dll", "openxr_loader.dll", "p2vr.ini"} {
			if files[need] == nil {
				return errors.New("The release archive has no " + need + ".")
			}
		}

		// 2. the files of the mod
		a.line(job, "step", "Installing into %s", dir)
		exe, err := os.ReadFile(filepath.Join(dir, gameExe))
		if err != nil {
			return err
		}
		vr, err := patchExe(exe)
		if err != nil {
			return err
		}
		if err := writeFile(filepath.Join(dir, vrExe), vr); err != nil {
			return err
		}
		a.line(job, "note", "DARKVR.EXE made from your DARK.EXE (the game's own files are not changed)")
		for _, f := range []struct{ from, to string }{{"p2vr.dll", modDll}, {"openxr_loader.dll", "openxr_loader.dll"}, {"readme.md", "P2VR-README.md"}, {"settings.md", "P2VR-SETTINGS.md"}} {
			if files[f.from] == nil {
				continue
			}
			if err := writeFile(filepath.Join(dir, f.to), files[f.from]); err != nil {
				return err
			}
		}
		a.line(job, "note", "P2VR.dll and openxr_loader.dll copied")
		if launcher := files[strings.ToLower(launcherExe)]; launcher != nil {
			path := filepath.Join(dir, launcherExe)
			if self := selfExe(); self != "" && strings.EqualFold(filepath.Clean(self), filepath.Clean(path)) {
				if cur, err := os.ReadFile(path); err == nil && sha256Of(cur) != sha256Of(launcher) {
					restart = path
				}
			}
			if err := replaceExe(path, launcher); err != nil {
				return err
			}
			a.line(job, "note", "Launcher P2VR-Launcher.exe added (play in VR or flat, updates)")
		}

		// 3. settings: keep the user's, add the new ones
		fresh := files["p2vr.ini"]
		if err := writeFile(filepath.Join(dir, defaultsFile), fresh); err != nil {
			return err
		}
		iniPath := filepath.Join(dir, settingsFile)
		if old, err := os.ReadFile(iniPath); err == nil {
			merged, added := mergeSettings(string(old), string(fresh), latest.Version)
			if added > 0 {
				if err := writeFile(iniPath, []byte(merged)); err != nil {
					return err
				}
				a.line(job, "note", "Your settings (p2vr.ini) are kept; %d new setting(s) added with their defaults", added)
			} else {
				a.line(job, "note", "Your settings (p2vr.ini) are kept")
			}
		} else {
			if err := writeFile(iniPath, fresh); err != nil {
				return err
			}
			a.line(job, "note", "Settings written to p2vr.ini (see P2VR-SETTINGS.md)")
		}
		if err := writeFile(filepath.Join(dir, versionFile), []byte(latest.Version+"\r\n")); err != nil {
			return err
		}

		// 4. bHaptics TactSuit support files, only for players with bHaptics Player (never fails the install)
		if msg, err := ensureBhaptics(dir); err != nil {
			a.line(job, "note", "%v", err)
		} else if msg != "" {
			a.line(job, "note", "%s", msg)
		}

		// 5. shortcuts
		if shortcut {
			target, args := filepath.Join(dir, vrExe), ""
			if _, err := os.Stat(filepath.Join(dir, launcherExe)); err == nil {
				target, args = filepath.Join(dir, launcherExe), `--dir "`+dir+`"`
			}
			for _, lnk := range shortcutPaths() {
				if err := createShortcut(lnk, target, args, dir); err != nil {
					a.line(job, "note", "%v", err)
				}
			}
			a.line(job, "note", "Shortcuts \"%s\" added to the desktop and the Start menu", shortcutName)
		}
		a.line(job, "ok", "Privateer 2 VR %s is installed", latest.Version)
		if restart != "" {
			a.line(job, "note", "Restarting the launcher (version %s)", latest.Version)
		}
		return nil
	}()
	a.endJob(job, err)
	// This launcher replaced itself: the steps that only the new version has (e.g. the bHaptics support files on "Play
	// in VR") would not run in this process, so the new launcher takes over, in the same game folder.
	if err == nil && restart != "" {
		time.Sleep(1500 * time.Millisecond) // the page shows the result first
		if rerr := launchLauncher(restart, game.Path); rerr != nil {
			logf("restart: %v (the new launcher runs at the next start)", rerr)
			return
		}
		logf("restart: new launcher started, closing this one")
		exitProcess(0)
	}
}

func (a *app) download(job *jobState, latest releaseInfo) (string, error) {
	client := &http.Client{Timeout: 30 * time.Minute}
	request, _ := http.NewRequest("GET", latest.url, nil)
	request.Header.Set("User-Agent", "p2vr-installer")
	response, err := client.Do(request)
	if err != nil {
		return "", errors.New("The download failed: " + err.Error())
	}
	defer response.Body.Close()
	if response.StatusCode != 200 {
		return "", fmt.Errorf("The download failed (%s).", response.Status)
	}
	out, err := os.CreateTemp("", "privateer2-vr-*.zip")
	if err != nil {
		return "", err
	}
	defer out.Close()
	total := response.ContentLength
	if total <= 0 {
		total = latest.Size
	}
	hash := sha256.New()
	buf := make([]byte, 256*1024)
	var done int64
	for {
		n, rerr := response.Body.Read(buf)
		if n > 0 {
			if _, err := out.Write(buf[:n]); err != nil {
				os.Remove(out.Name())
				return "", err
			}
			hash.Write(buf[:n])
			done += int64(n)
			if total > 0 {
				p := float64(done) / float64(total)
				a.update(func(s *appState) { job.Progress = p })
			}
		}
		if rerr == io.EOF {
			break
		}
		if rerr != nil {
			os.Remove(out.Name())
			return "", errors.New("The download failed: " + rerr.Error())
		}
	}
	a.update(func(s *appState) { job.Progress = -1 })
	if want, ok := strings.CutPrefix(latest.digest, "sha256:"); ok && !strings.EqualFold(want, hex.EncodeToString(hash.Sum(nil))) {
		os.Remove(out.Name())
		return "", errors.New("The download is damaged (its checksum does not match). Try again.")
	}
	a.line(job, "note", "Downloaded %.1f MB", float64(done)/1e6)
	return out.Name(), nil
}

func (a *app) uninstall(withSettings bool) {
	job, game, _, err := a.beginJob("uninstall")
	if err != nil {
		return
	}
	err = func() error {
		if processRunning(gameExe, vrExe) {
			return errors.New("Privateer 2 is running: close the game, then uninstall again.")
		}
		dir := game.Path
		a.line(job, "step", "Removing the VR mod from %s", dir)
		names := append([]string{}, modFiles...)
		if withSettings {
			names = append(names, settingsFile)
		}
		for _, n := range names {
			p := filepath.Join(dir, n)
			if strings.EqualFold(p, thisExe()) { // uninstalling from the launcher: removed once it quits
				removeSelfLater(p)
				continue
			}
			if err := os.Remove(p); err != nil && !os.IsNotExist(err) {
				return writeErr(err)
			}
		}
		for _, lnk := range shortcutPaths() {
			os.Remove(lnk)
		}
		if withSettings {
			a.line(job, "note", "Mod files, settings and shortcuts removed")
		} else {
			a.line(job, "note", "Mod files and shortcuts removed; your settings (p2vr.ini) are kept for a later install")
		}
		a.line(job, "ok", "The VR mod is uninstalled. The game itself is unchanged.")
		return nil
	}()
	a.endJob(job, err)
}

// ---- launching the game

func (a *app) play(vr, closeAfter bool) {
	a.mu.Lock()
	g, busy := a.state.Game, a.state.Launch.Busy || a.busy
	if !busy {
		a.state.Launch = launchState{Busy: true}
	}
	a.mu.Unlock()
	if busy {
		return
	}
	set := func(status string) {
		logf("launch: %s", status)
		a.update(func(s *appState) { s.Launch.Status = status })
	}
	err := func() error {
		if !g.Valid {
			return errors.New("Choose the game folder first.")
		}
		if processRunning(gameExe, vrExe) {
			return errors.New("Privateer 2 is already running.")
		}
		if vr {
			if !g.Installed || g.NeedsRepair != "" {
				return errors.New("Install the VR mod first.")
			}
			if msg, err := ensureBhaptics(g.Path); err != nil { // bHaptics Player installed since the mod was: fetch the support files
				logf("launch: %v", err)
			} else if msg != "" {
				logf("launch: %s", msg)
			}
			if !steamVRRunning() {
				set("Starting SteamVR…")
				startSteamVR()
				deadline := time.Now().Add(60 * time.Second)
				for !steamVRRunning() && time.Now().Before(deadline) {
					time.Sleep(500 * time.Millisecond)
				}
				if !steamVRRunning() {
					return errors.New("SteamVR did not start. Start it from Steam (with the headset connected), then press Play in VR again.")
				}
				set("Waiting for SteamVR to be ready…")
				time.Sleep(6 * time.Second)
			}
		}
		set("Starting the game…")
		if err := startGame(g.Path, vr); err != nil {
			return errors.New("The game could not be started: " + err.Error())
		}
		return nil
	}()
	if err != nil {
		a.update(func(s *appState) { s.Launch = launchState{Error: err.Error()} })
		logf("launch failed: %v", err)
		return
	}
	mode := "flat"
	if vr {
		mode = "in VR"
	}
	a.update(func(s *appState) { s.Launch = launchState{Busy: true, Status: "Privateer 2 started " + mode + "."} })
	if closeAfter {
		time.Sleep(2 * time.Second)
		os.Exit(0)
	}
	time.Sleep(4 * time.Second)
	a.update(func(s *appState) { s.Launch = launchState{} })
	a.refreshGame()
}

// ---- the page's API

func (a *app) routes(mux *http.ServeMux) {
	handle := func(path string, method string, f func(w http.ResponseWriter, r *http.Request)) {
		mux.HandleFunc(path, func(w http.ResponseWriter, r *http.Request) {
			if r.Method != method || r.Header.Get("X-Token") != a.token {
				http.Error(w, "forbidden", http.StatusForbidden)
				return
			}
			f(w, r)
		})
	}
	type requestBody struct {
		Dir      string `json:"dir"`
		Shortcut bool   `json:"shortcut"`
		Settings bool   `json:"settings"`
		URL      string `json:"url"`
		Mode     string `json:"mode"`
		Close    bool   `json:"close"`
	}
	decode := func(r *http.Request) (requestBody, error) {
		var b requestBody
		err := json.NewDecoder(io.LimitReader(r.Body, 64*1024)).Decode(&b)
		return b, err
	}
	handle("/api/state", "GET", func(w http.ResponseWriter, r *http.Request) {
		a.lastPoll.Store(time.Now().UnixNano())
		a.mu.Lock()
		data, _ := json.Marshal(a.state)
		a.mu.Unlock()
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.Write(data)
	})
	handle("/api/browse", "POST", func(w http.ResponseWriter, r *http.Request) {
		a.mu.Lock()
		start := a.state.Game.Path
		a.mu.Unlock()
		if dir := pickFolder(start); dir != "" {
			a.setGameDir(dir)
		}
		w.WriteHeader(http.StatusNoContent)
	})
	handle("/api/select", "POST", func(w http.ResponseWriter, r *http.Request) {
		if body, err := decode(r); err == nil && body.Dir != "" {
			a.setGameDir(body.Dir)
		}
		w.WriteHeader(http.StatusNoContent)
	})
	handle("/api/refresh", "POST", func(w http.ResponseWriter, r *http.Request) {
		go func() {
			a.refreshGame()
			a.mu.Lock()
			local := a.state.Latest.Local
			a.mu.Unlock()
			if !local {
				a.fetchLatest()
			}
		}()
		w.WriteHeader(http.StatusNoContent)
	})
	handle("/api/rescan", "POST", func(w http.ResponseWriter, r *http.Request) {
		go a.refreshGame()
		w.WriteHeader(http.StatusNoContent)
	})
	handle("/api/install", "POST", func(w http.ResponseWriter, r *http.Request) {
		body, _ := decode(r)
		go a.install(body.Shortcut)
		w.WriteHeader(http.StatusNoContent)
	})
	handle("/api/uninstall", "POST", func(w http.ResponseWriter, r *http.Request) {
		body, _ := decode(r)
		go a.uninstall(body.Settings)
		w.WriteHeader(http.StatusNoContent)
	})
	handle("/api/play", "POST", func(w http.ResponseWriter, r *http.Request) {
		body, _ := decode(r)
		go a.play(body.Mode == "vr", body.Close)
		w.WriteHeader(http.StatusNoContent)
	})
	handle("/api/folder", "POST", func(w http.ResponseWriter, r *http.Request) {
		a.mu.Lock()
		dir := a.state.Game.Path
		a.mu.Unlock()
		openFolder(dir)
		w.WriteHeader(http.StatusNoContent)
	})
	handle("/api/admin", "POST", func(w http.ResponseWriter, r *http.Request) {
		a.mu.Lock()
		dir := a.state.Game.Path
		a.mu.Unlock()
		if err := relaunchAsAdmin(dir); err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		w.WriteHeader(http.StatusNoContent)
		go func() { time.Sleep(300 * time.Millisecond); os.Exit(0) }()
	})
	// only the page's own links
	handle("/api/open", "POST", func(w http.ResponseWriter, r *http.Request) {
		body, err := decode(r)
		if err != nil || !(body.URL == repoURL || body.URL == repoURL+"#readme" || strings.HasPrefix(body.URL, repoURL+"/")) {
			http.Error(w, "bad request", http.StatusBadRequest)
			return
		}
		openBrowser(body.URL)
		w.WriteHeader(http.StatusNoContent)
	})
	// bug reports: the captures of recent sessions, their preview images, and the zip
	handle("/api/reports", "GET", func(w http.ResponseWriter, r *http.Request) {
		a.mu.Lock()
		dir := a.state.Game.Path
		a.mu.Unlock()
		data, _ := json.Marshal(listReports(dir))
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.Write(data)
	})
	mux.HandleFunc("/api/report-file", func(w http.ResponseWriter, r *http.Request) { // <img> cannot send a header: token in the URL
		q := r.URL.Query()
		if r.Method != "GET" || q.Get("t") != a.token || !safeName.MatchString(q.Get("f")) {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		a.mu.Lock()
		game := a.state.Game.Path
		a.mu.Unlock()
		dir, err := captureDir(game, q.Get("s"), q.Get("c"))
		if err != nil {
			http.Error(w, "not found", http.StatusNotFound)
			return
		}
		w.Header().Set("Cache-Control", "no-store")
		http.ServeFile(w, r, filepath.Join(dir, q.Get("f")))
	})
	handle("/api/report-zip", "POST", func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			Session string `json:"session"`
			Capture string `json:"capture"`
		}
		if json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&b) != nil {
			http.Error(w, "bad request", http.StatusBadRequest)
			return
		}
		a.mu.Lock()
		game := a.state.Game
		a.mu.Unlock()
		path, size, err := makeReportZip(game, b.Session, b.Capture)
		if err != nil {
			http.Error(w, "The zip could not be made: "+err.Error(), http.StatusInternalServerError)
			return
		}
		data, _ := json.Marshal(map[string]any{"path": path, "size": size, "tooBig": size > githubIssueLimit})
		w.Header().Set("Content-Type", "application/json")
		w.Write(data)
	})
	handle("/api/report-reveal", "POST", func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			Path string `json:"path"`
		}
		if json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&b) == nil && strings.HasPrefix(filepath.Base(b.Path), "P2VR-bug-report_") && strings.HasSuffix(b.Path, ".zip") {
			revealFile(b.Path)
		}
		w.WriteHeader(http.StatusNoContent)
	})
	handle("/api/log", "POST", func(w http.ResponseWriter, r *http.Request) {
		openFolder(filepath.Dir(logPath))
		w.WriteHeader(http.StatusNoContent)
	})
}

// waitForPageClosed (browser only): returns when the page stopped asking for the state while no job runs
func (a *app) waitForPageClosed() {
	opened := time.Now()
	for {
		time.Sleep(2 * time.Second)
		a.mu.Lock()
		busy := a.busy
		a.mu.Unlock()
		last := a.lastPoll.Load()
		if busy {
			continue
		}
		if last == 0 && time.Since(opened) > 30*time.Minute {
			return
		}
		if last != 0 && time.Since(time.Unix(0, last)) > 20*time.Second {
			return
		}
	}
}
