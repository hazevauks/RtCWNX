# Homebrew cannot load a library, but cgame, qagame and ui are written to be one.
# Linked as they are, their shared names (q_math, q_shared, bg_*) would clash. So
# each module's objects are linked into one relocatable object, whose every symbol
# is then made local but for its two entry points and the bounds of its writable
# data, which source/platform/nx_modules.c needs to give it fresh globals on each load.
#
#   rtcw_add_module(<target> <module name> <objects target>)
#
# Appends the result to RTCW_MODULE_OBJECTS in the caller's scope.
function(rtcw_add_module target name objects_target)
	set(output ${CMAKE_CURRENT_BINARY_DIR}/${target}.o)
	set(partial ${target}.partial.o)
	set(rsp ${CMAKE_CURRENT_BINARY_DIR}/${target}.rsp)
	set(script ${CMAKE_CURRENT_BINARY_DIR}/${target}.ld)

	file(WRITE ${script} "SECTIONS\n{\n"
		"\t.data.${target} : { module_data_start = .; *(.data .data.rel .data.rel.local) module_data_end = .; }\n"
		"\t.bss.${target} : { module_bss_start = .; *(.bss COMMON) module_bss_end = .; }\n"
		"}\n")

	# relative paths: the compiler is a native program on Windows, and does not
	# understand the paths CMake writes under MSYS2
	file(GENERATE OUTPUT ${rsp} CONTENT "$<JOIN:$<PATH:RELATIVE_PATH,$<TARGET_OBJECTS:${objects_target}>,${CMAKE_CURRENT_BINARY_DIR}>,\n>\n")

	add_custom_command(
		OUTPUT ${output}
		COMMAND ${CMAKE_C_COMPILER} -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft
			-r -nostdlib -Wl,-T,${target}.ld @${target}.rsp -o ${partial}
		COMMAND ${CMAKE_OBJCOPY}
			--keep-global-symbol=dllEntry --keep-global-symbol=vmMain
			--keep-global-symbol=module_data_start --keep-global-symbol=module_data_end
			--keep-global-symbol=module_bss_start --keep-global-symbol=module_bss_end
			${partial} ${target}.stage.o
		COMMAND ${CMAKE_OBJCOPY}
			--redefine-sym dllEntry=rtcw_${name}_dllEntry --redefine-sym vmMain=rtcw_${name}_vmMain
			--redefine-sym module_data_start=rtcw_${name}_data_start --redefine-sym module_data_end=rtcw_${name}_data_end
			--redefine-sym module_bss_start=rtcw_${name}_bss_start --redefine-sym module_bss_end=rtcw_${name}_bss_end
			${target}.stage.o ${target}.o
		COMMAND ${CMAKE_COMMAND} -E remove ${partial} ${target}.stage.o
		DEPENDS ${objects_target} $<TARGET_OBJECTS:${objects_target}> ${rsp} ${script}
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		COMMENT "Linking the ${name} module (${target})"
		VERBATIM
	)
	add_custom_target(${target}_object DEPENDS ${output})
	set_source_files_properties(${output} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
	set(RTCW_MODULE_OBJECTS ${RTCW_MODULE_OBJECTS} ${output} PARENT_SCOPE)
	set(RTCW_MODULE_TARGETS ${RTCW_MODULE_TARGETS} ${target}_object PARENT_SCOPE)
endfunction()
