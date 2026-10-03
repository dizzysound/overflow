package airplay

import (
	"fmt"
	"math"
)

// CheckVolumeDB validates a receiver volume level in dB for the helper's
// per-room volume: -30 (quietest level offered) to 0 (full scale). AirPlay's
// -144 dB mute value is deliberately outside this range.
func CheckVolumeDB(db float64) error {
	if math.IsNaN(db) || db < -30 || db > 0 {
		return fmt.Errorf("volume_db %v: want a level from -30 to 0 dB", db)
	}
	return nil
}

// SetVolume sets the receiver's volume on a live session with SET_PARAMETER,
// the same request SetAudioMuted uses, but with an arbitrary level. Receivers
// whose volumeControlType drives the TV's own volume (the Roku TV on site)
// apply it to the TV.
func (s *MirrorSession) SetVolume(db float64) error {
	if err := CheckVolumeDB(db); err != nil {
		return err
	}
	if s == nil || s.client == nil || s.sessionURI == "" {
		return fmt.Errorf("audio control unavailable")
	}
	if _, _, err := s.client.rtspRequest("SET_PARAMETER", s.sessionURI, "text/parameters", volumeDBBody(db), nil); err != nil {
		return fmt.Errorf("set volume %.1f dB: %w", db, err)
	}
	return nil
}
