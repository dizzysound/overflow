package airplay

import (
	"errors"
	"reflect"
	"testing"

	"howett.net/plist"
)

func receiverWithFeatures(bits ...uint) ReceiverInfo {
	var features uint64
	for _, bit := range bits {
		features |= uint64(1) << bit
	}
	return ReceiverInfo{Features: features}
}

func mustCompatibility(t *testing.T, info *ReceiverInfo, encrypted bool) receiverCompatibility {
	t.Helper()
	policy, err := compatibilityForReceiver(info, encrypted, true)
	if err != nil {
		t.Fatalf("compatibilityForReceiver: %v", err)
	}
	return policy
}

func TestReceiverCompatibilityUsesIndependentCapabilities(t *testing.T) {
	info := receiverWithFeatures(
		43, // system pairing
		featurePTP,
		featureAudioStreamConnectionSetup,
	)
	info.SourceVersion = "354.54.6"
	info.SupportedFormats = StreamFormats{ScreenStream: FormatMask(screenAudioFormatALAC)}

	got := mustCompatibility(t, &info, true)
	if got.timing != timingProtocolPTP || !got.permitsLocalPTPClock() {
		t.Fatalf("PTP policy = timing %q fallback=%t, want PTP with clock-header fallback", got.timing, got.permitsLocalPTPClock())
	}
	if got.audioSecurity != audioSecurityChaCha || got.audioConnections != audioLayoutStreamConnections {
		t.Fatalf("audio policy = security %d/layout %d, want ChaCha/streamConnections", got.audioSecurity, got.audioConnections)
	}
	if got.audioCodec != AudioCodecALAC {
		t.Fatalf("audio codec = %d, want ALAC", got.audioCodec)
	}
	if got.fairPlayRoots != fairPlayDescriptorOnly {
		t.Fatalf("FairPlay roots = %d, want descriptor-only", got.fairPlayRoots)
	}
	if got.sourceVersion() != modernAirPlaySourceVersion {
		t.Fatalf("sender sourceVersion = %q, want %q", got.sourceVersion(), modernAirPlaySourceVersion)
	}
}

func TestFeature59OnlyControlsInitialAudioDescriptorLayout(t *testing.T) {
	withFeature := receiverWithFeatures(featureAudioStreamConnectionSetup)
	withoutFeature := receiverWithFeatures()

	for _, test := range []struct {
		name       string
		info       *ReceiverInfo
		encrypted  bool
		wantLayout audioConnectionLayout
		wantRoots  fairPlayRootPlacement
	}{
		{
			name: "encrypted feature 59",
			info: &withFeature, encrypted: true,
			wantLayout: audioLayoutStreamConnections, wantRoots: fairPlayDescriptorOnly,
		},
		{
			name:       "plaintext feature 59",
			info:       &withFeature,
			wantLayout: audioLayoutStreamConnections, wantRoots: fairPlayAllRoots,
		},
		{
			name: "encrypted without feature 59",
			info: &withoutFeature, encrypted: true,
			wantLayout: audioLayoutControlPort, wantRoots: fairPlayDescriptorOnly,
		},
		{
			name:       "unknown receiver",
			wantLayout: audioLayoutControlPort, wantRoots: fairPlayAllRoots,
		},
	} {
		t.Run(test.name, func(t *testing.T) {
			got := mustCompatibility(t, test.info, test.encrypted)
			if got.audioConnections != test.wantLayout || got.fairPlayRoots != test.wantRoots {
				t.Fatalf("policy = layout %d/roots %d, want %d/%d", got.audioConnections, got.fairPlayRoots, test.wantLayout, test.wantRoots)
			}
		})
	}
}

