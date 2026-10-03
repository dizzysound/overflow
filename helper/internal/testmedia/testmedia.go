// Package testmedia provides deterministic media for helper tests and the
// airplay-feed spike tool: a 640x360 30 fps H.264 clip and a PCM tone.
package testmedia

import (
	_ "embed"
	"encoding/binary"
	"math"
)

const (
	ClipWidth  = 640
	ClipHeight = 360
	ClipFPS    = 30
	SampleRate = 44100
)

//go:embed testsrc.h264
var clip []byte

// VideoAUs splits the embedded clip into access units at each AUD NAL
// (00 00 00 01 09). ffmpeg's h264_metadata=aud=insert guarantees one per frame.
func VideoAUs() [][]byte { return splitAUs(clip) }

//go:embed testsrc1080.h264
var clip1080 []byte

// VideoAUs1080 splits a 1920x1080 Main-profile variant of the test clip (closer
// to real OBS encoder output) the same way as VideoAUs.
func VideoAUs1080() [][]byte { return splitAUs(clip1080) }

// StripAUD returns au without its access unit delimiter NAL (type 9). iOS
// senders do not send AUDs; some hardware decoders may not expect them.
func StripAUD(au []byte) []byte {
	if len(au) >= 6 && au[0] == 0 && au[1] == 0 && au[2] == 0 && au[3] == 1 && au[4]&0x1f == 9 {
		for i := 5; i+3 < len(au); i++ {
			if au[i] == 0 && au[i+1] == 0 && (au[i+2] == 1 || (au[i+2] == 0 && i+3 < len(au) && au[i+3] == 1)) {
				return au[i:]
			}
		}
	}
	return au
}

func splitAUs(clip []byte) [][]byte {
	var aus [][]byte
	start := -1
	for i := 0; i+4 < len(clip); i++ {
		if clip[i] == 0 && clip[i+1] == 0 && clip[i+2] == 0 && clip[i+3] == 1 && clip[i+4]&0x1f == 9 {
			if start >= 0 {
				aus = append(aus, clip[start:i])
			}
			start = i
		}
	}
	if start >= 0 {
		aus = append(aus, clip[start:])
	}
	return aus
}

// Tone returns samples frames of a 440 Hz sine at 20% amplitude as S16LE
// stereo. phase carries the oscillator between calls.
func Tone(samples int, phase *float64) []byte {
	out := make([]byte, samples*4)
	step := 2 * math.Pi * 440 / SampleRate
	for i := 0; i < samples; i++ {
		v := int16(0.2 * math.MaxInt16 * math.Sin(*phase))
		binary.LittleEndian.PutUint16(out[i*4:], uint16(v))
		binary.LittleEndian.PutUint16(out[i*4+2:], uint16(v))
		*phase += step
		if *phase > 2*math.Pi {
			*phase -= 2 * math.Pi
		}
	}
	return out
}
