package airplay

import (
	"testing"

	"howett.net/plist"
)

func TestDisplaySummaryReportsFrameRateFields(t *testing.T) {
	payload, err := plist.Marshal(map[string]interface{}{
		"displays": []map[string]interface{}{{
			"widthPixels":  int64(1920),
			"heightPixels": int64(1080),
			"maxFPS":       int64(60),
			"refreshRate":  1.0 / 60,
		}},
	}, plist.BinaryFormat)
	if err != nil {
		t.Fatal(err)
	}
	var info ReceiverInfo
	if _, err := plist.Unmarshal(payload, &info); err != nil {
		t.Fatal(err)
	}
	if got, want := displaySummary(&info), "1920x1080 maxFPS 60 refreshRate 0.016667"; got != want {
		t.Fatalf("summary %q, want %q", got, want)
	}
}

func TestDisplaySummaryWithoutFields(t *testing.T) {
	if got := displaySummary(&ReceiverInfo{}); got != "no displays in /info" {
		t.Fatalf("summary %q", got)
	}
	info := &ReceiverInfo{Displays: []DisplayInfo{{Width: 1280, Height: 720}}}
	if got, want := displaySummary(info), "1280x720 maxFPS - refreshRate -"; got != want {
		t.Fatalf("summary %q, want %q", got, want)
	}
}