func TestPTPRequiresCapabilityVersionAndEncryption(t *testing.T) {
	ptp := receiverWithFeatures(featurePTP)
	ptp.SourceVersion = "354.54.6"
	ptp.hasPTPInfo = false
	ptpInfoOnly := receiverWithFeatures()
	ptpInfoOnly.SourceVersion = "980.71.1"
	ptpInfoOnly.hasPTPInfo = true

	for _, test := range []struct {
		name      string
		info      ReceiverInfo
		encrypted bool
		want      string
	}{
		{name: "all predicates", info: ptp, encrypted: true, want: timingProtocolPTP},
		{name: "plaintext", info: ptp, want: timingProtocolNTP},
		{name: "PTPInfo without feature", info: ptpInfoOnly, encrypted: true, want: timingProtocolNTP},
		{name: "feature below version floor", info: func() ReceiverInfo {
			i := ptp
			i.SourceVersion = "354.54.5"
			return i
		}(), encrypted: true, want: timingProtocolNTP},
		{name: "377.40 interoperability exception", info: func() ReceiverInfo {
			i := ptp
			i.SourceVersion = "377.40.12"
			return i
		}(), encrypted: true, want: timingProtocolNTP},
	} {
		t.Run(test.name, func(t *testing.T) {
			got := mustCompatibility(t, &test.info, test.encrypted)
			if got.timing != test.want {
				t.Fatalf("timing = %q, want %q", got.timing, test.want)
			}
			if got.permitsLocalPTPClock() != (test.want == timingProtocolPTP) {
				t.Fatalf("permitsLocalPTPClock = %t for timing %q", got.permitsLocalPTPClock(), got.timing)
			}
		})
	}
}

func TestSupportsPTPSourceVersion(t *testing.T) {
	for _, test := range []struct {
		version string
		want    bool
	}{
		{version: "354.54.5"},
		{version: "354.54.6", want: true},
		{version: "354.55", want: true},
		{version: "355.0", want: true},
		{version: "377.39.99", want: true},
		{version: "377.40"},
		{version: "377.40.0"},
		{version: "377.40.999"},
		{version: "377.41", want: true},
		{version: "980.71.1", want: true},
		{version: ""},
		{version: "354"},
		{version: "354.54.beta"},
		{version: "354.54.6.1"},
	} {
		t.Run(test.version, func(t *testing.T) {
			if got := supportsPTPSourceVersion(test.version); got != test.want {
				t.Fatalf("supportsPTPSourceVersion(%q) = %t, want %t", test.version, got, test.want)
			}
		})
	}
}

func TestScreenAudioCodecUsesAdvertisedFormatMask(t *testing.T) {
	for _, test := range []struct {
		name     string
		mask     uint64
		probeErr error
		want     AudioCodec
		wantErr  bool
	}{
		{name: "missing mask", want: AudioCodecALAC},
		{name: "ALAC", mask: screenAudioFormatALAC, want: AudioCodecALAC},
		// Apple's screen-mirroring sender picks AAC-ELD when a receiver offers both
		// (Step 0, 2026-09-28: iPhone on iOS 27 chose ct=8 against UxPlay).
		{name: "AAC-ELD preferred when both", mask: screenAudioFormatALAC | screenAudioFormatAACELD44100Stereo, want: AudioCodecAACELD},
		{name: "ALAC when both but no ELD encoder", mask: screenAudioFormatALAC | screenAudioFormatAACELD44100Stereo, probeErr: ErrAACELDUnavailable, want: AudioCodecALAC},
		{name: "AAC ELD 44.1 kHz stereo", mask: screenAudioFormatAACELD44100Stereo, want: AudioCodecAACELD},
		{name: "unsupported advertised mask", mask: 0x800000, wantErr: true},
	} {
		t.Run(test.name, func(t *testing.T) {
			withAACELDEncoderProbe(t, test.probeErr)
			info := &ReceiverInfo{SupportedFormats: StreamFormats{ScreenStream: FormatMask(test.mask)}}
			policy, err := compatibilityForReceiver(info, false, true)
			if (err != nil) != test.wantErr {
				t.Fatalf("compatibility error = %v, want error=%t", err, test.wantErr)
			}
			if err == nil && policy.audioCodec != test.want {
				got := policy.audioCodec
				t.Fatalf("audio codec = %d, want %d", got, test.want)
			}
		})
	}
}

