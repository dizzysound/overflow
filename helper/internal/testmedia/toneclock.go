package testmedia

import "time"

// toneBlock is the duration of one ToneBlock: 441 samples at 44.1 kHz.
const toneBlock = 10 * time.Millisecond

// ToneBlock is 10 ms of test tone and its capture offset from the clock's start.
type ToneBlock struct {
	Offset time.Duration
	PCM    []byte
}

// ToneClock paces the test tone by elapsed time rather than by ticker ticks.
// A ticker drops ticks under load, and Windows timers can be coarser than
// 10 ms, so one block per tick delivers less audio than real time. The helper
// times buffered audio by sample count, so that shortfall shows up as audio
// falling ever further behind the wall clock until a session gives up on it
// as stale (CI, windows-latest, 2026-10-01: source age 440 ms).
type ToneClock struct {
	start time.Time
	rate  float64 // audio samples per wall-clock sample: 1 + ppm/1e6
	sent  int
	phase float64
}

func NewToneClock(start time.Time) *ToneClock { return &ToneClock{start: start, rate: 1} }

// NewToneClockPPM runs the tone ppm parts per million fast against the wall
// clock (negative: slow), as a capture device on its own crystal does. It
// reproduces a sender whose audio rate disagrees with its advertised clock.
func NewToneClockPPM(start time.Time, ppm float64) *ToneClock {
	return &ToneClock{start: start, rate: 1 + ppm/1e6}
}

// Due returns every block owed up to now, in order.
func (c *ToneClock) Due(now time.Time) []ToneBlock {
	// The epsilon keeps a block boundary that rounds to 0.9999... owed.
	due := int(float64(now.Sub(c.start))*c.rate/float64(toneBlock) + 1e-6)
	var out []ToneBlock
	for ; c.sent < due; c.sent++ {
		offset := time.Duration(float64(time.Duration(c.sent)*toneBlock) / c.rate)
		out = append(out, ToneBlock{Offset: offset, PCM: Tone(441, &c.phase)})
	}
	return out
}

// FrameClock paces test video by elapsed time at an exact, possibly
// fractional, frame rate (59.94 = 60000/1001, as OBS runs on the streaming PC). A
// ticker's period is rounded to whole nanoseconds and it drops ticks under
// load; the frame index sets each capture offset instead, so a receiver sees
// the nominal rate exactly.
type FrameClock struct {
	start time.Time
	fps   float64
	sent  int
}

func NewFrameClock(start time.Time, fps float64) *FrameClock {
	return &FrameClock{start: start, fps: fps}
}

// Due returns the capture offset of every frame owed up to now, in order.
// Frame 0 is due at the start.
func (c *FrameClock) Due(now time.Time) []time.Duration {
	// The epsilon keeps a frame boundary that rounds to 0.9999... owed.
	due := int(now.Sub(c.start).Seconds()*c.fps+1e-6) + 1
	var out []time.Duration
	for ; c.sent < due; c.sent++ {
		out = append(out, time.Duration(float64(c.sent)*float64(time.Second)/c.fps))
	}
	return out
}
