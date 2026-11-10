package main

// Windows specifics: the window (Microsoft Edge WebView2, included with Windows 10 and 11, through github.com/jchv/go-webview2,
// no cgo), the folder picker, shortcuts, and running again as administrator when the game folder is not writable.

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"unsafe"

	webview2 "github.com/jchv/go-webview2"
	"golang.org/x/sys/windows"
)

var (
	shell32                 = windows.NewLazySystemDLL("shell32.dll")
	user32                  = windows.NewLazySystemDLL("user32.dll")
	ole32                   = windows.NewLazySystemDLL("ole32.dll")
	procSHBrowseForFolderW  = shell32.NewProc("SHBrowseForFolderW")
	procSHGetPathFromIDList = shell32.NewProc("SHGetPathFromIDListW")
	procFindWindowW         = user32.NewProc("FindWindowW")
	procSendMessageW        = user32.NewProc("SendMessageW")
	procGetSystemMetrics    = user32.NewProc("GetSystemMetrics")
	procCoInitializeEx      = ole32.NewProc("CoInitializeEx")
	procCoTaskMemFree       = ole32.NewProc("CoTaskMemFree")
)

func openWindow(url string) bool {
	dataPath := ""
	if dir, err := os.UserCacheDir(); err == nil {
		// WebView2 keeps its profile next to the .exe otherwise: in the Downloads folder
		dataPath = filepath.Join(dir, "p2vr-installer", "WebView2")
	}
	w := webview2.NewWithOptions(webview2.WebViewOptions{
		AutoFocus: true,
		DataPath:  dataPath,
		WindowOptions: webview2.WindowOptions{
			Title:  appTitle,
			Width:  780,
			Height: windowHeight(1240),
			IconId: 1, // the application icon (rsrc_windows_amd64.syso, see build.ps1)
			Center: true,
		},
	})
	if w == nil {
		return false
	}
	defer w.Destroy()
	w.Navigate(url)
	w.Run()
	return true
}

// windowHeight: the wanted height, but never taller than the screen's work area (above the taskbar)
func windowHeight(want uint) uint {
	h, _, _ := procGetSystemMetrics.Call(17) // SM_CYFULLSCREEN: work area height without the caption
	if h > 100 && uint(h)+30 < want {
		return uint(h) + 30
	}
	return want
}

func showError(message string) {
	windows.MessageBox(0, windows.StringToUTF16Ptr(message), windows.StringToUTF16Ptr(appTitle), windows.MB_OK|windows.MB_ICONERROR)
}

func ownerWindow() uintptr {
	h, _, _ := procFindWindowW.Call(0, uintptr(unsafe.Pointer(windows.StringToUTF16Ptr(appTitle))))
	return h
}

type browseInfo struct {
	owner       uintptr
	root        uintptr
	displayName *uint16
	title       *uint16
	flags       uint32
	callback    uintptr
	lParam      uintptr
	image       int32
}

var browseStart *uint16

// browse callback: start in the current game folder
var browseCallback = syscall.NewCallback(func(hwnd, msg, lParam, data uintptr) uintptr {
	const bffmInitialized, bffmSetSelectionW = 1, 0x400 + 103
	if msg == bffmInitialized && browseStart != nil {
		procSendMessageW.Call(hwnd, bffmSetSelectionW, 1, uintptr(unsafe.Pointer(browseStart)))
	}
	return 0
})

// pickFolder: Windows' folder dialog; "" when cancelled
func pickFolder(start string) string {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	procCoInitializeEx.Call(0, 2) // apartment threaded
	name := make([]uint16, windows.MAX_PATH)
	browseStart = nil
	if start != "" {
		browseStart = windows.StringToUTF16Ptr(start)
	}
	bi := browseInfo{
		owner:       ownerWindow(),
		displayName: &name[0],
		title:       windows.StringToUTF16Ptr("Choose the folder of Privateer 2: The Darkening"),
		flags:       0x1 | 0x40 | 0x200, // file system folders, new dialog style, no "new folder" button
		callback:    browseCallback,
	}
	pidl, _, _ := procSHBrowseForFolderW.Call(uintptr(unsafe.Pointer(&bi)))
	if pidl == 0 {
		return ""
	}
	defer procCoTaskMemFree.Call(pidl)
	path := make([]uint16, 4096)
	if ok, _, _ := procSHGetPathFromIDList.Call(pidl, uintptr(unsafe.Pointer(&path[0]))); ok == 0 {
		return ""
	}
	return windows.UTF16ToString(path)
}

