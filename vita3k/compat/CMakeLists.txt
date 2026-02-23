add_library(
	compat
	STATIC
	include/compat/functions.h
	include/compat/state.h
	src/compat.cpp
)

target_include_directories(compat PUBLIC include)
if(NOT BUILD_LIBRETRO)
	target_link_libraries(compat PUBLIC gui emuenv)
else()
	target_link_libraries(compat PUBLIC emuenv dialog)
endif()
target_link_libraries(compat PRIVATE config pugixml::pugixml miniz util)
