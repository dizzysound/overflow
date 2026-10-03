package bridge

import (
	"errors"
	"fmt"
	"log"
	"net"
	"strings"

	"doubletake/internal/airplay"
)

// sanitizeRooms clears optional per-room fields that fail validation, logging
// each one, so a bad value never reaches the daemon. The room itself stays
// selected: tolerant parsing, as for unknown commands. Device IDs are
// normalized to uppercase here (a set_rooms boundary) so a receiver reported
// in a different case by mDNS, -device, or an operator-typed command never
// splits into two rooms.
func sanitizeRooms(sel []RoomSelection) []RoomSelection {
	out := make([]RoomSelection, 0, len(sel))
	for _, s := range sel {
		s.DeviceID = strings.ToUpper(s.DeviceID)
		if s.VolumeDB != nil {
			if err := airplay.CheckVolumeDB(*s.VolumeDB); err != nil {
				log.Printf("[bridge] room %s: ignoring volume_db: %v", s.DeviceID, err)
				s.VolumeDB = nil
			}
		}
		if s.IP != "" || s.Port != 0 {
			if err := checkManualAddress(s.IP, s.Port); err != nil {
				log.Printf("[bridge] room %s: ignoring ip/port: %v", s.DeviceID, err)
				s.IP, s.Port = "", 0
			}
		}
		if _, err := airplay.ParseScreenAudioFormat(s.AudioFormat); err != nil {
			log.Printf("[bridge] room %s: ignoring audio_format: %v", s.DeviceID, err)
			s.AudioFormat = ""
		}
		out = append(out, s)
	}
	return out
}

// checkManualAddress validates set_rooms ip/port: an IP literal (the helper
// does no name resolution) and a port from 1 to 65535, or 0 for the default.
func checkManualAddress(ip string, port int) error {
	if ip == "" {
		return errors.New("port given without ip")
	}
	if net.ParseIP(ip) == nil {
		return fmt.Errorf("ip %q is not an IP address", ip)
	}
	if port < 0 || port > 65535 {
		return fmt.Errorf("port %d: want 1-65535", port)
	}
	return nil
}
