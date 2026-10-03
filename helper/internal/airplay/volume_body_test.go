package airplay

import "testing"

func TestVolumeDBBody(t *testing.T) {
	if got := string(volumeDBBody(-12.5)); got != "volume: -12.500000\r\n" {
		t.Fatalf("got %q", got)
	}
}
