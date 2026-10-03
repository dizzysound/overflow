package bridge

import "testing"

func TestParseDevice(t *testing.T) {
	d, err := ParseDevice("0c:fb:30:58:df:2e@10.20.0.178:7000")
	if err != nil || d.DeviceID != "0C:FB:30:58:DF:2E" || d.IP != "10.20.0.178" || d.Port != 7000 {
		t.Fatalf("got %+v, %v", d, err)
	}
	d, err = ParseDevice("AA:BB:CC:DD:EE:FF@192.168.1.58")
	if err != nil || d.Port != 7000 || d.IP != "192.168.1.58" {
		t.Fatalf("default port: got %+v, %v", d, err)
	}
	for _, bad := range []string{"", "nodevice", "@10.0.0.1", "AA@", "AA@not-an-ip", "AA@10.0.0.1:99999"} {
		if _, err := ParseDevice(bad); err == nil {
			t.Fatalf("%q accepted", bad)
		}
	}
}
