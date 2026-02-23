add_library(
	camera
	STATIC
	src/camera.cpp
)

target_include_directories(camera PUBLIC include)
if(NOT BUILD_LIBRETRO)
	target_link_libraries(camera PUBLIC SDL3::Headers)
endif()
target_link_libraries(camera PRIVATE config rtc stb util)
