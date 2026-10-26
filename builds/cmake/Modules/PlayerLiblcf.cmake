# Keep an existing developer checkout untouched. Clean builds use the same
# published base plus the compatibility changes shipped with this Player.
set(LIBLCF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lib/liblcf")
if(EXISTS "${LIBLCF_PATH}/CMakeLists.txt")
	set(liblcf_data "${LIBLCF_PATH}/src/generated/lcf/rpg/saveeasyrpgdata.h")
	set(liblcf_frame "${LIBLCF_PATH}/src/generated/lcf/rpg/saveeventexecframe.h")
	set(liblcf_bank "${LIBLCF_PATH}/src/generated/lcf/rpg/saveeasyrpgselfvariablebank.h")
	set(liblcf_reader "${LIBLCF_PATH}/src/lcf/lsd/reader.h")
	if(EXISTS "${liblcf_data}" AND EXISTS "${liblcf_frame}" AND EXISTS "${liblcf_bank}" AND EXISTS "${liblcf_reader}")
		file(READ "${liblcf_data}" liblcf_data_contents)
		file(READ "${liblcf_frame}" liblcf_frame_contents)
		file(READ "${liblcf_reader}" liblcf_reader_contents)
		if(liblcf_data_contents MATCHES "maniac_self_variable_banks"
				AND liblcf_data_contents MATCHES "maniac_global_string_raw_bytes"
				AND liblcf_frame_contents MATCHES "easyrpg_maniac_return_type"
				AND liblcf_reader_contents MATCHES "bool maniac")
			return()
		endif()
	endif()
	message(STATUS "Existing liblcf lacks the required Maniac fields; using the pinned dependency without modifying it: ${LIBLCF_PATH}")
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
