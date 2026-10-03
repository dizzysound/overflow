// Command overflow-helper is the Overflow OBS plugin's sidecar. It reads the
// plugin protocol on stdin (see docs/protocol.md), writes JSON events on
// stdout, and logs to stderr. It exits when stdin closes.
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"time"

	"doubletake/internal/airplay"
	"doubletake/internal/bridge"
)

func main() {
	creds := flag.String("creds", "", "path to the pairing credentials file (used with -cred-backend file)")
	credBackend := flag.String("cred-backend", "file", "credential storage: file or keyring")
	portRange := flag.String("port-range", "", "local UDP port range for timing/audio, e.g. 60000-60010 (empty = ephemeral)")
	fps := flag.Int("fps", 30, "OBS output frame rate, 1-120")
	debug := flag.Bool("debug", false, "verbose protocol logging to stderr")
	timing := flag.String("timing", "auto", "timing protocol: auto or ntp (fallback for receivers where PTP mirroring fails, e.g. the Apple TV HD)")
	audioKey := flag.String("audio-key", "auto", "legacy AES audio key: auto (as pair-verify negotiated), raw (unhashed FairPlay key, for old AirPlay 1 receivers that play hashed-key audio as noise) or mixed (always hashed with the pair-verify secret)")
	videoKey := flag.String("video-key", "auto", "legacy AES video key: auto (as pair-verify negotiated), raw (unhashed FairPlay key, for old AirPlay 1 receivers) or mixed (always hashed with the pair-verify secret)")
	volume := flag.String("volume", "keep", "receiver volume at session start: keep (never change it) or a level in dB from -30 to 0 (0 = full scale)")
	var devices deviceFlags
	flag.Var(&devices, "device", "manual receiver DEVICEID@IP[:PORT] for receivers mDNS misses (repeatable)")
	targetLatencyMs := flag.Int("target-latency-ms", 0, "joint audio/video playout latency in milliseconds, 0-2000 (0 = the automatic AirPlay policy)")
	leadSlidePPM := flag.Int("lead-slide-ppm", 0, "rate at which a live session slides its TV delay, ppm of elapsed time, 1-5000 (0 = 300, for on-site tests)")
	eldEncoder := flag.String("eld-encoder", "", "path to the eld-encoder program for AAC-ELD audio (default: eld-encoder next to this executable; when absent, AAC-ELD-only receivers get video without audio)")
	discovery := flag.String("discovery", "auto", "receiver discovery: auto (the Windows DNS-SD service when available, else the built-in mDNS listener) or zeroconf (built-in listener only)")
	flag.Parse()

	log.SetOutput(os.Stderr)
	log.SetFlags(log.Ltime | log.Lmicroseconds)

	lo, hi, err := bridge.ParsePortRange(*portRange)
	if err != nil {
		log.Fatal(err)
	}
	if *fps < 1 || *fps > 120 {
		log.Fatalf("-fps %d: want a frame rate from 1 to 120", *fps)
	}
	if *credBackend != "file" && *credBackend != "keyring" {
		log.Fatalf("-cred-backend %q: want file or keyring", *credBackend)
	}
	if *credBackend == "file" && *creds == "" {
		log.Fatal("-creds is required with -cred-backend file")
	}
	if *timing != "auto" && *timing != "ntp" {
		log.Fatalf("-timing %q: want auto or ntp", *timing)
	}
	audioKeyMode, err := airplay.ParseLegacyKeyMode(*audioKey)
	if err != nil {
		log.Fatalf("-audio-key: %v", err)
	}
	setVolume, volumeDB, err := bridge.ParseVolume(*volume)
	if err != nil {
		log.Fatal(err)
	}
	videoKeyMode, err := airplay.ParseLegacyKeyMode(*videoKey)
	if err != nil {
		log.Fatalf("-video-key: %v", err)
	}

	if *targetLatencyMs < 0 || *targetLatencyMs > 2000 {
		log.Fatalf("-target-latency-ms %d: want 0 to 2000", *targetLatencyMs)
	}
	airplay.SetTargetLatency(time.Duration(*targetLatencyMs) * time.Millisecond)
	if *leadSlidePPM != 0 {
		if err := airplay.SetLeadSlidePPM(*leadSlidePPM); err != nil {
			log.Fatal(err)
		}
	}
	airplay.SetELDEncoderPath(*eldEncoder)
	if err := airplay.SetDiscoveryBackend(*discovery); err != nil {
		log.Fatal(err)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()

	err = bridge.Run(ctx, bridge.Config{
		CredFile:     *creds,
		CredBackend:  *credBackend,
		PortMin:      lo,
		PortMax:      hi,
		Debug:        *debug,
		FPS:          *fps,
		ForceNTP:     *timing == "ntp",
		AudioKey:     audioKeyMode,
		VideoKey:     videoKeyMode,
		ExtraDevices: devices.list,
		SetVolume:    setVolume,
		VolumeDB:     volumeDB,
	}, os.Stdin, os.Stdout)
	if err != nil {
		log.Printf("overflow-helper: %v", err)
		os.Exit(1)
	}
}

// deviceFlags collects repeatable -device DEVICEID@IP[:PORT] entries.
type deviceFlags struct{ list []bridge.Device }

func (f *deviceFlags) String() string { return fmt.Sprint(len(f.list)) }

func (f *deviceFlags) Set(s string) error {
	d, err := bridge.ParseDevice(s)
	if err != nil {
		return err
	}
	f.list = append(f.list, d)
	return nil
}