func TestUnsupportedScreenAudioFormatDoesNotBlockVideoOnlySession(t *testing.T) {
	info := &ReceiverInfo{SupportedFormats: StreamFormats{ScreenStream: 0x800000}}
	policy, err := compatibilityForReceiver(info, false, false)
	if err != nil {
		t.Fatalf("video-only compatibility: %v", err)
	}
	if policy.audioCodec != AudioCodecALAC {
		t.Fatalf("inert video-only audio descriptor codec = %d, want ALAC", policy.audioCodec)
	}
}

func TestAudioSecurityUsesNegotiatedEncryption(t *testing.T) {
	legacyFeature := receiverWithFeatures()
	if got := mustCompatibility(t, &legacyFeature, false).audioSecurity; got != audioSecurityLegacyAES {
		t.Fatalf("plaintext audio security = %d, want legacy AES", got)
	}
	if got := mustCompatibility(t, &legacyFeature, true).audioSecurity; got != audioSecurityChaCha {
		t.Fatalf("encrypted audio security = %d, want ChaCha", got)
	}
}

func TestIdentityDoesNotChangeCompatibilityPolicy(t *testing.T) {
	base := receiverWithFeatures(featurePTP, featureAudioStreamConnectionSetup)
	base.SourceVersion = "980.71.1"
	want := mustCompatibility(t, &base, true)

	for _, identity := range []ReceiverInfo{
		{Name: "receiver one", Model: "model one", Manufacturer: "vendor one"},
		{Name: "receiver two", Model: "model two", Manufacturer: "vendor two"},
	} {
		info := base
		info.Name = identity.Name
		info.Model = identity.Model
		info.Manufacturer = identity.Manufacturer
		if got := mustCompatibility(t, &info, true); !reflect.DeepEqual(got, want) {
			t.Fatalf("identity changed compatibility policy: got %+v, want %+v", got, want)
		}
	}
}

func TestHybridAudioDescriptorLayouts(t *testing.T) {
	key := make([]byte, 32)
	legacy := map[string]interface{}{}
	addScreenAudioStreamFields(legacy, key, 6001, audioLayoutControlPort)
	if got := plistInt(legacy["controlPort"]); got != 6001 {
		t.Fatalf("legacy controlPort = %d, want 6001", got)
	}
	if got, _ := legacy["shk"].([]byte); len(got) != len(key) {
		t.Fatalf("legacy shk length = %d, want %d", len(got), len(key))
	}
	if _, ok := legacy["streamConnections"]; ok {
		t.Fatal("legacy audio unexpectedly contains streamConnections")
	}

	modern := map[string]interface{}{"controlPort": int64(1)}
	addScreenAudioStreamFields(modern, key, 6002, audioLayoutStreamConnections)
	if _, ok := modern["controlPort"]; ok {
		t.Fatal("modern audio retained controlPort")
	}
	connections, _ := modern["streamConnections"].(map[string]interface{})
	if len(connections) == 0 {
		t.Fatal("modern audio omitted streamConnections")
	}

	plaintext := map[string]interface{}{}
	addScreenAudioStreamFields(plaintext, nil, 6003, audioLayoutStreamConnections)
	if _, ok := plaintext["shk"]; ok {
		t.Fatal("plaintext streamConnections descriptor unexpectedly contains shk")
	}
	plaintextConnections, _ := plaintext["streamConnections"].(map[string]interface{})
	rtp, _ := plaintextConnections["streamConnectionTypeRTP"].(map[string]interface{})
	if encrypted, ok := rtp["streamConnectionKeyUseStreamEncryptionKey"].(bool); !ok || encrypted {
		t.Fatalf("plaintext streamConnections encryption flag = %#v, want false", rtp["streamConnectionKeyUseStreamEncryptionKey"])
	}
}

