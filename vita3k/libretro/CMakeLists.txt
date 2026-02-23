add_library(vita3k_libretro SHARED
	libretro_vita3k.cpp
	libretro_input.cpp
	libretro_installer.cpp
	libretro_log.cpp
	libretro_game_loader.cpp
	libretro_stb.cpp
)

target_include_directories(vita3k_libretro PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/external/concurrentqueue
)

# Link against Vita3K static libraries (excluding gui and gdbstub)
target_link_libraries(vita3k_libretro PRIVATE
	app
	audio
	config
	cppcommon
	ctrl
	display
	glad
	gxm
	io
	miniz
	modules
	motion
	packages
	patch
	psvpfsparser
	renderer
	shader
	touch
	util
)

# Output name: vita3k_libretro.dll / vita3k_libretro.so / vita3k_libretro.dylib
set_target_properties(vita3k_libretro PROPERTIES
	PREFIX ""
	OUTPUT_NAME "vita3k_libretro"
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(WIN32)
	target_link_libraries(vita3k_libretro PRIVATE Version)
	if(MSVC)
		target_link_options(vita3k_libretro PRIVATE
			"/MAP:$<TARGET_FILE_DIR:vita3k_libretro>/vita3k_libretro.map"
			"/MAPINFO:EXPORTS")
	endif()
endif()

if(LINUX)
	target_link_libraries(vita3k_libretro PRIVATE -static-libgcc -static-libstdc++)
endif()

# Copy data/lang/shaders to output dir
add_custom_command(
	TARGET vita3k_libretro
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../../data" "$<TARGET_FILE_DIR:vita3k_libretro>/data"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../../lang" "$<TARGET_FILE_DIR:vita3k_libretro>/lang"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../shaders-builtin" "$<TARGET_FILE_DIR:vita3k_libretro>/shaders-builtin"
)
