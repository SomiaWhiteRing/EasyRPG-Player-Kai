#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
CACHE_ENV_FILE="${CACHE_ENV_FILE:-$SCRIPT_DIR/package-ubuntu-nightly.cache.env}"

WEB_ASSET_NAME="${WEB_ASSET_NAME:-EasyRPG-Player-Kai-nightly-web.zip}"
ANDROID_ASSET_NAME="${ANDROID_ASSET_NAME:-EasyRPG-Player-Kai-nightly-android.apk}"
ANDROID_ABIS="${ANDROID_ABIS:-armeabi-v7a,arm64-v8a,x86,x86_64}"
ANDROID_VERBOSE_PACKAGE_INSPECTION="${ANDROID_VERBOSE_PACKAGE_INSPECTION:-0}"
ARTIFACT_DIR="${ARTIFACT_DIR:-build/artifacts}"
WEB_TOOLCHAIN_RETRIES="${WEB_TOOLCHAIN_RETRIES:-3}"
CACHE_SCHEMA_VERSION=1

if [ -f "$CACHE_ENV_FILE" ]; then
	# shellcheck disable=SC1090
	source "$CACHE_ENV_FILE"
fi
ANDROID_NDK_VERSION="${ANDROID_NDK_VERSION:-28.2.13676358}"
BUILDSCRIPTS_REF="${BUILDSCRIPTS_REF:-e32f89b02864dbbca81d317575839668f18e7f57}"

log() {
	printf '\n==> %s\n' "$*"
}

usage() {
	cat <<'EOF'
Usage: bash ./builds/ci/package-ubuntu-nightly.sh [web|android|all]

Builds the Ubuntu-hosted nightly artifacts with the same entrypoint used by
GitHub Actions and local Docker Desktop packaging.

Windows Player.exe is built by builds/ci/package-windows-nightly.ps1 on the
windows-2022 runner, because that artifact depends on MSVC and the EasyRPG
Windows vcpkg toolchain.
EOF
}

ensure_buildscripts_clone() {
	local buildscripts_dir="$1"
	local clone_mode="$2"
	local buildscripts_ref="${3:-}"

	mkdir -p "$(dirname "$buildscripts_dir")"
	if [ ! -d "$buildscripts_dir/.git" ]; then
		rm -rf "$buildscripts_dir"
		if [ "$clone_mode" = "shallow" ]; then
			git clone --depth 1 https://github.com/EasyRPG/buildscripts.git "$buildscripts_dir"
		else
			git clone https://github.com/EasyRPG/buildscripts.git "$buildscripts_dir"
		fi
	fi

	if [ -n "$buildscripts_ref" ]; then
		if [ "$(git -C "$buildscripts_dir" rev-parse HEAD 2>/dev/null || true)" != "$buildscripts_ref" ]; then
			if ! git -C "$buildscripts_dir" cat-file -e "$buildscripts_ref^{commit}" 2>/dev/null; then
				git -C "$buildscripts_dir" fetch --depth 1 origin "$buildscripts_ref"
			fi
			git -C "$buildscripts_dir" checkout --force "$buildscripts_ref"
		fi
	fi
}

dockerfile_cache_hash() {
	sha256sum "$REPO_ROOT/builds/docker/package.Dockerfile" | awk '{print $1}'
}

toolchain_cache_signature() {
	local target="$1"

	printf 'schema=%s\n' "$CACHE_SCHEMA_VERSION"
	printf 'target=%s\n' "$target"
	printf 'buildscripts_ref=%s\n' "$BUILDSCRIPTS_REF"
	printf 'dockerfile_sha256=%s\n' "$(dockerfile_cache_hash)"
	if [ "$target" = "android" ]; then
		printf 'android_ndk_version=%s\n' "$ANDROID_NDK_VERSION"
	fi
}

toolchain_cache_marker_matches() {
	local marker_file="$1"
	local target="$2"

	[ -f "$marker_file" ] && [ "$(cat "$marker_file")" = "$(toolchain_cache_signature "$target")" ]
}

stamp_toolchain_cache() {
	local marker_file="$1"
	local target="$2"

	mkdir -p "$(dirname "$marker_file")"
	toolchain_cache_signature "$target" > "$marker_file"
}

