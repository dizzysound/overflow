# eld-encoder

A small program that encodes AAC-ELD audio for `overflow-helper`. The helper starts it and
exchanges frames with it over stdin and stdout. It is a separate program, not a library,
because it links the Fraunhofer FDK AAC library, whose license is not GPL-compatible, and
the helper is GPLv3. See [LICENSE-NOTICE.md](LICENSE-NOTICE.md).

Without it, receivers that accept only AAC-ELD screen audio get video with no audio.

## Encoder settings

These match doubletake's in-process encoder (`helper/internal/airplay/aac_eld_fdk.go`)
exactly: AOT 39 (ER AAC-ELD), 44100 Hz, stereo (`MODE_2`, channel order 1), 128 kbit/s,
`TT_MP4_RAW` (raw access units, no ADTS/LATM), SBR off, granule length 480. The encode call
is the same too, including an 8192-byte output buffer. With these settings fdk-aac 2.0.3
returns an access unit (about 174 bytes) for every frame, including the first.

## Protocol

All integers are u32 little-endian.

1. On start the encoder writes the magic `ELD1`, then the frame length in samples per
   channel, which is always `480`. The helper rejects anything else.
2. For each frame the helper writes exactly 480 x 2 channels x 2 bytes = **1920 bytes** of
   interleaved S16LE stereo PCM at 44100 Hz.
3. The encoder replies with a length `n`, then `n` bytes of one raw AAC-ELD access unit.
   `n = 0` means the encoder produced no output for that frame; the helper treats it exactly
   like an empty output from the cgo encoder (the frame is skipped). Output is flushed after
   every frame.
4. EOF on stdin at a frame boundary is a clean shutdown: exit 0. A partial frame or an
   encoder error is reported on stderr and the program exits non-zero.

`eld-encoder --asc` prints the encoder's AudioSpecificConfig in hex (`f8e85000`) and exits.
The test uses it to wrap the access units in a container ffmpeg can decode.

On Windows, stdin and stdout are switched to binary mode.

On the helper side (`helper/internal/airplay/aac_eld_process.go`), the handshake and each
frame have a 2-second deadline. A hung encoder is killed and that session's audio ends.
Close closes stdin, waits up to 1 second for exit, kills the process if needed, and always
waits for it so no zombie remains.

## Building

```sh
./eld-encoder/build.sh native     # macOS/Linux, links the system fdk-aac (pkg-config)
./eld-encoder/build.sh windows    # cross-build helper/bin/windows/eld-encoder.exe
```

The Windows build downloads the fdk-aac 2.0.3 source tag from
<https://github.com/mstorsjo/fdk-aac>, checks its sha256 (pinned in `build.sh`), builds a static
library with CMake and mingw-w64, and links `eld-encoder.exe` with `-static`. It then checks that
the executable is a PE32+ x86-64 console program that imports only Windows system DLLs
(`KERNEL32.dll` and the Universal CRT `api-ms-win-crt-*` API sets, present on Windows 10 and
later). It copies the FDK license to `helper/bin/windows/eld-encoder-FDK-AAC-NOTICE.txt`, next to
the exe. From `helper/`, `make eld-encoder-windows` does the same. Intermediates go in
`eld-encoder/build/`, which is git-ignored. fdk-aac source is never committed.

Ship `eld-encoder.exe` (and the notice) in the same directory as `overflow-helper.exe`, or pass
the helper `-eld-encoder <path>`.

## Testing

```sh
./eld-encoder/build.sh native
python3 eld-encoder/test_native.py           # 2 s 440 Hz tone -> encode -> ffmpeg decode
cd helper && ELD_ENCODER_TEST_BINARY=../eld-encoder/build/native/eld-encoder \
  go test ./internal/airplay/ -run ELD -v
```

`test_native.py` checks the frame count, that the access units are non-empty and not ADTS,
then wraps them in a Matroska file with the `--asc` AudioSpecificConfig, decodes it with
ffmpeg, and checks that the decoded audio is a 440 Hz tone.
