/*
 * eld-encoder: a stdin/stdout AAC-ELD encoder for overflow-helper.
 *
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Christopher Gillespie
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * The built binary links the Fraunhofer FDK AAC library, which is under its
 * own license; see LICENSE-NOTICE.md. This program is separate from the GPL
 * overflow-helper, which only talks to it over pipes.
 *
 * Protocol (all integers are u32 little-endian):
 *
 *   On start the encoder writes the 4-byte magic "ELD1" followed by the frame
 *   length in samples per channel (always 480).
 *
 *   Then, for each frame: the helper writes exactly 480 * 2 channels * 2 bytes
 *   = 1920 bytes of interleaved S16LE stereo PCM at 44100 Hz. The encoder
 *   replies with a u32 length followed by that many bytes of one raw AAC-ELD
 *   access unit (no ADTS or LATM framing). A zero length means the encoder
 *   produced no output for this frame, exactly as when the in-process cgo
 *   encoder returns zero bytes; the helper skips such frames.
 *
 *   EOF on stdin at a frame boundary is a clean shutdown (exit 0). EOF inside
 *   a frame, or any encoder error, is reported on stderr with a non-zero exit.
 *
 *   "eld-encoder --asc" prints the encoder's AudioSpecificConfig as hex on
 *   stdout and exits, for tests that wrap the access units in a container.
 *
 * The encoder parameters and the encode call mirror doubletake's cgo path in
 * helper/internal/airplay/aac_eld_fdk.go exactly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include <fdk-aac/aacenc_lib.h>

#define ELD_FRAME_LENGTH 480
#define ELD_CHANNELS 2
#define ELD_PCM_BYTES (ELD_FRAME_LENGTH * ELD_CHANNELS * 2)
/* doubletake's callers hand the cgo encoder an 8192-byte output buffer. */
#define ELD_OUT_BYTES 8192

static AACENC_ERROR eld_open(HANDLE_AACENCODER *enc) {
	AACENC_ERROR err;
	if ((err = aacEncOpen(enc, 0, 2)) != AACENC_OK) return err;
	if ((err = aacEncoder_SetParam(*enc, AACENC_AOT, 39)) != AACENC_OK) return err;
	if ((err = aacEncoder_SetParam(*enc, AACENC_SAMPLERATE, 44100)) != AACENC_OK) return err;
	if ((err = aacEncoder_SetParam(*enc, AACENC_CHANNELMODE, MODE_2)) != AACENC_OK) return err;
	if ((err = aacEncoder_SetParam(*enc, AACENC_CHANNELORDER, 1)) != AACENC_OK) return err;
	if ((err = aacEncoder_SetParam(*enc, AACENC_BITRATE, 128000)) != AACENC_OK) return err;
	if ((err = aacEncoder_SetParam(*enc, AACENC_TRANSMUX, TT_MP4_RAW)) != AACENC_OK) return err;
	if ((err = aacEncoder_SetParam(*enc, AACENC_SBR_MODE, 0)) != AACENC_OK) return err;
	if ((err = aacEncoder_SetParam(*enc, AACENC_GRANULE_LENGTH, 480)) != AACENC_OK) return err;
	return aacEncEncode(*enc, NULL, NULL, NULL, NULL);
}

static AACENC_ERROR eld_encode(HANDLE_AACENCODER enc, INT_PCM *pcm, INT nSamples,
                               UCHAR *out, INT outSize, INT *nOut) {
	AACENC_BufDesc inDesc = {0}, outDesc = {0};
	AACENC_InArgs inArgs = {0};
	AACENC_OutArgs outArgs = {0};
	void *inBufs[1] = {pcm}, *outBufs[1] = {out};
	INT inIds[1] = {IN_AUDIO_DATA}, outIds[1] = {OUT_BITSTREAM_DATA};
	INT inSizes[1] = {nSamples * (INT)sizeof(INT_PCM)}, outSizes[1] = {outSize};
	INT inElSizes[1] = {(INT)sizeof(INT_PCM)}, outElSizes[1] = {1};
	AACENC_ERROR err;
	inDesc.numBufs = outDesc.numBufs = 1;
	inDesc.bufs = inBufs;
	inDesc.bufferIdentifiers = inIds;
	inDesc.bufSizes = inSizes;
	inDesc.bufElSizes = inElSizes;
	outDesc.bufs = outBufs;
	outDesc.bufferIdentifiers = outIds;
	outDesc.bufSizes = outSizes;
	outDesc.bufElSizes = outElSizes;
	inArgs.numInSamples = nSamples;
	err = aacEncEncode(enc, &inDesc, &outDesc, &inArgs, &outArgs);
	*nOut = outArgs.numOutBytes;
	return err;
}

