set(PLAYER_MOVIE_FFMPEG_ARCHIVE "${CMAKE_BINARY_DIR}/downloads/ffmpeg-7.1.5.tar.xz"
	CACHE FILEPATH "FFmpeg 7.1.5 archive; may point to an existing offline copy")
set(movie_ffmpeg_sha256 de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f)
if(NOT EXISTS "${PLAYER_MOVIE_FFMPEG_ARCHIVE}")
	get_filename_component(movie_download_dir "${PLAYER_MOVIE_FFMPEG_ARCHIVE}" DIRECTORY)
	file(MAKE_DIRECTORY "${movie_download_dir}")
	file(DOWNLOAD "https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz" "${PLAYER_MOVIE_FFMPEG_ARCHIVE}.tmp"
		EXPECTED_HASH "SHA256=${movie_ffmpeg_sha256}" TLS_VERIFY ON)
	file(RENAME "${PLAYER_MOVIE_FFMPEG_ARCHIVE}.tmp" "${PLAYER_MOVIE_FFMPEG_ARCHIVE}")
endif()
file(SHA256 "${PLAYER_MOVIE_FFMPEG_ARCHIVE}" movie_archive_sha256)
if(NOT movie_archive_sha256 STREQUAL movie_ffmpeg_sha256)
	message(FATAL_ERROR "FFmpeg 7.1.5 archive has an unexpected SHA256: ${PLAYER_MOVIE_FFMPEG_ARCHIVE}")
endif()
find_program(PLAYER_MOVIE_BASH bash REQUIRED)
set(movie_dir "${CMAKE_CURRENT_BINARY_DIR}/movie-decoder-build")
set(movie_install "${movie_dir}/install")
set(movie_libraries)
foreach(library avformat avcodec swscale swresample avutil)
	list(APPEND movie_libraries "${movie_install}/lib/lib${library}.a")
endforeach()
file(MAKE_DIRECTORY "${movie_install}/include")
add_custom_command(OUTPUT ${movie_libraries}
	COMMAND "${PLAYER_MOVIE_BASH}" "${CMAKE_SOURCE_DIR}/builds/android/build-movie-decoder.sh"
		"${movie_dir}" "${PLAYER_MOVIE_FFMPEG_ARCHIVE}" "${CMAKE_C_COMPILER}"
		"${CMAKE_C_COMPILER_TARGET}" "${CMAKE_SYSROOT}" "${CMAKE_SYSTEM_PROCESSOR}"
		"${CMAKE_AR}" "${CMAKE_RANLIB}"
	DEPENDS builds/android/build-movie-decoder.sh "${PLAYER_MOVIE_FFMPEG_ARCHIVE}"
	VERBATIM)
add_custom_target(player-android-movie-decoder DEPENDS ${movie_libraries})
add_dependencies(${PROJECT_NAME} player-android-movie-decoder)
target_include_directories(${PROJECT_NAME} PRIVATE "${movie_install}/include")
target_link_libraries(${PROJECT_NAME} ${movie_libraries} m)
target_compile_definitions(${PROJECT_NAME} PUBLIC SUPPORT_MOVIES=1 HAVE_ANDROID_MOVIE_PLAYER=1)
target_sources(${PROJECT_NAME} PRIVATE
	src/platform/emscripten/movie_decoder.c
	src/platform/android/movie_player_android.cpp src/platform/android/movie_player_android.h)
