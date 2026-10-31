#!/bin/sh
set -eu

BUILD_DIR="${BUILD_DIR:-build/emscripten-release}"
ARTIFACT_DIR="${ARTIFACT_DIR:-build/artifacts}"
PACKAGE_DIR="${PACKAGE_DIR:-build/package/web}"
ASSET_NAME="${ASSET_NAME:-EasyRPG-Player-Kai-local-web.zip}"

for file in easyrpg-player.js easyrpg-player.wasm easyrpg-player.data easyrpg-player-asyncify.js easyrpg-player-asyncify.wasm player-host.js player-worker.js player-audio.js player-audio-worker.js player-files.js player-movie.js player-movie-worker.js movie-decoder.js movie-decoder.wasm movie-decoder.LICENSE.txt; do
	if [ ! -s "$BUILD_DIR/$file" ]; then
		echo "Missing required Web build output: $BUILD_DIR/$file" >&2
		exit 1
	fi
done

rm -rf "$PACKAGE_DIR" "$ARTIFACT_DIR/$ASSET_NAME"
mkdir -p "$PACKAGE_DIR" "$ARTIFACT_DIR"
ARTIFACT_PATH="$(cd "$ARTIFACT_DIR" && pwd)/$ASSET_NAME"

cp "$BUILD_DIR/player-host.js" "$BUILD_DIR/player-worker.js" "$BUILD_DIR/player-audio.js" "$PACKAGE_DIR/"
cp "$BUILD_DIR/player-audio-worker.js" "$BUILD_DIR/player-files.js" "$PACKAGE_DIR/"
cp "$BUILD_DIR/player-movie.js" "$BUILD_DIR/player-movie-worker.js" "$PACKAGE_DIR/"
cp "$BUILD_DIR/movie-decoder.js" "$BUILD_DIR/movie-decoder.wasm" "$BUILD_DIR/movie-decoder.LICENSE.txt" "$PACKAGE_DIR/"
cp docs/web-movies.md "$PACKAGE_DIR/"
cp docs/web-audio.md "$PACKAGE_DIR/"
cp COPYING "$PACKAGE_DIR/"
cp "$BUILD_DIR/easyrpg-player.js" "$PACKAGE_DIR/"
cp "$BUILD_DIR/easyrpg-player.wasm" "$PACKAGE_DIR/"
cp "$BUILD_DIR/easyrpg-player.data" "$PACKAGE_DIR/"
cp "$BUILD_DIR/easyrpg-player-asyncify.js" "$BUILD_DIR/easyrpg-player-asyncify.wasm" "$PACKAGE_DIR/"

cat > "$PACKAGE_DIR/README.txt" <<'EOF'
EasyRPG Player Kai private archive-site Web build.

Load player-host.js in the site's isolated player document. Pass runtimeBase,
workId and WORKERFS packages to createEasyRpgPlayer. Game files must already be
installed locally; this build does not download game resources.

The engine and WebGL2 display run in one dedicated Worker. A separate audio
Worker runs the native decoders/mixer and supplies AudioWorklet directly.
Workers select native Promise suspension when supported, otherwise Asyncify.
Installed files use a bounded memory cache. No SharedArrayBuffer or COOP/COEP
is required.

Keep easyrpg-player.js, easyrpg-player.wasm, easyrpg-player-asyncify.js,
easyrpg-player-asyncify.wasm, easyrpg-player.data, player-host.js,
player-worker.js, player-audio.js, player-audio-worker.js, player-files.js,
player-movie.js, player-movie-worker.js,
movie-decoder.js and movie-decoder.wasm together at one immutable runtime URL.
Both engine variants share easyrpg-player.data, containing the recommended
SoundFont. Saves use /work-saves/<workId> IDBFS.
Await player.stop() before destroying the player document.

Legacy movies use a lazy-loaded, decode-only FFmpeg WASM worker. See web-movies.md
for supported codecs, build sources, resource limits and hosting requirements.

EOF

if command -v zip >/dev/null 2>&1; then
	(cd "$PACKAGE_DIR" && zip -9 -r "$ARTIFACT_PATH" .)
elif command -v python3 >/dev/null 2>&1; then
	PACKAGE_DIR="$PACKAGE_DIR" ARTIFACT_PATH="$ARTIFACT_PATH" python3 - <<'PY'
import os
import zipfile

package_dir = os.environ["PACKAGE_DIR"]
artifact_path = os.environ["ARTIFACT_PATH"]

with zipfile.ZipFile(artifact_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
	for root, _, files in os.walk(package_dir):
		for name in files:
			path = os.path.join(root, name)
			archive.write(path, os.path.relpath(path, package_dir))
PY
else
	powershell.exe -NoProfile -Command \
		"Compress-Archive -Path '${PACKAGE_DIR}/*' -DestinationPath '${ARTIFACT_PATH}' -Force"
fi
