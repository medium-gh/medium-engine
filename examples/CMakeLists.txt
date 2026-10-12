# The renderer loads its shaders and font from one asset directory; this puts the example's font
# beside the shaders the build compiles.
get_filename_component(medium_example_assets "${MEDIUM_SHADER_OUTPUT_DIR}" DIRECTORY)
add_custom_command(OUTPUT ${medium_example_assets}/font.ttf
 COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/assets/font.ttf
         ${medium_example_assets}/font.ttf
 DEPENDS assets/font.ttf VERBATIM)
add_custom_target(medium_example_assets DEPENDS ${medium_example_assets}/font.ttf)
add_executable(medium_hello hello.cpp)
target_link_libraries(medium_hello PRIVATE medium_render)
medium_target(medium_hello)
add_dependencies(medium_hello medium_shaders medium_example_assets)
