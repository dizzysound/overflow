// Command airplay-feed stands in for the OBS plugin. It launches
// overflow-helper, feeds the embedded test clip and tone, selects the rooms
// named on the command line, prints helper events, and asks for PINs on the
// terminal. Typed lines starting with "/" are commands instead of PINs:
// "/restart DEVICEID" and "/volume DEVICEID DB". It is the Windows on-site
// spike tool and a reference client for docs/protocol.md.
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"os/signal"
	"strconv"
	"strings"
	"time"

	"doubletake/internal/airplay"
	"doubletake/internal/bridge"
	"doubletake/internal/testfeed"
)

// credPrompt is one outstanding "enter this credential" prompt, kept in FIFO
// order so a typed line always answers the prompt it was printed for.
type credPrompt struct {
	deviceID string
	kind     string
}

func main() {
	helperPath := flag.String("helper", "overflow-helper", "path to overflow-helper")
	rooms := flag.String("rooms", "", "comma-separated rooms to stream to: DEVICEID, or DEVICEID@IP[:PORT] to send a manual address in set_displays (empty = just list devices)")
	var roomVolumes stringList
	flag.Var(&roomVolumes, "room-volume", "per-room volume_db for set_displays, DEVICEID=DB with DB from -30 to 0 (repeatable)")
	var audioOffDevices stringList
	flag.Var(&audioOffDevices, "display-audio-off", "DEVICEID to select with audio:false in set_displays (video only; repeatable)")
	var wifiTolerantDevices stringList
	flag.Var(&wifiTolerantDevices, "display-wifi-tolerant", "DEVICEID to select with wifi_tolerant:true in set_displays (250 ms relay budget; repeatable)")
	var latencyDevices stringList
	flag.Var(&latencyDevices, "display-latency", "per-display latency_ms for set_displays, DEVICEID=MS (repeatable)")
	var headroomDevices stringList
	flag.Var(&headroomDevices, "display-lead-headroom", "per-display lead_headroom_ms for set_displays, DEVICEID=MS, 1-500 (repeatable; needs -display-latency)")
	leadSlidePPM := flag.Int("lead-slide-ppm", 0, "passed to the helper (0 = its default)")
	creds := flag.String("creds", "airplay-feed-credentials.json", "credentials file passed to the helper")
	debug := flag.Bool("debug", false, "pass -debug to the helper")
	timing := flag.String("timing", "auto", "passed to the helper: auto or ntp (ntp is the fallback if PTP mirroring fails)")
	audioKey := flag.String("audio-key", "auto", "passed to the helper: auto, raw (unhashed FairPlay audio key, old AirPlay 1 receivers) or mixed (always hashed with the pair-verify secret)")
	videoKey := flag.String("video-key", "auto", "passed to the helper: auto, raw (unhashed FairPlay video key, old AirPlay 1 receivers) or mixed (always hashed with the pair-verify secret)")
	var manualDevices stringList
	flag.Var(&manualDevices, "device", "passed to the helper: manual receiver DEVICEID@IP[:PORT] for receivers mDNS misses (repeatable)")
	volume := flag.String("volume", "keep", "passed to the helper: keep (never change receiver volume) or a dB level from -30 to 0")
	clip1080 := flag.Bool("clip1080", false, "send the 1920x1080 Main-profile test clip instead of 640x360 High")
	stripAUD := flag.Bool("strip-aud", false, "remove H.264 access unit delimiters from the test clip")
	portRange := flag.String("port-range", "", "passed to the helper: local UDP port range, e.g. 60000-60010 (empty = ephemeral)")
	discovery := flag.String("discovery", "auto", "passed to the helper: auto or zeroconf")
	audioPPM := flag.Float64("audio-ppm", 0, "run the test tone this many ppm fast against the PC clock (negative: slow), as a capture device on its own crystal does")
	videoFPS := flag.String("video-fps", "", "pace the test clip at exactly this frame rate by elapsed time, e.g. 30, 60, 59.94 (= 60000/1001, as OBS) or 29.97 (empty = 30 fps on a ticker)")
	duration := flag.Duration("duration", 0, "stop after this long, e.g. 60s (0 = run until Ctrl+C)")
	flag.Parse()

	fps, err := parseVideoFPS(*videoFPS)
	if err != nil {
		log.Fatal(err)
	}
	selection, err := parseRoomsWithHeadroom(*rooms, roomVolumes, audioOffDevices, wifiTolerantDevices, latencyDevices, headroomDevices)
	if err != nil {
		log.Fatal(err)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	if *duration > 0 {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, *duration)
		defer cancel()
	}

	args := []string{"-creds", *creds, "-timing", *timing, "-audio-key", *audioKey, "-video-key", *videoKey, "-volume", *volume, "-discovery", *discovery}
	for _, d := range manualDevices {
		args = append(args, "-device", d)
	}
	if *portRange != "" {
		args = append(args, "-port-range", *portRange)
	}
	if *leadSlidePPM != 0 {
		args = append(args, "-lead-slide-ppm", strconv.Itoa(*leadSlidePPM))
	}
	if *debug {
		args = append(args, "-debug")
	}
	// Plain exec.Command, not CommandContext: on ctx cancellation (Ctrl+C) we
	// want to shut the helper down cleanly via stdin EOF, not have the
	// stdlib SIGKILL it out from under us.
	cmd := exec.Command(*helperPath, args...)
	cmd.Stderr = os.Stderr
	stdin, err := cmd.StdinPipe()
	if err != nil {
		log.Fatal(err)
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		log.Fatal(err)
	}
	if err := cmd.Start(); err != nil {
		log.Fatalf("start helper: %v", err)
	}

	// Drain the helper's stdout into a channel immediately, before anything
	// else runs, so the helper's single main loop is never blocked on a full
	// stdout pipe waiting for us to read it.
	lines := make(chan string, 64)
	go func() {
		sc := bufio.NewScanner(stdout)
		for sc.Scan() {
			lines <- sc.Text()
		}
		close(lines)
	}()

	w := bridge.NewMessageWriter(stdin)
	helloSent := make(chan struct{})
	go func() {
		if err := testfeed.FeedWith(ctx, w, helloSent, testfeed.Options{Clip1080: *clip1080, StripAUD: *stripAUD, AudioPPM: *audioPPM, VideoFPS: fps}); err != nil {
			log.Printf("feed: %v", err)
		}
	}()
	// The helper rejects any message before hello, so commands wait for it.
	select {
	case <-helloSent:
	case <-time.After(10 * time.Second):
		log.Print("helper did not accept hello within 10 s (did it exit? see stderr)")
		shutdown(cmd, stdin)
		os.Exit(1)
	case <-ctx.Done():
		shutdown(cmd, stdin)
		os.Exit(1)
	}

	if len(selection) > 0 {
		b, _ := json.Marshal(bridge.Command{Cmd: "set_displays", Displays: selection})
		if err := w.Write(bridge.MsgCommand, b); err != nil {
			log.Printf("send set_displays: %v", err)
			shutdown(cmd, stdin)
			os.Exit(1)
		}
	}

	// Read the terminal on its own goroutine: it must never share a
	// goroutine with the stdout-draining loop, or a slow/absent human at the
	// keyboard would stall event processing (and, transitively, the
	// helper's own stdout writes) while ReadString blocks.
	typed := make(chan string)
	go func() {
		terminal := bufio.NewReader(os.Stdin)
		for {
			value, err := terminal.ReadString('\n')
			if err != nil {
				return
			}
			typed <- strings.TrimSpace(value)
		}
	}()

	var pending []credPrompt
	printPrompt := func() {
		if len(pending) > 0 {
			fmt.Printf("Enter %s for %s: ", pending[0].kind, pending[0].deviceID)
		}
	}

loop:
	for {
		select {
		case line, ok := <-lines:
			if !ok {
				break loop
			}
			fmt.Println(line)
			var ev map[string]any
			if json.Unmarshal([]byte(line), &ev) != nil {
				continue
			}
			isDisplayEvent := ev["event"] == "display" || ev["event"] == "room" // "room" kept for compatibility with older helpers
			if isDisplayEvent && ev["state"] == "credential" {
				pending = append(pending, credPrompt{
					deviceID: fmt.Sprint(ev["device_id"]),
					kind:     fmt.Sprint(ev["credential_kind"]),
				})
				if len(pending) == 1 {
					printPrompt()
				}
			}
		case value := <-typed:
			if strings.HasPrefix(value, "/") {
				c, err := parseTypedCommand(value)
				if err != nil {
					fmt.Println(err)
				} else {
					b, _ := json.Marshal(c)
					if err := w.Write(bridge.MsgCommand, b); err != nil {
						log.Printf("send %s: %v", c.Cmd, err)
					}
				}
				printPrompt()
				continue
			}
			if len(pending) == 0 {
				continue
			}
			p := pending[0]
			pending = pending[1:]
			b, _ := json.Marshal(bridge.Command{Cmd: "credential", DeviceID: p.deviceID, Value: value})
			if err := w.Write(bridge.MsgCommand, b); err != nil {
				log.Printf("send credential: %v", err)
			}
			printPrompt()
		case <-ctx.Done():
			break loop
		}
	}

	shutdown(cmd, stdin)
}

