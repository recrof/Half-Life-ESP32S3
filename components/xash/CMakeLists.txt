# Xash3D FWGS engine, software renderer, menu and the Half-Life game (hlsdk-portable), linked
# into the firmware. The source lists are shared with the host build (../../sources/*.txt).
#
# The engine finds its libraries (filesystem_stdio, ref_soft, menu, client, server) by name
# (engine/platform/misc/lib_static.c). Each library is linked into one relocatable object
# that only keeps its lib_<name>_exports table global: the libraries contain functions with
# the same names (pm_shared, UTIL_...).

get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT DEFINED XASH_DIR)
	get_filename_component(XASH_DIR "${ROOT}/xash3d-fwgs" ABSOLUTE)
endif()
if(NOT DEFINED HLSDK_DIR)
	get_filename_component(HLSDK_DIR "${ROOT}/hlsdk-portable" ABSOLUTE)
endif()

function(read_sources name out)
	file(STRINGS "${ROOT}/sources/${name}.txt" lines)
	set(files)
	foreach(l ${lines})
		string(REGEX REPLACE "^xash/" "${XASH_DIR}/" l "${l}")
		string(REGEX REPLACE "^hlsdk/" "${HLSDK_DIR}/" l "${l}")
		string(REGEX REPLACE "^port/" "${ROOT}/port/" l "${l}")
		list(APPEND files "${l}")
	endforeach()
	set(${out} ${files} PARENT_SCOPE)
endfunction()

read_sources(engine ENGINE_SRC)
read_sources(engine_esp ENGINE_ESP_SRC)

# ---- defines and include paths (as the host build, host/Makefile) ----
set(PORT_DEFS
	HISTORY_MAX=4 VOICE_HEAD_MODELS=4 DEFAULT_MAX_EDICTS=900 MAX_PHYSENTS=128 XASH_FS_CASE_INSENSITIVE=1
	SOUND_DMA_SPEED=22050)
set(XASH_DEFS
	XASH_GAMEDIR="valve" XASH_LOW_MEMORY=2 XASH_STATIC_LIBS=1 XASH_MINIZ=1 MINIZ_NO_ARCHIVE_APIS
	_GNU_SOURCE=1 ALLOCA_H=<alloca.h> HAVE_STRNCASECMP=1 HAVE_STRCASECMP=1 HAVE_STRLCPY=1
	HAVE_STRLCAT=1 HAVE_STRNLEN=1 XASH_BUILD_COMMIT="esp32s3" XASH_BUILD_BRANCH="esp32s3"
	XASH_BUILD_COMMIT_DATE="")
set(XASH_INC ${XASH_DIR}/public ${XASH_DIR}/common ${XASH_DIR}/pm_shared ${XASH_DIR}/engine
	${XASH_DIR}/3rdparty/library_suffix/include ${XASH_DIR}/3rdparty/miniz)
set(ENGINE_INC ${XASH_DIR}/engine/server ${XASH_DIR}/engine/client ${XASH_DIR}/engine/client/vgui
	${XASH_DIR}/engine/common/soundlib ${XASH_DIR}/engine/platform ${XASH_DIR}/engine/common
	${XASH_DIR}/engine/common/imagelib ${XASH_DIR}/filesystem ${XASH_INC}
	${XASH_DIR}/3rdparty/MultiEmulator/include ${XASH_DIR}/3rdparty/bzip2/bzip2
	${CMAKE_CURRENT_BINARY_DIR}/gen ${ROOT}/port ${ROOT}/main)
set(HL_DEFS HAVE_CMATH=1 stricmp=strcasecmp strnicmp=strncasecmp _snprintf=snprintf
	_vsnprintf=vsnprintf _LINUX=1 LINUX=1 CLIENT_WEAPONS=1 CROWBAR_FIX_RAPID_CROWBAR=1
	SPEAKABLE_TARGETS=1 HD_HUD_DEFAULT=1 HAVE_STRLCPY=1 HAVE_STRLCAT=1)
set(HL_INC ${HLSDK_DIR}/common ${HLSDK_DIR}/engine ${HLSDK_DIR}/pm_shared ${HLSDK_DIR}/game_shared
	${HLSDK_DIR}/public)

# no warnings (old code), signed chars (Xtensa chars are unsigned), single precision constants
# (the FPU has no double precision)
set(COMMON_OPTS -w -Wno-error=incompatible-pointer-types -fsigned-char -fno-strict-aliasing -fsingle-precision-constant -fno-math-errno -freciprocal-math)
set(CXX_OPTS -std=gnu++11 -fno-exceptions)

set(gen "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${gen}")

