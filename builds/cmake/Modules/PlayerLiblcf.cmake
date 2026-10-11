# Keep an existing developer checkout untouched. Clean builds use the same
# published base plus the compatibility changes shipped with this Player.
set(LIBLCF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lib/liblcf")
if(EXISTS "${LIBLCF_PATH}/CMakeLists.txt")
	return()
elseif(EXISTS "${LIBLCF_PATH}")
	message(FATAL_ERROR "Existing liblcf is incomplete; inspect it without deleting local files: ${LIBLCF_PATH}")
endif()

include(FetchContent)
find_package(Git REQUIRED)
set(liblcf_patch "${CMAKE_CURRENT_SOURCE_DIR}/builds/patches/liblcf-maniac-250921.patch")
file(SHA256 "${liblcf_patch}" liblcf_patch_hash)
# A changed patch gets a fresh source directory, including with a restored CI cache.
FetchContent_Declare(player_liblcf
	URL "https://codeload.github.com/SomiaWhiteRing/liblcf/tar.gz/4056dbf533182dd46c0a9b1831da23bc16cc0ae8"
	URL_HASH SHA256=6df846fa13f5af62334dcc2715fbf785a45a3607d4b0bce9b960af1e9b1036a9
	SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/liblcf-${liblcf_patch_hash}"
	PATCH_COMMAND "${GIT_EXECUTABLE}" init --quiet .
		COMMAND "${GIT_EXECUTABLE}" -c core.autocrlf=false apply --whitespace=nowarn "${liblcf_patch}")
FetchContent_MakeAvailable(player_liblcf)
set(LIBLCF_PATH "${player_liblcf_SOURCE_DIR}")