// shutdown is the single exit path for the helper subprocess: close its
// stdin so it exits on EOF, give it a few seconds to do so, and kill it if
// it hasn't. Every place main() stops running goes through this, so
// overflow-helper is never left orphaned when airplay-feed quits or is
// interrupted.
// parseVideoFPS reads -video-fps. 59.94 and 29.97 mean the exact NTSC rates
// (60000/1001, 30000/1001); the decimal 59.94 is 1 ppm off OBS's rate.
func parseVideoFPS(s string) (float64, error) {
	switch s {
	case "":
		return 0, nil
	case "59.94":
		return 60000.0 / 1001, nil
	case "29.97":
		return 30000.0 / 1001, nil
	}
	fps, err := strconv.ParseFloat(s, 64)
	if err != nil || fps <= 0 || fps > 120 {
		return 0, fmt.Errorf("-video-fps %q: want a rate above 0 and up to 120", s)
	}
	return fps, nil
}

func shutdown(cmd *exec.Cmd, stdin io.Closer) {
	_ = stdin.Close()
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		_ = cmd.Process.Kill()
		<-done
	}
}

// stringList collects a repeatable string flag.
type stringList []string

func (l *stringList) String() string     { return strings.Join(*l, ",") }
func (l *stringList) Set(v string) error { *l = append(*l, v); return nil }