// legacyAudioInfo decodes a /info plist shaped like the Newline Cast panel's
// (AirTunes/220.68): no supportedFormats, only the older audioFormats array.
func legacyAudioInfo(t *testing.T, entries ...map[string]any) *ReceiverInfo {
	t.Helper()
	raw := map[string]any{"name": "Legacy Panel", "sourceVersion": "280.33"}
	if len(entries) > 0 {
		list := make([]any, 0, len(entries))
		for _, entry := range entries {
			list = append(list, entry)
		}
		raw["audioFormats"] = list
	}
	body, err := plist.Marshal(raw, plist.BinaryFormat)
	if err != nil {
		t.Fatal(err)
	}
	var info ReceiverInfo
	if _, err := plist.Unmarshal(body, &info); err != nil {
		t.Fatalf("decode legacy /info: %v", err)
	}
	return &info
}

func legacyAudioEntry(streamType int64, mask uint64) map[string]any {
	return map[string]any{
		"type":               streamType,
		"audioInputFormats":  mask,
		"audioOutputFormats": mask,
	}
}

func TestScreenAudioCodecUsesLegacyAudioFormatsWhenSupportedFormatsAbsent(t *testing.T) {
	for _, test := range []struct {
		name    string
		entries []map[string]any
		want    AudioCodec
		wantErr bool
	}{
		{name: "AAC-ELD only (Newline Cast panel)", entries: []map[string]any{legacyAudioEntry(96, 0x1000000)}, want: AudioCodecAACELD},
		{name: "ALAC only", entries: []map[string]any{legacyAudioEntry(96, 0x40000)}, want: AudioCodecALAC},
		{name: "ALAC preferred over AAC-ELD", entries: []map[string]any{legacyAudioEntry(96, 0x1040000)}, want: AudioCodecALAC},
		{name: "no audio information keeps ALAC fallback", want: AudioCodecALAC},
		{name: "non-screen entry ignored", entries: []map[string]any{legacyAudioEntry(100, 0x1000000)}, want: AudioCodecALAC},
		{name: "screen entry chosen among several", entries: []map[string]any{legacyAudioEntry(100, 0x40000), legacyAudioEntry(96, 0x1000000)}, want: AudioCodecAACELD},
		{name: "unsupported legacy mask", entries: []map[string]any{legacyAudioEntry(96, 0x800000)}, wantErr: true},
	} {
		t.Run(test.name, func(t *testing.T) {
			info := legacyAudioInfo(t, test.entries...)
			policy, err := compatibilityForReceiver(info, false, true)
			if (err != nil) != test.wantErr {
				t.Fatalf("compatibility error = %v, want error=%t", err, test.wantErr)
			}
			if err == nil && policy.audioCodec != test.want {
				t.Fatalf("audio codec = %d, want %d", policy.audioCodec, test.want)
			}
		})
	}
}

func TestSupportedFormatsScreenStreamTakesPrecedenceOverLegacyAudioFormats(t *testing.T) {
	info := legacyAudioInfo(t, legacyAudioEntry(96, 0x1000000))
	info.SupportedFormats.ScreenStream = FormatMask(screenAudioFormatALAC)
	if got := mustCompatibility(t, info, false).audioCodec; got != AudioCodecALAC {
		t.Fatalf("audio codec = %d, want ALAC from supportedFormats.screenStream", got)
	}
}

func TestScreenAudioMaskSourceNamesWhereTheMaskCameFrom(t *testing.T) {
	both := screenAudioFormatALAC | screenAudioFormatAACELD44100Stereo
	mask, source := screenAudioMask(&ReceiverInfo{SupportedFormats: StreamFormats{ScreenStream: FormatMask(both)}})
	if mask != both || source != "supportedFormats.screenStream" {
		t.Fatalf("mask=0x%x source=%q", mask, source)
	}
	if mask, source := screenAudioMask(nil); mask != 0 || source != "no /info" {
		t.Fatalf("nil info: mask=0x%x source=%q", mask, source)
	}
}

