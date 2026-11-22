# Archive-site Web audio and resource reads

The site owns two dedicated Workers. `player-worker.js` runs the game and
OffscreenCanvas display. `player-audio-worker.js` runs the existing C++
GenericAudio mixer and its WAV/OGG/MP3/Opus/module/MIDI decoders in a second
instance of the same WASM module, without starting a game loop or mounting
IDBFS. The bundled SoundFont is initialized before the game starts.

Game commands and playback status use a direct MessagePort between Workers.
The proxy sends resolved filenames, volume, pitch, fades, pause/resume and audio
configuration. A generation number prevents old BGM status from overwriting a
new track's status. MIDI ticks and loop status are asynchronous snapshots,
normally updated once per 1024 produced frames. Custom SoundFonts are copied
from the game instance on selection; only the game instance persists settings
and saves.

The mixer supplies stereo signed-16 PCM directly to AudioWorklet through a
second MessagePort. Neither game rendering nor the page relays PCM requests.
At most three 1024-frame blocks are queued or in flight (64 ms at 48 kHz).
With the optional host audioProfile 'buffered' or 'reduced', the queue holds
up to ten blocks and resumes with five. 'reduced' also requests 22050 Hz and
falls back to the output device rate if unavailable. The default profile
resumes with two available blocks; a 64-sample ramp softens rare
underruns. This is not an unlimited buffer and does not hide arbitrarily slow
decoders or devices. Worker isolation prevents *game/page* stalls from starving
the audio producer; a very expensive audio operation can still exceed its
deadline and must be measured on target hardware.

Both engine instances mount the same immutable OPFS File slices through
WORKERFS. `player-files.js` adds a 32 MiB LRU byte cache per instance. Files up
to 16 MiB are read once in full; larger files use 512 KiB blocks. Repeated small
decoder reads then copy from memory. Eviction only drops cached bytes, not an
open stream or its seek position. The native decoded bitmap and SE caches
remain in use. The full game is never predecoded or copied into the WASM heap.

This costs an additional WASM instance and a bounded audio file cache, but
does not require pthreads, SharedArrayBuffer, or COOP/COEP. The engine module
and data are the same immutable URLs, allowing browser HTTP cache reuse.

Ship `player-audio-worker.js` and `player-files.js` with every runtime ZIP.
Game installation keys, pack layout and Work-based IDBFS saves are unchanged.
`stop()` waits for the game to flush saves, then terminates both Workers;
failed persistence leaves the session alive for retry.

Run `node --test tests/web_runtime.mjs` for file-cache boundaries and audio
underrun recovery. The website's `scripts/easyrpg-worker-check.mjs` verifies
picture switching, audio continuity while deliberately blocking the game and
page, screenshot pixels, and save retry. Cold audio format switches and actual
device performance require additional browser sampling; synthetic timing is
not a cross-device guarantee.