// parseRooms turns -rooms entries (DEVICEID or DEVICEID@IP[:PORT]),
// -room-volume entries (DEVICEID=DB), -display-audio-off entries (DEVICEID) and
// -display-wifi-tolerant entries (DEVICEID) and -display-latency entries (DEVICEID=MS) into set_displays selections.
// Volume, audio-off and Wi-Fi tolerant entries match rooms case-insensitively.
func parseRooms(rooms string, volumes, audioOff, wifiTolerant, latencies []string) ([]bridge.RoomSelection, error) {
	return parseRoomsWithHeadroom(rooms, volumes, audioOff, wifiTolerant, latencies, nil)
}

// parseRoomsWithHeadroom is parseRooms plus -display-lead-headroom entries
// (DEVICEID=MS, 1 to 500), which set lead_headroom_ms.
func parseRoomsWithHeadroom(rooms string, volumes, audioOff, wifiTolerant, latencies, headrooms []string) ([]bridge.RoomSelection, error) {
	levels := make(map[string]float64, len(volumes))
	for _, v := range volumes {
		id, dbText, ok := strings.Cut(v, "=")
		id = strings.TrimSpace(id)
		if !ok || id == "" {
			return nil, fmt.Errorf("-room-volume %q: want DEVICEID=DB", v)
		}
		db, err := strconv.ParseFloat(strings.TrimSpace(dbText), 64)
		if err != nil {
			return nil, fmt.Errorf("-room-volume %q: %v", v, err)
		}
		if err := airplay.CheckVolumeDB(db); err != nil {
			return nil, fmt.Errorf("-room-volume %q: %v", v, err)
		}
		levels[strings.ToUpper(id)] = db
	}
	noAudio := make(map[string]bool, len(audioOff))
	for _, v := range audioOff {
		id := strings.TrimSpace(v)
		if id == "" {
			return nil, fmt.Errorf("-display-audio-off %q: want DEVICEID", v)
		}
		noAudio[strings.ToUpper(id)] = true
	}
	tolerant := make(map[string]bool, len(wifiTolerant))
	for _, v := range wifiTolerant {
		id := strings.TrimSpace(v)
		if id == "" {
			return nil, fmt.Errorf("-display-wifi-tolerant %q: want DEVICEID", v)
		}
		tolerant[strings.ToUpper(id)] = true
	}
	leads := make(map[string]int, len(latencies))
	for _, v := range latencies {
		id, msText, ok := strings.Cut(v, "=")
		id = strings.ToUpper(strings.TrimSpace(id))
		ms, err := strconv.Atoi(strings.TrimSpace(msText))
		if !ok || id == "" || err != nil || ms <= 0 {
			return nil, fmt.Errorf("-display-latency %q: want DEVICEID=MS with MS > 0", v)
		}
		leads[id] = ms
	}
	headroomMs := make(map[string]int, len(headrooms))
	for _, v := range headrooms {
		id, msText, ok := strings.Cut(v, "=")
		id = strings.ToUpper(strings.TrimSpace(id))
		ms, err := strconv.Atoi(strings.TrimSpace(msText))
		if !ok || id == "" || err != nil || ms < 1 || ms > 500 {
			return nil, fmt.Errorf("-display-lead-headroom %q: want DEVICEID=MS with MS from 1 to 500", v)
		}
		headroomMs[id] = ms
	}
	var sel []bridge.RoomSelection
	for _, entry := range strings.Split(rooms, ",") {
		entry = strings.TrimSpace(entry)
		if entry == "" {
			continue
		}
		s := bridge.RoomSelection{DeviceID: entry, AutoReconnect: true}
		if strings.Contains(entry, "@") {
			d, err := bridge.ParseDevice(entry)
			if err != nil {
				return nil, err
			}
			s.DeviceID, s.IP, s.Port = d.DeviceID, d.IP, d.Port
		}
		if db, ok := levels[strings.ToUpper(s.DeviceID)]; ok {
			level := db
			s.VolumeDB = &level
		}
		if noAudio[strings.ToUpper(s.DeviceID)] {
			off := false
			s.Audio = &off
		}
		if tolerant[strings.ToUpper(s.DeviceID)] {
			on := true
			s.WifiTolerant = &on
		}
		if ms, ok := leads[strings.ToUpper(s.DeviceID)]; ok {
			lead := ms
			s.LatencyMs = &lead
		}
		if ms, ok := headroomMs[strings.ToUpper(s.DeviceID)]; ok {
			headroom := ms
			s.LeadHeadroomMs = &headroom
		}
		sel = append(sel, s)
	}
	return sel, nil
}

