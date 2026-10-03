package bridge

import "testing"

func TestParseVolume(t *testing.T) {
	if set, _, err := ParseVolume("keep"); err != nil || set {
		t.Fatalf("keep: set=%v err=%v", set, err)
	}
	if set, db, err := ParseVolume("-12.5"); err != nil || !set || db != -12.5 {
		t.Fatalf("-12.5: set=%v db=%v err=%v", set, db, err)
	}
	for _, bad := range []string{"loud", "1", "-31", "-144"} {
		if _, _, err := ParseVolume(bad); err == nil {
			t.Fatalf("%q accepted", bad)
		}
	}
}