# constant tables of the software renderer, in flash (REF_CONST_TABLES)
set(REFTABLES "${gen}/reftables.c")
if(NOT EXISTS "${REFTABLES}" OR "${ROOT}/tools/gen_reftables.py" IS_NEWER_THAN "${REFTABLES}")
	execute_process(COMMAND python3 "${ROOT}/tools/gen_reftables.py" "${REFTABLES}" RESULT_VARIABLE res)
	if(res)
		message(FATAL_ERROR "tools/gen_reftables.py failed")
	endif()
endif()

# ---- engine (part of the component) ----
idf_component_register(
	SRCS ${ENGINE_SRC} ${ENGINE_ESP_SRC}
	INCLUDE_DIRS ${ROOT}/port
	PRIV_INCLUDE_DIRS ${ENGINE_INC}
	PRIV_REQUIRES esp_timer esp_psram heap lwip esp_netif esp_partition spi_flash
	LDFRAGMENTS linker.lf
)
if(CMAKE_BUILD_EARLY_EXPANSION)
	return()
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${PORT_DEFS} ${XASH_DEFS} ENGINE_DLL=1
	XASH_REF_SOFT_ENABLED=1 XASH_PORT_INPUT=1 XASH_MAPPED_FILES=1 XASH_SW_SWAP_BUFFER=1 XASH_PARALLEL2=1)
target_compile_options(${COMPONENT_LIB} PRIVATE ${COMMON_OPTS})

# the float math functions in the engine files that compute in floats (port/hl_floatmath.h;
# not the time, which is double)
foreach(f public/xash3d_mathlib.c public/matrixlib.c engine/common/pm_surface.c
		engine/client/sound/s_mix.c engine/server/sv_move.c engine/server/sv_game.c
		engine/server/sv_client.c engine/client/cl_view.c engine/common/mod_bmodel.c
		engine/common/mod_alias.c)
	set_source_files_properties("${XASH_DIR}/${f}" PROPERTIES COMPILE_OPTIONS "-include;${ROOT}/port/hl_floatmath.h")
endforeach()

# ---- libraries ----
set(LIB_NAMES filesystem_stdio ref_soft menu client server)
set(filesystem_stdio_DEFS ${XASH_DEFS} HAVE_DIRENT_D_TYPE=1 HL_FSSTAT=1) # TEMP: HL_FSSTAT
set(filesystem_stdio_GLOBALS -G fsstat_us -G fsstat_n -G fsstat_bytes -G fsstat_log -G fsstat_miss -G fsstat_flush) # TEMP: HL_FSSTAT
set(filesystem_stdio_INC ${XASH_DIR}/filesystem ${XASH_INC})
set(ref_soft_DEFS ${XASH_DEFS} REF_DLL=1 REF_CONST_TABLES=1)
set(ref_soft_INC ${XASH_DIR}/ref/soft ${XASH_DIR}/ref/common ${XASH_DIR}/engine/common
	${XASH_DIR}/engine/common/imagelib ${XASH_DIR}/filesystem ${XASH_INC})
# text menu (port/menu_text.c)
set(menu_DEFS ${XASH_DEFS})
set(menu_INC ${XASH_DIR}/engine ${XASH_INC})
set(server_DEFS ${HL_DEFS})
set(server_INC ${HLSDK_DIR}/dlls ${HL_INC})
set(client_DEFS ${HL_DEFS} CLIENT_DLL INTERNAL_VGUI_SUPPORT NO_VGUI_VIEWPORT=1)
set(client_INC ${HLSDK_DIR}/cl_dll ${HLSDK_DIR}/dlls ${HL_INC} ${HLSDK_DIR}/utils/vgui/include
	${HLSDK_DIR}/freevgui)
set(freevgui_DEFS ${HL_DEFS} INTERNAL_VGUI_SUPPORT)
set(freevgui_INC ${HLSDK_DIR}/freevgui ${HLSDK_DIR}/engine)