// parseTypedCommand reads an operator line that starts with "/":
// "/restart DEVICEID", "/volume DEVICEID DB" or "/lead DEVICEID MS".
func parseTypedCommand(line string) (bridge.Command, error) {
	const usage = "commands: /restart DEVICEID, /volume DEVICEID DB, /lead DEVICEID MS"
	fields := strings.Fields(strings.TrimPrefix(line, "/"))
	if len(fields) == 0 {
		return bridge.Command{}, errors.New(usage)
	}
	switch fields[0] {
	case "restart":
		if len(fields) != 2 {
			return bridge.Command{}, errors.New("usage: /restart DEVICEID")
		}
		return bridge.Command{Cmd: "restart", DeviceID: fields[1]}, nil
	case "volume":
		if len(fields) != 3 {
			return bridge.Command{}, errors.New("usage: /volume DEVICEID DB")
		}
		db, err := strconv.ParseFloat(fields[2], 64)
		if err != nil {
			return bridge.Command{}, fmt.Errorf("volume %q: %v", fields[2], err)
		}
		if err := airplay.CheckVolumeDB(db); err != nil {
			return bridge.Command{}, err
		}
		return bridge.Command{Cmd: "set_volume", DeviceID: fields[1], VolumeDB: &db}, nil
	case "lead":
		if len(fields) != 3 {
			return bridge.Command{}, errors.New("usage: /lead DEVICEID MS")
		}
		ms, err := strconv.Atoi(fields[2])
		if err != nil || ms <= 0 || ms > 2000 {
			return bridge.Command{}, fmt.Errorf("/lead %s: want 1 to 2000 ms", fields[2])
		}
		return bridge.Command{Cmd: "set_lead", DeviceID: fields[1], LeadMs: &ms}, nil
	default:
		return bridge.Command{}, fmt.Errorf("unknown command /%s; %s", fields[0], usage)
	}
}
