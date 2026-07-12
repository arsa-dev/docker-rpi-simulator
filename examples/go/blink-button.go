// SPDX-License-Identifier: AGPL-3.0-or-later
// blink-button.go — blink an LED and wait for a button using the sysfs GPIO
// interface, with the Go standard library only (no external modules).
//
// Edge detection uses select(2) with the value fd in the *exception* set — the way
// sysfs GPIO edges are surfaced to select(). Equivalent to poll()'s POLLPRI.
//
//	LED    = GPIO17 (output)
//	BUTTON = GPIO27 (input, rising edge)
//
// Run:  go run blink-button.go   (or: go build -o blink-button . && ./blink-button)
package main

import (
	"fmt"
	"os"
	"strings"
	"syscall"
	"time"
)

const (
	sysfs  = "/sys/class/gpio"
	led    = 17
	button = 27
)

func write(path, val string) error { return os.WriteFile(path, []byte(val), 0) }
func export(pin int)               { _ = write(sysfs+"/export", fmt.Sprint(pin)) }   // ignore EBUSY
func unexport(pin int)             { _ = write(sysfs+"/unexport", fmt.Sprint(pin)) }

func main() {
	// --- blink the LED ---
	export(led)
	if err := write(fmt.Sprintf("%s/gpio%d/direction", sysfs, led), "out"); err != nil {
		panic(err)
	}
	fmt.Printf("Blinking GPIO%d...\n", led)
	valPath := fmt.Sprintf("%s/gpio%d/value", sysfs, led)
	for i := 0; i < 6; i++ {
		level := "0"
		if i%2 == 0 {
			level = "1"
		}
		write(valPath, level)
		time.Sleep(100 * time.Millisecond)
	}
	write(valPath, "0")

	// --- set up the button: input, rising edge ---
	export(button)
	write(fmt.Sprintf("%s/gpio%d/direction", sysfs, button), "in")
	write(fmt.Sprintf("%s/gpio%d/edge", sysfs, button), "rising")

	f, err := os.Open(fmt.Sprintf("%s/gpio%d/value", sysfs, button))
	if err != nil {
		panic(err)
	}
	defer f.Close()
	buf := make([]byte, 8)
	f.Seek(0, 0)
	f.Read(buf) // initial read clears any pending state (rearm)

	fd := int(f.Fd())
	fmt.Printf("WAITING for button press on GPIO%d (up to 5s)...\n", button)

	var except syscall.FdSet
	except.Bits[fd/64] |= int64(1) << (uint(fd) % 64)
	tv := syscall.Timeval{Sec: 5, Usec: 0}

	n, err := syscall.Select(fd+1, nil, nil, &except, &tv)
	if err != nil && err != syscall.EINTR {
		panic(err)
	}

	rc := 3
	if n > 0 {
		f.Seek(0, 0)
		m, _ := f.Read(buf)
		fmt.Printf("Button pressed! value=%s\n", strings.TrimSpace(string(buf[:m])))
		rc = 0
	} else {
		fmt.Println("timeout: no button press")
	}

	unexport(led)
	unexport(button)
	os.Exit(rc)
}
