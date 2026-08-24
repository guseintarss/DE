package main

import (
	"context"
	"time"

	"github.com/godbus/dbus/v5"
)

const (
	wmService = "com.myde.WindowManager"
	wmPath    = dbus.ObjectPath("/com/myde/WindowManager")
	wmMethod  = wmService + ".LaunchApp"
)

var dbusObj dbus.BusObject

func initDBus() {
	conn, err := dbus.ConnectSessionBus()
	if err != nil {
		return
	}
	dbusObj = conn.Object(wmService, wmPath)
}

func dbusLaunch(name string) bool {
	if dbusObj == nil {
		return false
	}
	ctx, cancel := context.WithTimeout(context.Background(), 1500*time.Millisecond)
	defer cancel()
	var ok bool
	err := dbusObj.CallWithContext(ctx, wmMethod, 0, name).Store(&ok)
	return err == nil && ok
}