reset_buildscripts_subdir() {
	local buildscripts_dir="$1"
	local subdir="$2"

	git -C "$buildscripts_dir" clean -ffdx -- "$subdir"
	git -C "$buildscripts_dir" checkout --force HEAD -- "$subdir"
}

current_buildscripts_ref() {
	local buildscripts_dir="$1"

	if [ -d "$buildscripts_dir/.git" ]; then
		git -C "$buildscripts_dir" rev-parse HEAD 2>/dev/null || true
	fi
}

toolchain_cache_needs_reset() {
	local marker_file="$1"
	local target="$2"
	local previous_buildscripts_ref="$3"

	if [ -f "$marker_file" ]; then
		! toolchain_cache_marker_matches "$marker_file" "$target"
	else
		[ -n "$previous_buildscripts_ref" ] && [ "$previous_buildscripts_ref" != "$BUILDSCRIPTS_REF" ]
	fi
}

prepare_web_toolchain() {
	local emscripten_work_dir="$1"
	local attempt=1
	local status=0

	while [ "$attempt" -le "$WEB_TOOLCHAIN_RETRIES" ]; do
		(
			cd "$emscripten_work_dir"
			BUILD_LIBLCF=1 bash ./0_build_everything.sh
		) && return 0

		status=$?
		if [ "$attempt" -ge "$WEB_TOOLCHAIN_RETRIES" ]; then
			echo "EasyRPG Emscripten toolchain preparation failed after $attempt attempt(s)."
			return "$status"
		fi

		echo "EasyRPG Emscripten toolchain preparation failed with exit code $status; retrying ($((attempt + 1))/$WEB_TOOLCHAIN_RETRIES)."
		rm -rf "$emscripten_work_dir/emsdk-portable"
		attempt=$((attempt + 1))
		sleep 10
	done
}

package_web() {
	local build_dir="${BUILD_DIR:-build/emscripten-release}"
	local buildscripts_dir="${WEB_BUILDSCRIPTS_DIR:-${BUILDSCRIPTS_DIR:-$PWD/external/local-docker/web/buildscripts}}"
	local emscripten_work_dir="${EMSCRIPTEN_WORK_DIR:-$buildscripts_dir/emscripten}"
	local package_dir="${PACKAGE_DIR:-build/package/nightly-web}"
	local marker_file="$emscripten_work_dir/.player-toolchain-cache"
	local previous_buildscripts_ref

	log "Cloning EasyRPG buildscripts for Web"
	previous_buildscripts_ref="$(current_buildscripts_ref "$buildscripts_dir")"
	ensure_buildscripts_clone "$buildscripts_dir" shallow "$BUILDSCRIPTS_REF"

	if toolchain_cache_needs_reset "$marker_file" web "$previous_buildscripts_ref"; then
		log "Discarding stale Web toolchain cache"
		reset_buildscripts_subdir "$buildscripts_dir" emscripten
	fi

	log "Preparing EasyRPG Emscripten toolchain"
	(
		cd "$emscripten_work_dir"
		if [ ! -x emsdk-portable/upstream/emscripten/emcc ] || [ ! -f lib/cmake/liblcf/liblcf-config.cmake ]; then
			prepare_web_toolchain "$emscripten_work_dir"
		fi
		test -x emsdk-portable/upstream/emscripten/emcc
		test -f lib/cmake/liblcf/liblcf-config.cmake
		stamp_toolchain_cache "$marker_file" web
	)

	log "Configuring Web build"
	(
		# shellcheck disable=SC1091
		source "$emscripten_work_dir/emsdk-portable/emsdk_env.sh"
		local version_suffix="(nightly, $(date -u +%Y-%m-%d))"
		cmake -S . -B "$build_dir" -G Ninja \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_TOOLCHAIN_FILE="$emscripten_work_dir/emsdk-portable/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake" \
			-DPLAYER_PREFIX_PATH_APPEND="$emscripten_work_dir" \
			-DPLAYER_FIND_ROOT_PATH_APPEND=ON \
			-DPLAYER_ENABLE_TESTS=OFF \
			-DPLAYER_BUILD_LIBLCF=ON \
			-DPLAYER_TARGET_PLATFORM=Web \
			-DPLAYER_VERSION_APPEND="$version_suffix"

		log "Building Web"
		cmake --build "$build_dir"
	)

	log "Verifying Web outputs"
	for file in easyrpg-player.js easyrpg-player.wasm player-host.js player-worker.js player-audio.js player-audio-worker.js player-files.js player-movie.js player-movie-worker.js movie-decoder.js movie-decoder.wasm movie-decoder.LICENSE.txt; do
		test -s "$build_dir/$file" || {
			echo "Missing build output: $build_dir/$file"
			exit 1
		}
	done
	grep -q "createEasyRpgEngine" "$build_dir/easyrpg-player.js"

	log "Packaging Web artifact"
	BUILD_DIR="$build_dir" \
		ARTIFACT_DIR="$ARTIFACT_DIR" \
		PACKAGE_DIR="$package_dir" \
		ASSET_NAME="$WEB_ASSET_NAME" \
		bash ./builds/package-web.sh

	log "Web artifact: $ARTIFACT_DIR/$WEB_ASSET_NAME"
	if [ -n "${GITHUB_OUTPUT:-}" ]; then
		echo "artifact_path=$ARTIFACT_DIR/$WEB_ASSET_NAME" >> "$GITHUB_OUTPUT"
	fi
}

