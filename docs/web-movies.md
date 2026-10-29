# Web movie playback

The Web runtime first tries the browser's HTML video element. On media/decode
failure (or a 15-second native startup timeout), it lazily loads a separate,
single-threaded FFmpeg decoder Worker. An autoplay permission rejection waits for
the next pointer/key gesture instead of unnecessarily loading a decoder. The
movie event continues only after playback finishes or an error is reported.

## Supported fallback formats

| Component | Included |
| --- | --- |
| Containers | AVI, MPEG program/transport streams, MOV/MP4 |
| Video | MPEG-1/2, MPEG-4 Part 2 (DivX/Xvid), MSMPEG4 v1/v2/v3, Microsoft Video 1, Cinepak, Indeo 3, MJPEG, H.264, raw video |
| Audio | MP1/2/3, AAC, AC-3, unsigned 8-bit/signed 16/24/32-bit little-endian PCM, Microsoft/IMA WAV ADPCM |

WebM/Ogg and other codecs supported by the browser use the native path. This is
an explicit codec subset, not every format supported by a full FFmpeg install.
An unsupported audio track fails with a diagnostic rather than silently playing
a mute video. Original AVI/MPG files and event references can remain unchanged.

## Runtime and resource use

The engine stays in its existing Worker. The fallback Worker reads movie Blob
slices using WORKERFS, converts each video frame to RGBA, and sends stereo float
audio with presentation timestamps. The page paints a separate canvas and
schedules audio using the existing AudioContext clock. Context suspension freezes
that clock; the next user gesture can resume it. Video frames that fall behind
the audio clock are dropped. Playback speed remains real time.

Decode requests are backpressured to about half a second ahead and 32 MiB of
queued video (plus at most one frame). The WASM heap is capped at 256 MiB, with a
4096 × 2160 decoded pixel budget. Large/high-rate movies may exceed these limits
or the device's software decoding capacity. The decoder is terminated on stop,
end, error, or game shutdown; it does not retain whole decoded movies. Each movie
has a session ID, so stale completions cannot change a subsequent movie's state.
There are 60-second module startup and 30-second per-decode-request timeouts.

## Build and deployment

The Emscripten CMake target builds the decoder automatically using
`builds/emscripten/build-movie-decoder.sh` and
`src/platform/emscripten/movie_decoder.c`. Linux/Docker builds require Bash,
curl, tar, make, sha256sum and the activated Emscripten SDK. The repository's
packaging image already supplies these. Native Windows CMake users should use
the documented Docker Web packaging command.

The build pins [FFmpeg 7.1.5](https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz),
SHA-256 `de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f`.
The download is verified before extraction. Builds are optimized for size with
LTO; encoders, muxers, filters, networking, hardware acceleration and threads are
omitted. Cached FFmpeg objects are rebuilt when the build script or SDK changes.
The source archive and objects remain under the build's `movie-decoder-build/`.

Deploy the entire Web ZIP at one immutable runtime URL, including:

- `player-host.js`, `player-worker.js`, `player-audio.js`
- `player-audio-worker.js`, `player-files.js`, `web-audio.md`
- `easyrpg-player.js`, `easyrpg-player.wasm`, `easyrpg-player.data`
- `easyrpg-player-asyncify.js`, `easyrpg-player-asyncify.wasm`, `easyrpg-player-asyncify.data`
- `player-movie.js`, `player-movie-worker.js`
- `movie-decoder.js`, `movie-decoder.wasm`, `movie-decoder.LICENSE.txt`
- `COPYING` and this document

`player-movie.js` is an ES module; serve JS with a JavaScript MIME type and WASM
as `application/wasm`. The site must copy/serve the new files when importing the
runtime, rather than restricting imports to the previous six-file list. Existing
game packages do not need to be repacked. No CDN requests are made at runtime.
The hosting policy must allow same-origin Workers/scripts, WASM compilation,
runtime fetches, and `blob:` media for the native path. SharedArrayBuffer and
COOP/COEP remain unnecessary. Actual policy values belong to the hosting site.

The FFmpeg subset uses LGPL-2.1-or-later components (license bundled with the ZIP).
The bridge and Player remain GPL-3.0-or-later. The pinned upstream source plus
the build script and bridge at the matching Player revision provide the inputs
to rebuild/relink the decoder; preserve the corresponding source availability
when redistributing binaries.

## Verification boundary

Compiler, JS/shell syntax, and packaging checks do not validate browser media
policy, sound-device behavior, visual placement, or real-time audio/video sync.
Those require an authorized browser playback check. No game assets are included
in this repository.
