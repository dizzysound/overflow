#!/usr/bin/env python3
"""End-to-end check of a native eld-encoder build.

Usage: python3 eld-encoder/test_native.py [path/to/eld-encoder]

1. Generates 2 s of a 440 Hz stereo tone with ffmpeg (S16LE, 44100 Hz).
2. Pipes it through eld-encoder and parses the ELD1 protocol.
3. Checks the frame count and that most access units are non-empty.
4. Wraps the access units in a minimal Matroska file (CodecID A_AAC, with the
   encoder's AudioSpecificConfig from --asc as CodecPrivate), decodes it with
   the ffmpeg CLI, and checks that the decoded audio is dominated by 440 Hz.

Needs only python3 and ffmpeg. Exits non-zero on any failure.
"""
import math
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ENCODER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "build", "native", "eld-encoder")
RATE, CHANNELS, FRAME = 44100, 2, 480
SECONDS = 2


def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def tone_pcm():
    return subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
         "-i", f"sine=frequency=440:sample_rate={RATE}:duration={SECONDS}",
         "-ac", str(CHANNELS), "-f", "s16le", "-ar", str(RATE), "-"],
        check=True, capture_output=True).stdout


def encode(pcm):
    frame_bytes = FRAME * CHANNELS * 2
    whole = len(pcm) // frame_bytes
    pcm = pcm[: whole * frame_bytes]
    proc = subprocess.run([ENCODER], input=pcm, capture_output=True)
    if proc.returncode != 0:
        fail(f"encoder exit {proc.returncode}: {proc.stderr.decode(errors='replace')}")
    out = proc.stdout
    if out[:4] != b"ELD1":
        fail(f"bad magic {out[:4]!r}")
    (frame_len,) = struct.unpack("<I", out[4:8])
    if frame_len != FRAME:
        fail(f"frame length {frame_len}, want {FRAME}")
    aus, pos = [], 8
    while pos < len(out):
        (n,) = struct.unpack("<I", out[pos:pos + 4])
        pos += 4
        aus.append(out[pos:pos + n])
        pos += n
    if pos != len(out):
        fail("trailing bytes after last access unit")
    return whole, aus


# --- minimal Matroska writer ------------------------------------------------

def ebml_id(i):
    return i.to_bytes((i.bit_length() + 7) // 8, "big")


def ebml_size(n):
    for length in range(1, 9):
        if n < (1 << (7 * length)) - 1:
            return ((1 << (7 * length)) | n).to_bytes(length, "big")
    raise ValueError(n)


def el(i, payload):
    return ebml_id(i) + ebml_size(len(payload)) + payload


def uint(i, v):
    return el(i, v.to_bytes(max(1, (v.bit_length() + 7) // 8), "big"))


def mkv(asc, aus):
    header = el(0x1A45DFA3, uint(0x4286, 1) + uint(0x42F7, 1) + uint(0x42F2, 4)
                + uint(0x42F3, 8) + el(0x4282, b"matroska") + uint(0x4287, 4) + uint(0x4285, 2))
    info = el(0x1549A966, uint(0x2AD7B1, 1000000) + el(0x4D80, b"eld-test") + el(0x5741, b"eld-test"))
    audio = el(0xE1, el(0xB5, struct.pack(">d", float(RATE))) + uint(0x9F, CHANNELS))
    track = el(0xAE, uint(0xD7, 1) + uint(0x73C5, 1) + uint(0x83, 2) + el(0x86, b"A_AAC")
               + el(0x63A2, asc) + audio)
    tracks = el(0x1654AE6B, track)
    blocks = b""
    for idx, au in enumerate(aus):
        if not au:
            continue
        ms = idx * FRAME * 1000 // RATE
        blocks += el(0xA3, b"\x81" + struct.pack(">h", ms) + b"\x80" + au)
    cluster = el(0x1F43B675, uint(0xE7, 0) + blocks)
    return header + el(0x18538067, info + tracks + cluster)


def goertzel_power(samples, freq):
    k = 2 * math.cos(2 * math.pi * freq / RATE)
    s1 = s2 = 0.0
    for x in samples:
        s1, s2 = x + k * s1 - s2, s1
    return s1 * s1 + s2 * s2 - k * s1 * s2


def main():
    pcm = tone_pcm()
    frames, aus = encode(pcm)
    nonempty = sum(1 for au in aus if au)
    sizes = [len(au) for au in aus if au]
    print(f"input frames: {frames}")
    print(f"access units: {len(aus)} ({nonempty} non-empty, {len(aus) - nonempty} empty)")
    if sizes:
        print(f"AU bytes: min {min(sizes)}, max {max(sizes)}, mean {sum(sizes) / len(sizes):.1f}")
    if len(aus) != frames:
        fail(f"got {len(aus)} access units for {frames} frames")
    if nonempty < frames * 0.9:
        fail("fewer than 90% of access units are non-empty")
    if any(au[:2] in (b"\xff\xf1", b"\xff\xf9") for au in aus if au):
        fail("an access unit carries an ADTS header")

    asc_hex = subprocess.run([ENCODER, "--asc"], check=True, capture_output=True, text=True).stdout.strip()
    asc = bytes.fromhex(asc_hex)
    print(f"AudioSpecificConfig: {asc_hex}")

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "eld.mkv")
        with open(path, "wb") as f:
            f.write(mkv(asc, aus))
        probe = subprocess.run(
            ["ffprobe", "-hide_banner", "-loglevel", "error", "-show_entries",
             "stream=codec_name,profile,sample_rate,channels", "-of", "compact", path],
            capture_output=True, text=True)
        print("ffprobe:", probe.stdout.strip() or probe.stderr.strip())
        dec = subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", path,
             "-f", "s16le", "-ac", "1", "-ar", str(RATE), "-"],
            capture_output=True)
        if dec.returncode != 0:
            fail("ffmpeg decode failed: " + dec.stderr.decode(errors="replace"))
        if dec.stderr:
            print("ffmpeg stderr:", dec.stderr.decode(errors="replace").strip())

    samples = struct.unpack(f"<{len(dec.stdout) // 2}h", dec.stdout)
    print(f"decoded samples: {len(samples)} ({len(samples) / RATE:.3f} s)")
    if len(samples) < RATE * SECONDS * 0.9:
        fail("decoded audio is too short")
    mid = samples[len(samples) // 4: len(samples) // 4 + RATE // 2]
    rms = math.sqrt(sum(x * x for x in mid) / len(mid))
    p440 = goertzel_power(mid, 440)
    others = {f: goertzel_power(mid, f) for f in (220, 330, 550, 880, 1000)}
    ratio = p440 / max(others.values())
    print(f"decoded RMS: {rms:.1f}; 440 Hz power / strongest other probe: {ratio:.1f}x")
    if rms < 1000:
        fail("decoded audio is nearly silent")
    if ratio < 100:
        fail("decoded audio is not dominated by 440 Hz")
    print("PASS")


if __name__ == "__main__":
    main()