run_sdkmanager() {
	set +o pipefail
	yes | android-sdk/cmdline-tools/latest/bin/sdkmanager --sdk_root="$ANDROID_SDK_ROOT" "$@"
	local sdkmanager_status=${PIPESTATUS[1]}
	set -o pipefail
	return "$sdkmanager_status"
}

is_android_sdk_package_installed() {
	local package="$1"
	local package_dir="${package//;/\/}"

	[ -d "$ANDROID_SDK_ROOT/$package_dir" ]
}

is_android_toolchain_complete() {
	local abi="$1"
	[ -f "$abi-toolchain/lib/cmake/liblcf/liblcf-config.cmake" ]
}

inspect_android_packaging_inputs() {
	cd builds/android
	local app_build_dir="${ANDROID_GRADLE_APP_BUILD_DIR:-app/build}"
	echo "APK outputs:"
	find "$app_build_dir/outputs/apk" -maxdepth 4 -type f -print -exec ls -lh {} \; 2>/dev/null || true
	echo "APK native libraries and built-in assets:"
	find "$app_build_dir/outputs/apk" -type f -name '*.apk' -print -exec python3 -c '
import sys
import zipfile

apk_path = sys.argv[1]
with zipfile.ZipFile(apk_path) as archive:
	for name in sorted(archive.namelist()):
		if name.startswith("lib/"):
			print(name)
' {} \; 2>/dev/null || true

	if [ "$ANDROID_VERBOSE_PACKAGE_INSPECTION" != "1" ]; then
		return
	fi

	echo "Compressed assets:"
	echo "Native libraries:"
	find "$app_build_dir/intermediates" -path '*native_libs*' -type f \( -name '*.so' -o -name '*.json' \) -print -exec ls -lh {} \; 2>/dev/null || true
}

run_android_gradle() {
	local wrapper="./gradlew"
	local runner="$wrapper"
	local runner_status

	if LC_ALL=C grep -q $'\r' "$wrapper"; then
		runner="./.gradlew-docker"
		tr -d '\r' < "$wrapper" > "$runner"
	fi

	chmod +x "$runner"
	set +e
	# Signing runs after Gradle; do not expose secrets to its daemon or caches.
	env -u ANDROID_RELEASE_KEYSTORE_BASE64 -u ANDROID_RELEASE_STORE_PASSWORD \
		-u ANDROID_RELEASE_KEY_ALIAS -u ANDROID_RELEASE_KEY_PASSWORD \
		"$runner" "$@"
	runner_status=$?
	set -e

	if [ "$runner" != "$wrapper" ]; then
		rm -f "$runner"
	fi

	return "$runner_status"
}