static void put_u32le(unsigned char *p, unsigned int v) {
	p[0] = (unsigned char)(v & 0xff);
	p[1] = (unsigned char)((v >> 8) & 0xff);
	p[2] = (unsigned char)((v >> 16) & 0xff);
	p[3] = (unsigned char)((v >> 24) & 0xff);
}

static int write_all(const void *buf, size_t n) {
	return n == 0 || fwrite(buf, 1, n, stdout) == n;
}

int main(int argc, char **argv) {
	HANDLE_AACENCODER enc = NULL;
	AACENC_InfoStruct info;
	AACENC_ERROR err;
	/* INT_PCM is 16-bit in fdk-aac 2.x; keep the buffer aligned for it. */
	static INT_PCM pcm[ELD_FRAME_LENGTH * ELD_CHANNELS];
	static UCHAR out[ELD_OUT_BYTES];
	unsigned char header[8];
	int print_asc = argc > 1 && strcmp(argv[1], "--asc") == 0;

	if (argc > 1 && !print_asc) {
		fprintf(stderr, "usage: eld-encoder [--asc]\n");
		return 2;
	}
	if (sizeof(INT_PCM) != 2) {
		fprintf(stderr, "eld-encoder: INT_PCM is %u bytes, want 2\n", (unsigned)sizeof(INT_PCM));
		return 1;
	}

#ifdef _WIN32
	if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) {
		fprintf(stderr, "eld-encoder: cannot set binary mode on stdio\n");
		return 1;
	}
#endif

	if ((err = eld_open(&enc)) != AACENC_OK) {
		fprintf(stderr, "eld-encoder: open AAC-ELD encoder: FDK error %d\n", (int)err);
		if (enc) aacEncClose(&enc);
		return 1;
	}
	memset(&info, 0, sizeof(info));
	if ((err = aacEncInfo(enc, &info)) != AACENC_OK) {
		fprintf(stderr, "eld-encoder: read AAC-ELD encoder info: FDK error %d\n", (int)err);
		aacEncClose(&enc);
		return 1;
	}
	if (info.frameLength != ELD_FRAME_LENGTH) {
		fprintf(stderr, "eld-encoder: encoder selected %u samples per frame, want %d\n",
		        (unsigned)info.frameLength, ELD_FRAME_LENGTH);
		aacEncClose(&enc);
		return 1;
	}

	if (print_asc) {
		UINT i;
		for (i = 0; i < info.confSize; i++) printf("%02x", info.confBuf[i]);
		printf("\n");
		aacEncClose(&enc);
		return 0;
	}

	memcpy(header, "ELD1", 4);
	put_u32le(header + 4, info.frameLength);
	if (!write_all(header, 8) || fflush(stdout) != 0) {
		fprintf(stderr, "eld-encoder: write handshake failed\n");
		aacEncClose(&enc);
		return 1;
	}

	for (;;) {
		size_t got = fread(pcm, 1, ELD_PCM_BYTES, stdin);
		INT encoded = 0;
		if (got == 0 && feof(stdin)) break;
		if (got != ELD_PCM_BYTES) {
			fprintf(stderr, "eld-encoder: short PCM frame: %u of %d bytes%s\n", (unsigned)got,
			        ELD_PCM_BYTES, ferror(stdin) ? " (read error)" : "");
			aacEncClose(&enc);
			return 1;
		}
		err = eld_encode(enc, pcm, ELD_FRAME_LENGTH * ELD_CHANNELS, out, ELD_OUT_BYTES, &encoded);
		if (err != AACENC_OK) {
			fprintf(stderr, "eld-encoder: encode AAC-ELD: FDK error %d\n", (int)err);
			aacEncClose(&enc);
			return 1;
		}
		if (encoded < 0 || encoded > ELD_OUT_BYTES) {
			fprintf(stderr, "eld-encoder: encoder returned invalid size %d\n", (int)encoded);
			aacEncClose(&enc);
			return 1;
		}
		put_u32le(header, (unsigned int)encoded);
		if (!write_all(header, 4) || !write_all(out, (size_t)encoded) || fflush(stdout) != 0) {
			fprintf(stderr, "eld-encoder: write access unit failed\n");
			aacEncClose(&enc);
			return 1;
		}
	}

	aacEncClose(&enc);
	return 0;
}
