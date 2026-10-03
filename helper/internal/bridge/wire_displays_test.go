package bridge

import (
	"encoding/json"
	"testing"
)

// TestSetDisplaysDecodes verifies the canonical set_displays command (array
// key "displays") decodes into Command.Displays with the same entry fields
// as set_rooms.
func TestSetDisplaysDecodes(t *testing.T) {
	var c Command
	in := `{"cmd":"set_displays","displays":[{"device_id":"A","auto_reconnect":true,"volume_db":-12},{"device_id":"B","auto_reconnect":true,"ip":"10.20.0.178","port":7000}]}`
	if err := json.Unmarshal([]byte(in), &c); err != nil {
		t.Fatal(err)
	}
	if c.Cmd != "set_displays" {
		t.Fatalf("Cmd = %q, want set_displays", c.Cmd)
	}
	if len(c.Displays) != 2 {
		t.Fatalf("Displays = %+v, want 2 entries", c.Displays)
	}
	if c.Displays[0].DeviceID != "A" || c.Displays[0].VolumeDB == nil || *c.Displays[0].VolumeDB != -12 {
		t.Fatalf("Displays[0] = %+v", c.Displays[0])
	}
	if c.Displays[1].DeviceID != "B" || c.Displays[1].IP != "10.20.0.178" || c.Displays[1].Port != 7000 {
		t.Fatalf("Displays[1] = %+v", c.Displays[1])
	}
}

// TestSetRoomsStillDecodesAsAlias verifies set_rooms (the pre-1.1 name) still
// decodes into Command.Rooms, so an old client (or an old JSON blob) keeps
// working.
func TestSetRoomsStillDecodesAsAlias(t *testing.T) {
	var c Command
	in := `{"cmd":"set_rooms","rooms":[{"device_id":"A","auto_reconnect":true}]}`
	if err := json.Unmarshal([]byte(in), &c); err != nil {
		t.Fatal(err)
	}
	if c.Cmd != "set_rooms" || len(c.Rooms) != 1 || c.Rooms[0].DeviceID != "A" {
		t.Fatalf("decoded %+v", c)
	}
	if c.Displays != nil {
		t.Fatalf("Displays = %+v, want nil when only rooms is sent", c.Displays)
	}
}

// TestResolveDisplaysEmptyDisplaysWinsOverRooms verifies that an explicit
// "displays":[] alongside "rooms":[...] resolves to an empty selection, not
// to the rooms alias. encoding/json decodes an explicit empty JSON array
// into a non-nil, zero-length slice (distinct from an absent key, which
// decodes to nil), so presence must be checked with a nil check, not a
// length check.
func TestResolveDisplaysEmptyDisplaysWinsOverRooms(t *testing.T) {
	var c Command
	in := `{"cmd":"set_displays","displays":[],"rooms":[{"device_id":"A"}]}`
	if err := json.Unmarshal([]byte(in), &c); err != nil {
		t.Fatal(err)
	}
	if c.Displays == nil {
		t.Fatal("displays:[] must decode as a non-nil empty slice, not nil")
	}
	if got := resolveDisplays(c); len(got) != 0 {
		t.Fatalf("resolveDisplays = %+v, want empty (displays present and empty must win over rooms)", got)
	}
}

// TestResolveDisplaysEmptyDisplaysAlone verifies "displays":[] alone (no
// rooms key at all) also resolves to an empty selection -- an empty
// set_displays deselects every display.
func TestResolveDisplaysEmptyDisplaysAlone(t *testing.T) {
	var c Command
	in := `{"cmd":"set_displays","displays":[]}`
	if err := json.Unmarshal([]byte(in), &c); err != nil {
		t.Fatal(err)
	}
	if got := resolveDisplays(c); len(got) != 0 {
		t.Fatalf("resolveDisplays = %+v, want empty", got)
	}
}