package_android() {
	: "${ANDROID_RELEASE_KEYSTORE_BASE64:?Missing Android release keystore}"
	: "${ANDROID_RELEASE_STORE_PASSWORD:?Missing Android release store password}"
	: "${ANDROID_RELEASE_KEY_ALIAS:?Missing Android release key alias}"
	: "${ANDROID_RELEASE_KEY_PASSWORD:?Missing Android release key password}"
	local buildscripts_dir="${ANDROID_BUILDSCRIPTS_DIR:-${BUILDSCRIPTS_DIR:-$PWD/external/local-docker/android/buildscripts}}"
	local android_work_dir="${ANDROID_WORK_DIR:-$buildscripts_dir/android}"
	local android_gradle_build_root="${ANDROID_GRADLE_BUILD_ROOT:-}"
	local android_gradle_app_build_dir="${ANDROID_GRADLE_APP_BUILD_DIR:-}"
	local android_gradle_cxx_build_dir="${ANDROID_GRADLE_CXX_BUILD_DIR:-}"
	local android_gradle_project_cache_dir="${ANDROID_GRADLE_PROJECT_CACHE_DIR:-}"
	local version_code="${VERSION_CODE_OVERRIDE:-$((9778000 + ${GITHUB_RUN_NUMBER:-0}))}"
	local marker_file="$android_work_dir/.player-toolchain-cache"
	local previous_buildscripts_ref

	if [ -n "$android_gradle_build_root" ]; then
		android_gradle_app_build_dir="${android_gradle_app_build_dir:-$android_gradle_build_root/app}"
		android_gradle_cxx_build_dir="${android_gradle_cxx_build_dir:-$android_gradle_build_root/app-cxx}"
		android_gradle_project_cache_dir="${android_gradle_project_cache_dir:-$android_gradle_build_root/project-cache}"
	fi

	export ANDROID_WORK_DIR="$android_work_dir"
	export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$android_work_dir/android-sdk}"
	export ANDROID_HOME="${ANDROID_HOME:-$android_work_dir/android-sdk}"
	export ANDROID_GRADLE_APP_BUILD_DIR="${android_gradle_app_build_dir:-app/build}"
	mkdir -p "${HOME:-/tmp}/.android"

	log "Cloning EasyRPG buildscripts for Android"
	previous_buildscripts_ref="$(current_buildscripts_ref "$buildscripts_dir")"
	ensure_buildscripts_clone "$buildscripts_dir" full "$BUILDSCRIPTS_REF"
	if toolchain_cache_needs_reset "$marker_file" android "$previous_buildscripts_ref"; then
		log "Discarding stale Android toolchain cache"
		reset_buildscripts_subdir "$buildscripts_dir" android
	fi

	sed -i \
		-e '/extras;android;m2repository/d' \
		-e '/extras;google;m2repository/d' \
		"$android_work_dir/1_download_library.sh"
	sed -i \
		-e "s/ndk;[0-9.][0-9.]*/ndk;$ANDROID_NDK_VERSION/g" \
		-e "s#ndk/[0-9.][0-9.]*#ndk/$ANDROID_NDK_VERSION#g" \
		"$android_work_dir/1_download_library.sh" \
		"$android_work_dir/2_build_toolchain.sh"

	log "Preparing EasyRPG Android toolchain"
	(
		cd "$android_work_dir"

		local needs_toolchain=0
		local abi
		for abi in armeabi-v7a arm64-v8a x86 x86_64; do
			if ! is_android_toolchain_complete "$abi"; then
				echo "Android dependency toolchain is missing required files for $abi; rebuilding."
				needs_toolchain=1
			fi
		done

		if [ ! -x android-sdk/cmdline-tools/latest/bin/sdkmanager ] || [ "$needs_toolchain" -eq 1 ]; then
			rm -rf android-sdk \
				armeabi-v7a-toolchain \
				arm64-v8a-toolchain \
				x86-toolchain \
				x86_64-toolchain
			BUILD_LIBLCF=1 bash ./1_download_library.sh
			needs_toolchain=1
		fi

		local sdk_package
		local sdk_packages_to_install=()
		for sdk_package in "platforms;android-36" "build-tools;36.0.0"; do
			if ! is_android_sdk_package_installed "$sdk_package"; then
				sdk_packages_to_install+=("$sdk_package")
			fi
		done

		if [ "${#sdk_packages_to_install[@]}" -gt 0 ]; then
			run_sdkmanager --licenses >/dev/null || true
			run_sdkmanager "${sdk_packages_to_install[@]}"
		fi

		if [ "$needs_toolchain" -eq 1 ]; then
			BUILD_LIBLCF=1 bash ./2_build_toolchain.sh
			bash ./3_cleanup.sh
		fi

		for abi in armeabi-v7a arm64-v8a x86 x86_64; do
			if ! is_android_toolchain_complete "$abi"; then
				echo "Android dependency toolchain is incomplete for $abi after build."
				find "$abi-toolchain" -maxdepth 4 -type f -name 'liblcf-config.cmake' -print || true
				exit 1
			fi
		done
		stamp_toolchain_cache "$marker_file" android
	)

	log "Building Android APK"
	local gradle_started=$SECONDS
	ccache --zero-stats
	(
		cd builds/android
		local gradle_args=(
			-PtoolchainDirs="$android_work_dir" \
			-PandroidUseCcache=true \
			-PABI_FILTERS_RELEASE="$ANDROID_ABIS" \
			-PandroidUnsignedRelease=true \
			-PVERSION_CODE_OVERRIDE="$version_code" \
			-PcmakeOptions="-DPLAYER_TARGET_PLATFORM=SDL3 -DPLAYER_BUILD_LIBLCF=ON" \
			assembleRelease \
			--stacktrace
		)

		if [ -n "$android_gradle_app_build_dir" ]; then
			mkdir -p "$android_gradle_app_build_dir"
			gradle_args=("-PandroidBuildDir=$android_gradle_app_build_dir" "${gradle_args[@]}")
		fi
		if [ -n "$android_gradle_cxx_build_dir" ]; then
			mkdir -p "$android_gradle_cxx_build_dir"
			gradle_args=("-PandroidCxxBuildDir=$android_gradle_cxx_build_dir" "${gradle_args[@]}")
		fi
		if [ -n "$android_gradle_project_cache_dir" ]; then
			mkdir -p "$android_gradle_project_cache_dir"
			gradle_args=("--project-cache-dir" "$android_gradle_project_cache_dir" "${gradle_args[@]}")
		fi

		run_android_gradle "${gradle_args[@]}"
	)
	log "Android Gradle build completed in $((SECONDS - gradle_started)) seconds"
	ccache --show-stats

	log "Inspecting Android packaging inputs"
	(inspect_android_packaging_inputs)

	log "Staging Android artifact"
	local artifact_dir="$ARTIFACT_DIR"
	local asset_path="$artifact_dir/$ANDROID_ASSET_NAME"
	local apk_path="${android_gradle_app_build_dir:-builds/android/app/build}/outputs/apk/release/app-release-unsigned.apk"

	test -s "$apk_path" || {
		echo "Missing build output: $apk_path"
		exit 1
	}
	mkdir -p "$artifact_dir"
	bash "$SCRIPT_DIR/sign-android-release.sh" "$apk_path" "$asset_path"
	apk_path="$asset_path"
	APK_PATH="$apk_path" ANDROID_ABIS="$ANDROID_ABIS" python3 - <<'PY'
