add_library(
	touch
	STATIC
	include/touch/functions.h
	include/touch/state.h
	include/touch/touch.h
	src/touch.cpp
)

target_include_directories(touch PUBLIC include)
target_link_libraries(touch PUBLIC emuenv)
if(BUILD_LIBRETRO)
	target_include_directories(touch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../libretro)
	target_link_libraries(touch PRIVATE display)
else()
	target_link_libraries(touch PRIVATE display SDL3::SDL3)
endif()
if(ANDROID)
	target_link_libraries(touch PRIVATE android)
endif()
