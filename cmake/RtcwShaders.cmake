# The native renderer's programs, compiled with uam into the NROs' romfs: one vertex
# program, and shaders/rtcw.frag for each texture environment, with and without the
# portal's clip plane. uam takes no #defines of its own, so each gets a source file.
set(RTCW_ROMFS_DIR ${CMAKE_CURRENT_BINARY_DIR}/romfs)
set(RTCW_SHADER_SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${RTCW_ROMFS_DIR}/shaders ${RTCW_SHADER_SOURCE_DIR})

set(rtcw_vert ${RTCW_ROMFS_DIR}/shaders/rtcw_vert.dksh)
add_custom_command(
	OUTPUT ${rtcw_vert}
	COMMAND ${NX_UAM_EXE} -s vert -o ${rtcw_vert} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rtcw.vert
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rtcw.vert
	COMMENT "Compiling shader rtcw.vert"
	VERBATIM
)
set(RTCW_SHADER_OUTPUTS ${rtcw_vert})

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rtcw.frag)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rtcw.frag RTCW_FRAGMENT_BODY)

foreach(variant COLOR TEX TEX_REPLACE MULTI_MODULATE MULTI_ADD MULTI_DECAL)
	string(TOLOWER ${variant} name)
	foreach(clip "" "_clip")
		set(defines "#version 460\n#define ${variant}\n")
		if(clip)
			string(APPEND defines "#define CLIP\n")
		endif()
		set(source ${RTCW_SHADER_SOURCE_DIR}/${name}${clip}.frag)
		set(output ${RTCW_ROMFS_DIR}/shaders/${name}${clip}_frag.dksh)
		file(CONFIGURE OUTPUT ${source} CONTENT "${defines}${RTCW_FRAGMENT_BODY}" @ONLY)
		add_custom_command(
			OUTPUT ${output}
			COMMAND ${NX_UAM_EXE} -s frag -o ${output} ${source}
			DEPENDS ${source}
			COMMENT "Compiling shader ${name}${clip}"
			VERBATIM
		)
		list(APPEND RTCW_SHADER_OUTPUTS ${output})
	endforeach()
endforeach()

add_custom_target(rtcw_shaders DEPENDS ${RTCW_SHADER_OUTPUTS})
dkp_add_asset_target(rtcw_romfs ${RTCW_ROMFS_DIR})
add_dependencies(rtcw_romfs rtcw_shaders)
set_property(TARGET rtcw_romfs APPEND PROPERTY DKP_ASSET_FILES ${RTCW_SHADER_OUTPUTS})