import os
import sys
import zipfile

apk_path = os.environ["APK_PATH"]
abis = [abi for abi in os.environ["ANDROID_ABIS"].split(",") if abi]
required = [f"lib/{abi}/libeasyrpg_android.so" for abi in abis]

with zipfile.ZipFile(apk_path) as archive:
	names = set(archive.namelist())
	uncompressed_libraries = [
		info.filename for info in archive.infolist()
		if info.filename.startswith("lib/") and info.filename.endswith(".so")
		and info.compress_type != zipfile.ZIP_DEFLATED
	]

if uncompressed_libraries:
	raise SystemExit(f"Native libraries must be compressed: {uncompressed_libraries}")

apk_size = os.path.getsize(apk_path)
if apk_size >= 100_000_000:
	raise SystemExit(f"Android APK exceeds the 100 MB download limit: {apk_size} bytes")
print(f"Android APK size: {apk_size:,} bytes; native libraries compressed")

missing = [name for name in required if name not in names]
if missing:
	print(f"Missing Android APK entries in {apk_path}:")
	for name in missing:
		print(f"  {name}")
	print("APK entries relevant to native libraries and built-in assets:")
	for name in sorted(names):
		if name.startswith("lib/"):
			print(f"  {name}")
	sys.exit(1)
PY

	log "Android artifact: $asset_path"
	if [ -n "${GITHUB_OUTPUT:-}" ]; then
		echo "artifact_path=$asset_path" >> "$GITHUB_OUTPUT"
	fi
}

main() {
	local target="${1:-all}"

	case "$target" in
		web)
			package_web
			;;
		android)
			package_android
			;;
		all)
			package_web
			package_android
			;;
		-h|--help|help)
			usage
			;;
		*)
			usage >&2
			exit 2
			;;
	esac
}

main "$@"
