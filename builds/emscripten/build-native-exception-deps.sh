#!/usr/bin/env bash
set -eo pipefail

sdk="$(cd "$1" && pwd)"
mkdir -p "$2"
prefix="$(cd "$2" && pwd)"
# Use the same dependency versions as the pinned EasyRPG SDK.
source "$sdk/../shared/packages.sh"
source "$sdk/emsdk-portable/emsdk_env.sh"
signature="wasm-longjmp-v1 $LIBPNG_DIR $FREETYPE_DIR $(emcc --version | head -n 1)"
if [ -f "$prefix/.complete" ] && [ "$(cat "$prefix/.complete")" = "$signature" ] \
		&& [ -s "$prefix/lib/libpng16.a" ] && [ -s "$prefix/lib/libfreetype.a" ]; then
	exit 0
fi

mkdir -p "$prefix/source"
cd "$prefix/source"
for dependency in LIBPNG FREETYPE; do
	directory_var="${dependency}_DIR"
	url_var="${dependency}_URL"
	directory="${!directory_var}"
	if [ ! -d "$directory" ]; then
		curl --fail --location --retry 2 "${!url_var}" -o "$directory.tar.xz"
		tar -xf "$directory.tar.xz"
	fi
done

# These two C libraries use setjmp/longjmp. Their ABI must match native Wasm
# exceptions; reusing the SDK's JavaScript longjmp objects breaks the link.
rm -rf "$prefix/png-build" "$prefix/freetype-build"
emcmake cmake -S "$LIBPNG_DIR" -B "$prefix/png-build" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
	-DCMAKE_C_FLAGS=-sSUPPORT_LONGJMP=wasm \
	-DCMAKE_PREFIX_PATH="$sdk" \
	-DCMAKE_FIND_ROOT_PATH="$sdk" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	-DPNG_SHARED=OFF -DPNG_TOOLS=OFF -DPNG_TESTS=OFF
cmake --build "$prefix/png-build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
cmake --install "$prefix/png-build"

emcmake cmake -S "$FREETYPE_DIR" -B "$prefix/freetype-build" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
	-DCMAKE_C_FLAGS=-sSUPPORT_LONGJMP=wasm \
	-DCMAKE_PREFIX_PATH="$sdk" \
	-DCMAKE_FIND_ROOT_PATH="$sdk" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	-DPNG_LIBRARY_RELEASE="$prefix/lib/libpng16.a" \
	-DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_BROTLI=ON
cmake --build "$prefix/freetype-build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
cmake --install "$prefix/freetype-build"
printf '%s\n' "$signature" > "$prefix/.complete"
