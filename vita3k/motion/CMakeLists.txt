add_library(
	motion
	STATIC
	src/motion.cpp
	src/motion_input.cpp
)

target_include_directories(motion PUBLIC include)
if(BUILD_LIBRETRO)
	target_include_directories(motion PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../libretro)
	target_link_libraries(motion PUBLIC emuenv util)
else()
	target_link_libraries(motion PUBLIC emuenv SDL3::SDL3 util)
endif()
target_link_libraries(motion PRIVATE ctrl)
if(ANDROID)
    target_link_libraries(motion PRIVATE android)
endif()
