cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{DEVKITPRO})
	set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/Switch.cmake")
endif()

project(RtCWNX VERSION 0.1.0 LANGUAGES C CXX ASM)

if(NOT NINTENDO_SWITCH)
	message(FATAL_ERROR "RtCWNX targets the Nintendo Switch: configure it with the devkitA64 toolchain.")
endif()

set(RTCW_RENDERER_BACKEND "opengl" CACHE STRING "What draws: deko3d, or opengl (the fallback, through Mesa)")
set_property(CACHE RTCW_RENDERER_BACKEND PROPERTY STRINGS deko3d opengl)

set(NX_APP_NAME "Return to Castle Wolfenstein")
set(NX_APP_AUTHOR "hazevauks")
set(NX_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg")
set(RTCW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/iortcw")

include(cmake/RtcwSources.cmake)
include(cmake/RtcwModule.cmake)

# id's code leans on type punning and was written where char is signed, which it is
# not on arm64: parsers, tables and the network's usercmds all assume it. Frame
# pointers keep the crash reports useful.
set(RTCW_COMMON_FLAGS
	-fsigned-char
	-fno-strict-aliasing
	-fno-omit-frame-pointer
	-g
	-w
)
set(RTCW_ENGINE_FLAGS -O3 -ffast-math ${RTCW_COMMON_FLAGS})
# The modules keep their writable data in plain .data and .bss, for RtcwModule.cmake.
set(RTCW_MODULE_FLAGS -O2 -fno-data-sections ${RTCW_COMMON_FLAGS})

# ---------------------------------------------------------------------------
# One game: its engine, its three modules, and the platform layer around them.
# ---------------------------------------------------------------------------
function(rtcw_add_game game output_name)
	set(code "${RTCW_ROOT}/${game}/code")
	string(TOLOWER ${game} lower)
	set(version "1.51d-${game}")
	set(game_defines PRODUCT_VERSION="${version}")
	if(game STREQUAL "MP")
		list(APPEND game_defines RTCW_MP)
	endif()

	set(RTCW_MODULE_OBJECTS "")
	set(RTCW_MODULE_TARGETS "")
	foreach(module CGAME QAGAME UI)
		string(TOLOWER ${module} module_lower)
		list(TRANSFORM RTCW_${game}_${module} PREPEND "${code}/" OUTPUT_VARIABLE sources)
		set(objects rtcw_${lower}_${module_lower}_code)
		add_library(${objects} OBJECT ${sources})
		target_compile_options(${objects} PRIVATE ${RTCW_MODULE_FLAGS})
		target_compile_definitions(${objects} PRIVATE ${game_defines})
		if(module STREQUAL "CGAME")
			target_compile_definitions(${objects} PRIVATE CGAMEDLL CGAME)
		elseif(module STREQUAL "QAGAME")
			target_compile_definitions(${objects} PRIVATE GAMEDLL QAGAME)
		else()
			target_compile_definitions(${objects} PRIVATE UI)
		endif()
		rtcw_add_module(rtcw_${lower}_${module_lower} ${module_lower} ${objects})
	endforeach()

	list(TRANSFORM RTCW_${game}_ENGINE PREPEND "${code}/" OUTPUT_VARIABLE engine_sources)
	list(TRANSFORM RTCW_BOTLIB PREPEND "${code}/" OUTPUT_VARIABLE botlib_sources)
	list(TRANSFORM RTCW_RENDERER PREPEND "${code}/" OUTPUT_VARIABLE renderer_sources)
	list(TRANSFORM RTCW_SPLINES PREPEND "${code}/" OUTPUT_VARIABLE splines_sources)
	set_source_files_properties(${botlib_sources} PROPERTIES COMPILE_DEFINITIONS BOTLIB)

	set(platform_sources
		source/platform/nx_main.c
		source/platform/nx_log.c
		source/platform/nx_crash.c
		source/platform/nx_modules.c
		source/platform/nx_zipio.c
		source/platform/sys_nx.c
		source/platform/in_nx.c
		source/platform/snd_nx.c
	)
	if(RTCW_RENDERER_BACKEND STREQUAL "deko3d")
		message(FATAL_ERROR "The deko3D back end is not part of this build yet")
	else()
		list(APPEND platform_sources source/platform/glimp_gl.c)
	endif()

	add_executable(${output_name}
		${engine_sources} ${botlib_sources} ${renderer_sources} ${splines_sources}
		${platform_sources} ${RTCW_MODULE_OBJECTS})
	add_dependencies(${output_name} ${RTCW_MODULE_TARGETS})
	target_include_directories(${output_name} PRIVATE
		source/platform
		${code}
		${code}/sys
	)
	target_include_directories(${output_name} SYSTEM PRIVATE ${DEVKITPRO}/portlibs/switch/include)
	target_compile_options(${output_name} PRIVATE ${RTCW_ENGINE_FLAGS})
	target_compile_definitions(${output_name} PRIVATE
		${game_defines}
		NO_VM_COMPILED
		DEFAULT_BASEDIR="sdmc:/switch/RtCWNX"
	)
	target_link_options(${output_name} PRIVATE
		-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${output_name}.map
		-Wl,--gc-sections
	)
	if(RTCW_RENDERER_BACKEND STREQUAL "deko3d")
		target_link_libraries(${output_name} PRIVATE deko3d jpeg z nx m)
	else()
		target_link_libraries(${output_name} PRIVATE EGL glapi drm_nouveau jpeg z nx m)
	endif()
endfunction()

rtcw_add_game(SP RtCWNX)
rtcw_add_game(MP RtCWNX_MP)

nx_generate_nacp(RtCWNX.nacp NAME "${NX_APP_NAME}" AUTHOR "${NX_APP_AUTHOR}" VERSION "${PROJECT_VERSION}")
nx_generate_nacp(RtCWNX_MP.nacp NAME "${NX_APP_NAME} Multiplayer" AUTHOR "${NX_APP_AUTHOR}" VERSION "${PROJECT_VERSION}")
nx_create_nro(RtCWNX NACP RtCWNX.nacp ICON "${NX_ICON}")
nx_create_nro(RtCWNX_MP NACP RtCWNX_MP.nacp ICON "${NX_ICON}")

# What gets copied to the SD card: see tools/package_sd.py for the game's files.
add_custom_target(sd_card ALL
	COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/SD_CARD/switch/RtCWNX/mp"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_PROPERTY:RtCWNX_nro,DKP_FILE>" "${CMAKE_CURRENT_SOURCE_DIR}/SD_CARD/switch/RtCWNX/RtCWNX.nro"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_PROPERTY:RtCWNX_MP_nro,DKP_FILE>" "${CMAKE_CURRENT_SOURCE_DIR}/SD_CARD/switch/RtCWNX/mp/RtCWNX_MP.nro"
	DEPENDS RtCWNX_nro RtCWNX_MP_nro
	COMMENT "Updating SD_CARD/"
	VERBATIM
)
