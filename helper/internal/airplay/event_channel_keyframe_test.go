package airplay

import (
	"testing"
	"time"

	"howett.net/plist"
)

func TestForceKeyFrameCommandCallsBack(t *testing.T) {
	body, err := plist.Marshal(map[string]interface{}{"type": "forceKeyFrame"}, plist.BinaryFormat)
	if err != nil {
		t.Fatal(err)
	}
	called := false
	req := eventRequest{method: "POST", path: "/command", contentType: "application/x-apple-binary-plist", body: body}
	if err := handleEventRequest(req, &mediaClock{}, time.Now(), func() { called = true }); err != nil {
		t.Fatalf("handleEventRequest: %v", err)
	}
	if !called {
		t.Fatal("forceKeyFrame did not call back")
	}
}
