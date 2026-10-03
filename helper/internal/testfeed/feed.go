// Package testfeed plays the OBS plugin's role over the helper wire protocol,
// for the loopback tests and the airplay-feed tool.
package testfeed

import (
	"context"
	"encoding/json"
	"time"

	"doubletake/internal/bridge"
	"doubletake/internal/testmedia"
)

// Feed plays the role of the OBS plugin: it sends hello, closes helloSent (if
// non-nil) so callers may start sending commands, then streams the clip at
// 30 fps and the tone in 10 ms blocks until ctx ends. Timestamps come from a
// monotonic clock with capture_ns == send_ns (zero pipeline age).
func Feed(ctx context.Context, w *bridge.MessageWriter, helloSent chan<- struct{}) error {
	return FeedWith(ctx, w, helloSent, Options{})
}

// Options selects test-clip variants for on-site decoder diagnosis.
type Options struct {
	Clip1080 bool // use the 1920x1080 Main-profile clip instead of 640x360 High
	StripAUD bool // remove access unit delimiter NALs before sending
	// AudioPPM runs the tone this many parts per million fast against the
	// wall clock (0: exact), to reproduce a capture device's clock offset.
	AudioPPM float64
	// VideoFPS paces the clip at this exact rate by elapsed time, with each
	// frame's capture time set by its index (0: the clip's 30 fps on a ticker).
	// Use 60000.0/1001 for OBS's 59.94.
	VideoFPS float64
}

// FeedWith is Feed with clip options.
func FeedWith(ctx context.Context, w *bridge.MessageWriter, helloSent chan<- struct{}, opts Options) error {
	hello, _ := json.Marshal(bridge.DefaultHello())
	if err := w.Write(bridge.MsgHello, hello); err != nil {
		return err
	}
	if helloSent != nil {
		close(helloSent)
	}
	epoch := time.Now()
	// +1 keeps the timestamp nonzero even on the very first tick, when
	// time.Since(epoch) can read as 0; a zero send_ns/capture_ns pair would
	// look like an unset timestamp to a consumer.
	stamp := func() uint64 { return uint64(time.Since(epoch)) + 1 }
	aus := testmedia.VideoAUs()
	if opts.Clip1080 {
		aus = testmedia.VideoAUs1080()
	}
	if opts.StripAUD {
		stripped := make([][]byte, len(aus))
		for i, au := range aus {
			stripped[i] = testmedia.StripAUD(au)
		}
		aus = stripped
	}
	tone := testmedia.NewToneClockPPM(epoch, opts.AudioPPM)
	// With VideoFPS the ticker only polls the frame clock.
	var frames *testmedia.FrameClock
	vt := time.NewTicker(time.Second / testmedia.ClipFPS)
	if opts.VideoFPS > 0 {
		frames = testmedia.NewFrameClock(epoch, opts.VideoFPS)
		vt.Reset(2 * time.Millisecond)
	}
	at := time.NewTicker(10 * time.Millisecond)
	defer vt.Stop()
	defer at.Stop()
	for i := 0; ; {
		select {
		case <-ctx.Done():
			return nil
		case <-vt.C:
			send := stamp()
			captures := []uint64{send}
			if frames != nil {
				captures = captures[:0]
				for _, offset := range frames.Due(time.Now()) {
					captures = append(captures, uint64(offset)+1)
				}
			}
			for _, capture := range captures {
				au := aus[i%len(aus)]
				if err := w.Write(bridge.MsgVideoAU, bridge.EncodeMedia(bridge.Media{CaptureNs: capture, SendNs: send, Keyframe: isKeyframe(au), Data: au})); err != nil {
					return err
				}
				i++
			}
		case <-at.C:
			// Capture time is the block's place in real time, not the tick's:
			// see testmedia.ToneClock.
			send := stamp()
			for _, b := range tone.Due(time.Now()) {
				capture := uint64(b.Offset) + 1
				if err := w.Write(bridge.MsgAudioPCM, bridge.EncodeMedia(bridge.Media{CaptureNs: capture, SendNs: send, Data: b.PCM})); err != nil {
					return err
				}
			}
		}
	}
}

func isKeyframe(au []byte) bool {
	for j := 0; j+4 < len(au); j++ {
		if au[j] == 0 && au[j+1] == 0 && au[j+2] == 1 && au[j+3]&0x1f == 5 {
			return true
		}
	}
	return false
}
