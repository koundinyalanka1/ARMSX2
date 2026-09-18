add_library(pcsx2-libretro SHARED)

# The core name, without the _libretro suffix. A knob because __pagesize is
# compiled in, so a core serves one kernel page size and each variant needs a
# file of its own for the frontend to choose between (armsx2 at 4K,
# armsx2_16k at 16K) - see the ANDROID branch in cmake/BuildParameters.cmake
# for why one 16K binary for both is the wrong trade.
set(ARMSX2_LIBRETRO_CORE_NAME "armsx2" CACHE STRING
	"Base name of the libretro core, before the _libretro suffix")

# Libretro cores are named <core>_libretro.so with no "lib" prefix.
set_target_properties(pcsx2-libretro PROPERTIES
	OUTPUT_NAME "${ARMSX2_LIBRETRO_CORE_NAME}_libretro"
	PREFIX ""
)

if (PACKAGE_MODE)
	install(TARGETS pcsx2-libretro DESTINATION ${CMAKE_INSTALL_LIBDIR})
else()
	install(TARGETS pcsx2-libretro DESTINATION ${CMAKE_SOURCE_DIR}/bin)
endif()

target_sources(pcsx2-libretro PRIVATE
	Main.cpp
)

target_include_directories(pcsx2-libretro PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
	"${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(pcsx2-libretro PRIVATE
	PCSX2_FLAGS
	PCSX2
	libretro-headers
)

# A shared core must be fully position-independent — the pcsx2-sdl ET_EXEC
# fixed-address trick (persisted-JIT VU cache) does not apply here.
set_target_properties(pcsx2-libretro PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Only export the retro_* entry points; PCSX2's internal symbols must not
# leak into (or collide with) the frontend process.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_link_options(pcsx2-libretro PRIVATE
		"-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/link.T")
	set_target_properties(pcsx2-libretro PROPERTIES
		LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/link.T")
endif()

# The libretro buildbot's ci-templates build the target named
# <CORENAME>_libretro (see .gitlab-ci.yml), so the alias has to track the core
# name rather than be spelled out.
add_custom_target(${ARMSX2_LIBRETRO_CORE_NAME}_libretro)
add_dependencies(${ARMSX2_LIBRETRO_CORE_NAME}_libretro pcsx2-libretro)