func knownFolder(id *windows.KNOWNFOLDERID) string {
	p, err := windows.KnownFolderPath(id, 0)
	if err != nil {
		return ""
	}
	return p
}

func shortcutPaths() []string {
	var out []string
	if d := knownFolder(windows.FOLDERID_Desktop); d != "" {
		out = append(out, filepath.Join(d, shortcutName+".lnk"))
	}
	if d := knownFolder(windows.FOLDERID_Programs); d != "" {
		out = append(out, filepath.Join(d, shortcutName+".lnk"))
	}
	return out
}

func hidden(cmd *exec.Cmd) *exec.Cmd {
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000} // CREATE_NO_WINDOW
	return cmd
}

func psQuote(s string) string { return "'" + strings.ReplaceAll(s, "'", "''") + "'" }

// createShortcut: a .lnk (through Windows' own shortcut object, from PowerShell)
func createShortcut(lnk, target, args, workDir string) error {
	script := "$s = (New-Object -ComObject WScript.Shell).CreateShortcut(" + psQuote(lnk) + "); " +
		"$s.TargetPath = " + psQuote(target) + "; $s.Arguments = " + psQuote(args) + "; $s.WorkingDirectory = " + psQuote(workDir) + "; " +
		"$s.IconLocation = " + psQuote(target+",0") + "; $s.Description = 'Privateer 2: The Darkening in VR'; $s.Save()"
	out, err := hidden(exec.Command("powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script)).CombinedOutput()
	if err != nil {
		return &shortcutError{strings.TrimSpace(string(out))}
	}
	return nil
}

type shortcutError struct{ msg string }

func (e *shortcutError) Error() string { return "the shortcut could not be made: " + e.msg }

// relaunchAsAdmin: the same installer, elevated (UAC prompt), on the same game folder
func relaunchAsAdmin(gameDir string) error {
	exe, err := os.Executable()
	if err != nil {
		return err
	}
	args := "--dir \"" + gameDir + "\""
	return windows.ShellExecute(0, windows.StringToUTF16Ptr("runas"), windows.StringToUTF16Ptr(exe), windows.StringToUTF16Ptr(args), nil, windows.SW_NORMAL)
}

func isAdmin() bool { return windows.GetCurrentProcessToken().IsElevated() }

// startGame: DARKVR.EXE (VR) or the game's own DARK.EXE (flat), in the game folder
func startGame(dir string, vr bool) error {
	exe := gameExe
	if vr {
		exe = vrExe
	}
	cmd := exec.Command(filepath.Join(dir, exe))
	cmd.Dir = dir
	return cmd.Start()
}

// startSteamVR: through Steam (SteamVR's app id 250820)
func startSteamVR() { openBrowser("steam://rungameid/250820") }

func steamVRRunning() bool { return processRunning("vrserver.exe", "vrmonitor.exe") }

// thisExe: the running installer / launcher
func thisExe() string {
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	return exe
}

// replaceExe writes an .exe that may be the running launcher: a running .exe cannot be overwritten, but it can be renamed
// (to <name>.old, removed at the next start)
func replaceExe(path string, data []byte) error {
	if _, err := os.Stat(path); err == nil {
		os.Remove(path + ".old")
		if err := os.Rename(path, path+".old"); err != nil {
			return writeErr(err)
		}
	}
	if err := writeFile(path, data); err != nil {
		os.Rename(path+".old", path)
		return err
	}
	return nil
}

// removeSelfLater: delete the running launcher once it has quit (uninstall from the launcher itself)
func removeSelfLater(path string) {
	hidden(exec.Command("cmd", "/c", `ping -n 4 127.0.0.1 >nul & del /f /q "`+path+`"`)).Start()
}

func openFolder(dir string) {
	exec.Command("explorer", dir).Start()
}

func openBrowser(url string) {
	exec.Command("rundll32", "url.dll,FileProtocolHandler", url).Start()
}
