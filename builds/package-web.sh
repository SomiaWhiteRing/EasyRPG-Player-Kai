#!/bin/sh
set -eu

BUILD_DIR="${BUILD_DIR:-build/emscripten-release}"
ARTIFACT_DIR="${ARTIFACT_DIR:-build/artifacts}"
PACKAGE_DIR="${PACKAGE_DIR:-build/package/web}"
ASSET_NAME="${ASSET_NAME:-EasyRPG-Player-Kai-local-web.zip}"

for file in easyrpg-player.html easyrpg-player.js easyrpg-player.wasm; do
	if [ ! -s "$BUILD_DIR/$file" ]; then
		echo "Missing required Web build output: $BUILD_DIR/$file" >&2
		exit 1
	fi
done

rm -rf "$PACKAGE_DIR" "$ARTIFACT_DIR/$ASSET_NAME"
mkdir -p "$PACKAGE_DIR" "$ARTIFACT_DIR"
ARTIFACT_PATH="$(cd "$ARTIFACT_DIR" && pwd)/$ASSET_NAME"

cp "$BUILD_DIR/easyrpg-player.html" "$BUILD_DIR/easyrpg-player.js" "$BUILD_DIR/easyrpg-player.wasm" "$PACKAGE_DIR/"
cp resources/emscripten/indexgen.php "$PACKAGE_DIR/"
cp COPYING "$PACKAGE_DIR/"

cat > "$PACKAGE_DIR/README.txt" <<'EOF'
EasyRPG Player Web

Serve this directory over HTTP. Put a game under games/default/ or select a
game with ?game=<name> and put its files under games/<name>/. Generate an
index.json for each game with EasyRPG Tools gencache or the bundled indexgen.php.
Browser storage keeps each game's saves separately.
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