func TestReselectScreenAudioUsesSessionInfoWhenPrePairInfoHadNoMask(t *testing.T) {
	withAACELDEncoderProbe(t, nil)
	both := FormatMask(screenAudioFormatALAC | screenAudioFormatAACELD44100Stereo)
	session := &ReceiverInfo{SupportedFormats: StreamFormats{ScreenStream: both}}
	// The Hisense Roku's pre-pair /info omits supportedFormats (mask 0 -> ALAC);
	// its authenticated session /info offers both, so AAC-ELD wins.
	if got, changed := reselectScreenAudio(0, AudioCodecALAC, session); !changed || got != AudioCodecAACELD {
		t.Fatalf("got %d changed=%t, want AAC-ELD", got, changed)
	}
	// A choice made from a real pre-pair mask is never revisited.
	if got, changed := reselectScreenAudio(uint64(both), AudioCodecALAC, session); changed || got != AudioCodecALAC {
		t.Fatalf("got %d changed=%t, want unchanged ALAC", got, changed)
	}
	// No formats in the session info either: keep the original choice.
	if got, changed := reselectScreenAudio(0, AudioCodecALAC, &ReceiverInfo{}); changed || got != AudioCodecALAC {
		t.Fatalf("got %d changed=%t, want unchanged ALAC", got, changed)
	}
}

func TestReselectScreenAudioKeepsALACWhenELDEncoderUnavailable(t *testing.T) {
	withAACELDEncoderProbe(t, ErrAACELDUnavailable)
	eldOnly := &ReceiverInfo{SupportedFormats: StreamFormats{ScreenStream: FormatMask(screenAudioFormatAACELD44100Stereo)}}
	if got, changed := reselectScreenAudio(0, AudioCodecALAC, eldOnly); changed || got != AudioCodecALAC {
		t.Fatalf("got %d changed=%t, want unchanged ALAC (cannot encode ELD)", got, changed)
	}
}

func TestParseScreenAudioFormat(t *testing.T) {
	for _, tc := range []struct {
		in   string
		want AudioCodec
		ok   bool
	}{
		{"", 0, true},
		{"auto", 0, true},
		{"alac", AudioCodecALAC, true},
		{"aac-eld", AudioCodecAACELD, true},
		{"opus", 0, false},
		{"ALAC", 0, false},
	} {
		got, err := ParseScreenAudioFormat(tc.in)
		if got != tc.want || (err == nil) != tc.ok {
			t.Errorf("ParseScreenAudioFormat(%q) = %d, %v; want %d, ok=%v", tc.in, got, err, tc.want, tc.ok)
		}
	}
}

func TestForcedScreenAudio(t *testing.T) {
	withAACELDEncoderProbe(t, nil)
	if got, forced := forcedScreenAudio(AudioCodecALAC, AudioCodecAACELD); got != AudioCodecAACELD || !forced {
		t.Fatalf("force ELD over ALAC = %d, %v; want ELD, forced", got, forced)
	}
	if got, forced := forcedScreenAudio(AudioCodecAACELD, AudioCodecALAC); got != AudioCodecALAC || !forced {
		t.Fatalf("force ALAC over ELD = %d, %v; want ALAC, forced", got, forced)
	}
	if got, forced := forcedScreenAudio(AudioCodecALAC, AudioCodecALAC); got != AudioCodecALAC || !forced {
		t.Fatalf("force the automatic codec = %d, %v; want ALAC, forced (no session re-pick)", got, forced)
	}
	if got, forced := forcedScreenAudio(AudioCodecALAC, 0); got != AudioCodecALAC || forced {
		t.Fatalf("no override = %d, %v; want ALAC, not forced", got, forced)
	}
}

func TestForcedAACELDWithoutEncoderKeepsAutomaticChoice(t *testing.T) {
	withAACELDEncoderProbe(t, errors.New("no encoder"))
	if got, forced := forcedScreenAudio(AudioCodecALAC, AudioCodecAACELD); got != AudioCodecALAC || forced {
		t.Fatalf("force ELD without encoder = %d, %v; want ALAC, not forced", got, forced)
	}
}