set(LD_TOOL "${CMAKE_LINKER}")
set(OBJCOPY_TOOL "${CMAKE_OBJCOPY}")
set(LIB_TABLE_EXTERNS "")
set(LIB_TABLE_ENTRIES "")
foreach(lib ${LIB_NAMES})
	read_sources(${lib} src)
	if(lib STREQUAL "ref_soft")
		list(APPEND src "${REFTABLES}")
	endif()

	# export table: lib_<name>_exports[] = { { "name", &name }, ... }
	file(STRINGS "${ROOT}/host/exports/${lib}.txt" exports)
	set(c "")
	foreach(e ${exports})
		string(APPEND c "extern void ${e}(void);\n")
	endforeach()
	string(APPEND c "struct { const char *name; void *func; } lib_${lib}_exports[] = {\n")
	foreach(e ${exports})
		string(APPEND c "{ \"${e}\", (void *)&${e} },\n")
	endforeach()
	string(APPEND c "{ 0, 0 } };\n")
	file(WRITE "${gen}/${lib}_exports.c.tmp" "${c}")
	configure_file("${gen}/${lib}_exports.c.tmp" "${gen}/${lib}_exports.c" COPYONLY)

	add_library(${lib}_objs OBJECT ${src} "${gen}/${lib}_exports.c")
	# the component flags (IDF include paths, -mlongcalls...)
	target_compile_options(${lib}_objs PRIVATE $<TARGET_PROPERTY:${COMPONENT_LIB},COMPILE_OPTIONS>)
	target_include_directories(${lib}_objs PRIVATE ${${lib}_INC})
	target_compile_definitions(${lib}_objs PRIVATE ${PORT_DEFS} ${${lib}_DEFS})
	target_compile_options(${lib}_objs PRIVATE ${COMMON_OPTS} $<$<COMPILE_LANGUAGE:CXX>:${CXX_OPTS}>)
	if(lib STREQUAL "client" OR lib STREQUAL "server")
		# freevgui (dynamic_cast)
		target_compile_options(${lib}_objs PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-frtti>)
		# the float math functions (port/hl_floatmath.h)
		target_compile_options(${lib}_objs PRIVATE -include ${ROOT}/port/hl_floatmath.h)
	endif()
	if(lib STREQUAL "client")
		# freevgui uses its own headers instead of utils/vgui/include
		foreach(f ${src})
			if(f MATCHES "/freevgui/")
				set_source_files_properties(${f} TARGET_DIRECTORY ${lib}_objs PROPERTIES
					INCLUDE_DIRECTORIES "${freevgui_INC}")
			endif()
		endforeach()
	endif()
	if(lib STREQUAL "ref_soft")
		set_target_properties(${lib}_objs PROPERTIES COMPILE_OPTIONS "")
		target_compile_options(${lib}_objs PRIVATE $<TARGET_PROPERTY:${COMPONENT_LIB},COMPILE_OPTIONS>
			${COMMON_OPTS} -O3 -fno-stack-protector -include ${ROOT}/port/hl_floatmath.h)
	endif()

	set(out "${CMAKE_CURRENT_BINARY_DIR}/${lib}.o")
	add_custom_command(OUTPUT "${out}"
		COMMAND "${LD_TOOL}" -r --force-group-allocation -o "${out}.full" $<TARGET_OBJECTS:${lib}_objs>
		COMMAND "${OBJCOPY_TOOL}" -G lib_${lib}_exports ${${lib}_GLOBALS} "${out}.full" "${out}"
		DEPENDS $<TARGET_OBJECTS:${lib}_objs>
		COMMAND_EXPAND_LISTS
		COMMENT "Linking ${lib}.o")
	list(APPEND LIB_OBJECTS "${out}")

	string(APPEND LIB_TABLE_EXTERNS "extern table_t lib_${lib}_exports[];\n")
	string(APPEND LIB_TABLE_ENTRIES "{ \"${lib}\", &lib_${lib}_exports },\n")
endforeach()

# the libraries in one archive, for the linker fragment (static data in PSRAM)
set(libs_archive "${CMAKE_CURRENT_BINARY_DIR}/libxashlibs.a")
add_custom_command(OUTPUT "${libs_archive}"
	COMMAND ${CMAKE_COMMAND} -E remove -f "${libs_archive}"
	COMMAND "${CMAKE_AR}" rcs "${libs_archive}" ${LIB_OBJECTS}
	DEPENDS ${LIB_OBJECTS}
	COMMENT "Archiving the game libraries")
add_custom_target(xashlibs_archive DEPENDS "${libs_archive}")
add_library(xashlibs STATIC IMPORTED GLOBAL)
set_target_properties(xashlibs PROPERTIES IMPORTED_LOCATION "${libs_archive}")
add_dependencies(xashlibs xashlibs_archive)
add_dependencies(${COMPONENT_LIB} xashlibs_archive)
target_link_libraries(${COMPONENT_LIB} INTERFACE xashlibs)

# relative paths are made absolute (port/esp/cwd_esp.c)
target_link_libraries(${COMPONENT_LIB} INTERFACE
	"-Wl,--wrap=open,--wrap=fopen,--wrap=stat,--wrap=opendir,--wrap=mkdir,--wrap=remove,--wrap=rename")

# the port layer (port/esp) calls the board code of the main component, linked before this one
idf_component_get_property(main_lib main COMPONENT_LIB)
target_link_libraries(${COMPONENT_LIB} INTERFACE ${main_lib})

file(WRITE "${gen}/generated_library_tables.h.tmp"
	"${LIB_TABLE_EXTERNS}struct { const char *name; void *func; } libs[] = {\n${LIB_TABLE_ENTRIES}{ 0, 0 }\n};\n")
configure_file("${gen}/generated_library_tables.h.tmp" "${gen}/generated_library_tables.h" COPYONLY)
