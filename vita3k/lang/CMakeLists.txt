add_library(
	lang
	STATIC
	include/lang/state.h
	include/lang/functions.h
	src/lang.cpp
)

target_include_directories(lang PUBLIC include)
target_link_libraries(lang PUBLIC emuenv compat ime dialog)
if(NOT BUILD_LIBRETRO)
	target_link_libraries(lang PRIVATE config gui pugixml::pugixml util)
else()
	target_link_libraries(lang PRIVATE config pugixml::pugixml util)
endif()
