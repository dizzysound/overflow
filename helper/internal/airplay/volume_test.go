package airplay

import (
	"bufio"
	"io"
	"math"
	"net"
	"strconv"
	"strings"
	"testing"
)

func TestCheckVolumeDB(t *testing.T) {
	for _, db := range []float64{0, -12.5, -30} {
		if err := CheckVolumeDB(db); err != nil {
			t.Errorf("CheckVolumeDB(%v) = %v, want nil", db, err)
		}
	}
	for _, db := range []float64{0.5, -30.1, -144, math.NaN(), math.Inf(-1), math.Inf(1)} {
		err := CheckVolumeDB(db)
		if err == nil {
			t.Errorf("CheckVolumeDB(%v) = nil, want an error", db)
			continue
		}
		if !strings.Contains(err.Error(), "-30 to 0") {
			t.Errorf("CheckVolumeDB(%v) error %q does not name the range", db, err)
		}
	}
}

// readTestRTSPRequest reads one RTSP request (headers and body) from conn.
func readTestRTSPRequest(conn net.Conn) (string, error) {
	r := bufio.NewReader(conn)
	var head strings.Builder
	contentLength := 0
	for {
		line, err := r.ReadString('\n')
		if err != nil {
			return head.String(), err
		}
		head.WriteString(line)
		if line == "\r\n" {
			break
		}
		if name, value, ok := strings.Cut(line, ":"); ok && strings.EqualFold(strings.TrimSpace(name), "Content-Length") {
			contentLength, _ = strconv.Atoi(strings.TrimSpace(value))
		}
	}
	body := make([]byte, contentLength)
	if _, err := io.ReadFull(r, body); err != nil {
		return head.String(), err
	}
	return head.String() + string(body), nil
}

func TestMirrorSessionSetVolumeSendsSetParameter(t *testing.T) {
	clientConn, serverConn := net.Pipe()
	defer clientConn.Close()
	defer serverConn.Close()

	requests := make(chan string, 1)
	go func() {
		req, err := readTestRTSPRequest(serverConn)
		if err != nil {
			requests <- "read error: " + err.Error()
			return
		}
		requests <- req
		serverConn.Write([]byte("RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n"))
	}()

	s := &MirrorSession{client: &AirPlayClient{conn: clientConn}, sessionURI: "rtsp://127.0.0.1:7000/15"}
	if err := s.SetVolume(-12.5); err != nil {
		t.Fatalf("SetVolume: %v", err)
	}
	req := <-requests
	for _, want := range []string{
		"SET_PARAMETER rtsp://127.0.0.1:7000/15 RTSP/1.0\r\n",
		"Content-Type: text/parameters\r\n",
		"\r\n\r\nvolume: -12.500000\r\n",
	} {
		if !strings.Contains(req, want) {
			t.Fatalf("request %q lacks %q", req, want)
		}
	}
}

func TestMirrorSessionSetVolumeRejectsOutOfRange(t *testing.T) {
	// No connection: validation must fail before any I/O.
	s := &MirrorSession{client: &AirPlayClient{}, sessionURI: "rtsp://127.0.0.1:7000/15"}
	if err := s.SetVolume(3); err == nil {
		t.Fatal("SetVolume(3) = nil, want a range error")
	}
}

func TestMirrorSessionSetVolumeWithoutSession(t *testing.T) {
	var s *MirrorSession
	if err := s.SetVolume(-10); err == nil {
		t.Fatal("nil session SetVolume = nil, want an error")
	}
}
