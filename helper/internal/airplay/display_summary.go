package airplay

import (
	"fmt"
	"strconv"
)

// displaySummary describes the receiver's first advertised display for the
// per-session log: size, maxFPS and refreshRate, "-" for a field it omits.
// It records what a TV claims about its screen, e.g. to compare a 60 Hz panel
// with OBS's 59.94 fps.
func displaySummary(info *ReceiverInfo) string {
	if info == nil || len(info.Displays) == 0 {
		return "no displays in /info"
	}
	d := info.Displays[0]
	w, h := d.WidthPixels, d.HeightPixels
	if w <= 0 || h <= 0 {
		w, h = d.Width, d.Height
	}
	fps, refresh := "-", "-"
	if d.MaxFPS > 0 {
		fps = strconv.Itoa(int(d.MaxFPS))
	}
	if d.RefreshRate > 0 {
		refresh = strconv.FormatFloat(float64(d.RefreshRate), 'f', 6, 64)
	}
	return fmt.Sprintf("%dx%d maxFPS %s refreshRate %s", w, h, fps, refresh)
}
