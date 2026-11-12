// Privateer 2 VR installer and launcher: a one-click installer for the Privateer 2: The Darkening VR mod, which also starts the
// game in VR (SteamVR started first when needed) or flat. Installed into the game folder as P2VR-Launcher.exe for the shortcut. It finds the game (Steam libraries,
// GOG, or a folder the user chooses), shows whether the mod is installed and which version, reads the latest release on GitHub,
// and installs, updates, repairs or uninstalls the mod. It only adds files next to the game's DARK.EXE (see game.go).
//
// The interface is a web page (ui/) served on 127.0.0.1 and shown in a native window (WebView2); without WebView2 it opens in
// the default browser and the app quits when the page is closed.
//
//	P2VR-Installer.exe [--browser] [--dir GAMEFOLDER] [--archive privateer2-vr-X.Y.Z.zip]
//
//	--browser        show the interface in the default browser instead of a window
//	--dir FOLDER     use this game folder (also how the installer passes the folder when it restarts as administrator)
//	--archive FILE   install this release archive instead of downloading the latest release (testing, offline installs)
package main

import (
	"crypto/rand"
	"embed"
	"encoding/hex"
	"flag"
	"fmt"
	"io/fs"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

const (
	appTitle     = "Privateer 2 VR"
	shortcutName = "Privateer 2 VR"
	repoURL      = "https://github.com/SPD13/privateer2-vr"
	releaseAPI   = "https://api.github.com/repos/SPD13/privateer2-vr/releases/latest"
)

var version = "dev" // set by build.ps1 (-X main.version=X.Y.Z)

//go:embed ui
var uiFiles embed.FS

func main() {
	browser := flag.Bool("browser", false, "show the interface in the default browser")
	dir := flag.String("dir", "", "game folder")
	archive := flag.String("archive", "", "install this release archive (.zip) instead of the latest release")
	flag.Parse()

	openLog()
	logf("%s %s on %s/%s (%s)", appTitle, version, runtime.GOOS, runtime.GOARCH, thisExe())
	// the launcher copy in the game folder: that folder, and the copy left by a self-update removed
	if exe := thisExe(); exe != "" {
		// the copy left by a self-update: the old launcher may still be closing after starting this one
		go func(old string) {
			for i := 0; i < 40; i++ {
				if err := os.Remove(old); err == nil || os.IsNotExist(err) {
					return
				}
				time.Sleep(250 * time.Millisecond)
			}
		}(exe + ".old")
		if *dir == "" && strings.EqualFold(filepath.Base(exe), launcherExe) {
			if d, ok := normalizeGameDir(filepath.Dir(exe)); ok {
				*dir = d
			}
		}
	}

	tokenBytes := make([]byte, 16)
	rand.Read(tokenBytes)
	app := newApp(hex.EncodeToString(tokenBytes), *dir, *archive)

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		fatal("cannot start: " + err.Error())
	}
	ui, _ := fs.Sub(uiFiles, "ui")
	mux := http.NewServeMux()
	mux.Handle("/", http.FileServer(http.FS(ui)))
	app.routes(mux)
	go http.Serve(listener, mux)
	url := fmt.Sprintf("http://%s/#%s", listener.Addr(), app.token)

	go app.start()

	if !*browser && openWindow(url) {
		return
	}
	app.update(func(s *appState) { s.Browser = true })
	openBrowser(url)
	app.waitForPageClosed()
}

// installer.log in %LOCALAPPDATA%\p2vr-installer (shown on the page): what was found and done, for help when something fails
var logFile *os.File
var logPath string

func openLog() {
	dir, err := os.UserCacheDir()
	if err != nil {
		return
	}
	dir = filepath.Join(dir, "p2vr-installer")
	os.MkdirAll(dir, 0o700)
	logPath = filepath.Join(dir, "installer.log")
	logFile, _ = os.Create(logPath)
}

func logf(format string, args ...any) {
	if logFile != nil {
		fmt.Fprintf(logFile, time.Now().Format("15:04:05 ")+format+"\n", args...)
	}
}

func fatal(message string) {
	fmt.Fprintln(os.Stderr, message)
	showError(message)
	os.Exit(1)
}
