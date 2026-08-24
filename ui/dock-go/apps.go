package main

import (
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

type IconKind int

const (
	IconFolder IconKind = iota
	IconTerminal
	IconGlobe
	IconCode
	IconMail
	IconMusic
	IconGear
	IconTrash
)

type App struct {
	Name  string
	Cmd   string
	Icon  IconKind
	Track bool
}

func defaultApps() []App {
	return []App{
		{"Files", "nautilus", IconFolder, true},
		{"Terminal", "alacritty", IconTerminal, true},
		{"Browser", "firefox", IconGlobe, true},
		{"Code", "code", IconCode, true},
		{"Почта", "thunderbird", IconMail, true},
		{"Музыка", "mpv", IconMusic, true},
		{"Настройки", "gnome-control-center", IconGear, true},
		{"Корзина", "nautilus trash:///", IconTrash, false},
	}
}

func launch(app App) {
	if dbusLaunch(app.Name) {
		return
	}
	fields := strings.Fields(app.Cmd)
	if len(fields) == 0 {
		return
	}
	cmd := exec.Command(fields[0], fields[1:]...)
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	_ = cmd.Start()
	go func() { _ = cmd.Wait() }()
}

var (
	runningMu sync.RWMutex
	running   = map[string]bool{}
)

func markRunning(name string, ok bool) {
	runningMu.Lock()
	running[name] = ok
	runningMu.Unlock()
}

func isRunning(name string) bool {
	runningMu.RLock()
	defer runningMu.RUnlock()
	return running[name]
}

func watchRunning(apps []App) {
	update := func() {
		procs := scanProcs()
		for _, a := range apps {
			if !a.Track {
				continue
			}
			markRunning(a.Name, procs[commName(a.Cmd)])
		}
	}
	update()
	for range time.Tick(2 * time.Second) {
		update()
	}
}

func commName(cmdline string) string {
	f := strings.Fields(cmdline)
	if len(f) == 0 {
		return ""
	}
	p := f[0]
	if i := strings.LastIndexByte(p, '/'); i >= 0 {
		p = p[i+1:]
	}
	p = strings.ToLower(p)
	if len(p) > 15 {
		p = p[:15]
	}
	return p
}

func scanProcs() map[string]bool {
	res := map[string]bool{}
	entries, err := os.ReadDir("/proc")
	if err != nil {
		return res
	}
	for _, e := range entries {
		if _, err := strconv.Atoi(e.Name()); err != nil {
			continue
		}
		b, err := os.ReadFile("/proc/" + e.Name() + "/comm")
		if err != nil {
			continue
		}
		res[strings.TrimSpace(strings.ToLower(string(b)))] = true
	}
	return res
}
