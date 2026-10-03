package testmedia

import "testing"

func nalTypes(au []byte) []byte {
	var types []byte
	for i := 0; i+3 < len(au); i++ {
		if au[i] == 0 && au[i+1] == 0 && au[i+2] == 1 {
			types = append(types, au[i+3]&0x1f)
			i += 2
		}
	}
	return types
}

func TestVideoAUsSplitsClipIntoSixtyFrames(t *testing.T) {
	aus := VideoAUs()
	if len(aus) != 60 {
		t.Fatalf("got %d access units, want 60 (2 s at 30 fps)", len(aus))
	}
	for i, au := range aus {
		types := nalTypes(au)
		if len(types) == 0 || types[0] != 9 {
			t.Fatalf("AU %d does not start with an AUD: %v", i, types)
		}
	}
}

func TestEveryIDRCarriesSPSAndPPS(t *testing.T) {
	idrs := 0
	for i, au := range VideoAUs() {
		types := nalTypes(au)
		has := map[byte]bool{}
		for _, ty := range types {
			has[ty] = true
		}
		if has[5] {
			idrs++
			if !has[7] || !has[8] {
				t.Fatalf("IDR AU %d lacks SPS/PPS: %v", i, types)
			}
		}
	}
	if idrs != 2 {
		t.Fatalf("got %d IDRs, want 2 (GOP 30 over 60 frames)", idrs)
	}
}

func TestToneLengthAndContinuity(t *testing.T) {
	var phase float64
	a := Tone(441, &phase)
	if len(a) != 441*4 {
		t.Fatalf("len = %d, want %d", len(a), 441*4)
	}
	b := Tone(441, &phase)
	if string(a) == string(b) {
		t.Fatal("phase did not advance between calls")
	}
}

func TestClip1080AndStripAUD(t *testing.T) {
	aus := VideoAUs1080()
	if len(aus) != 60 {
		t.Fatalf("1080 clip: %d AUs, want 60", len(aus))
	}
	for i, au := range aus {
		s := StripAUD(au)
		types := nalTypes(s)
		if len(types) == 0 || types[0] == 9 {
			t.Fatalf("AU %d still starts with AUD after strip: %v", i, types)
		}
		for _, ty := range types {
			if ty == 9 {
				t.Fatalf("AU %d contains an AUD after strip", i)
			}
		}
	}
}
