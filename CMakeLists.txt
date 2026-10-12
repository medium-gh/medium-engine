cmake_minimum_required(VERSION 3.25)
project(MediumEngine VERSION 0.1.0 LANGUAGES C CXX)

# Built on its own, the engine builds its tests and example too. Added to another project with
# add_subdirectory(), it builds its libraries and leaves the rest off unless asked.
if(PROJECT_IS_TOP_LEVEL)
 set(CMAKE_CXX_STANDARD 20)
 set(CMAKE_CXX_STANDARD_REQUIRED ON)
 set(CMAKE_CXX_EXTENSIONS OFF)
 set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()
option(MEDIUM_GRAPHICS "Build the SDL_GPU renderer (medium_render)" ON)
option(MEDIUM_TESTS "Build the engine's tests" ${PROJECT_IS_TOP_LEVEL})
option(MEDIUM_EXAMPLES "Build the engine's examples" ${PROJECT_IS_TOP_LEVEL})
option(MEDIUM_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(MEDIUM_RELEASE_SIZE_OPTIMIZATIONS "Discard unused functions and data in Release binaries" ON)
# Where the compiled sprite shaders go. The renderer loads them from <assets>/shaders, so this
# is the `shaders` directory of whatever asset directory the application hands it.
set(MEDIUM_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/assets/shaders" CACHE PATH
    "Directory the compiled sprite shaders are written to")

if(PROJECT_IS_TOP_LEVEL AND MSVC AND MEDIUM_SANITIZERS)
 # MSVC ships AddressSanitizer but has no UndefinedBehaviorSanitizer. /RTC1, which CMake adds to
 # Debug by default, is incompatible with /fsanitize=address and is removed here.
 message(WARNING "MEDIUM_SANITIZERS on MSVC enables AddressSanitizer only.")
 string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
 string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()

include(cmake/Dependencies.cmake)
include(cmake/Target.cmake)
set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_VALIDATE ON CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(box2d json stb)

# ---- medium_core: scenes, animation, tiles, audio mixing, residency and profiling. No window, no GPU.
add_library(medium_core src/core.cpp src/residency.cpp src/audio.cpp src/audio_file.cpp
            src/profile.cpp src/tiles.cpp)
add_library(medium::core ALIAS medium_core)
target_include_directories(medium_core PUBLIC include)
# stb_vorbis for sound files, compiled into audio_file.cpp alone; a system include, so the
# library's own diagnostics stay the library's.
target_include_directories(medium_core SYSTEM PRIVATE ${stb_SOURCE_DIR})
target_link_libraries(medium_core PUBLIC nlohmann_json::nlohmann_json box2d)
medium_target(medium_core)

# ---- medium_render: the sorted, batched sprite renderer on SDL_GPU, and its shaders.
if(MEDIUM_GRAPHICS)
 set(SDL_SHARED OFF CACHE BOOL "" FORCE)
 set(SDL_STATIC ON CACHE BOOL "" FORCE)
 set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
 set(SDL_TESTS OFF CACHE BOOL "" FORCE)
 FetchContent_MakeAvailable(sdl)
 # The Vulkan SDK's Bin directory may not be on PATH in a shell opened before it was installed.
 find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang HINTS ENV VULKAN_SDK
              PATH_SUFFIXES Bin bin REQUIRED)
 file(MAKE_DIRECTORY ${MEDIUM_SHADER_OUTPUT_DIR})
 foreach(stage vert frag)
  add_custom_command(OUTPUT ${MEDIUM_SHADER_OUTPUT_DIR}/sprite.${stage}.spv
   COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.0 ${CMAKE_CURRENT_SOURCE_DIR}/shaders/sprite.${stage}
           -o ${MEDIUM_SHADER_OUTPUT_DIR}/sprite.${stage}.spv
   DEPENDS shaders/sprite.${stage} VERBATIM)
 endforeach()
 add_custom_target(medium_shaders DEPENDS ${MEDIUM_SHADER_OUTPUT_DIR}/sprite.vert.spv
                                          ${MEDIUM_SHADER_OUTPUT_DIR}/sprite.frag.spv)
 add_library(medium_render src/render.cpp)
 add_library(medium::render ALIAS medium_render)
 # SYSTEM so third-party header warnings are not attributed to our /W4 -Wall builds.
 target_include_directories(medium_render SYSTEM PRIVATE ${stb_SOURCE_DIR})
 target_link_libraries(medium_render PUBLIC medium_core SDL3::SDL3)
 medium_target(medium_render)
endif()

if(MEDIUM_TESTS)
 enable_testing()
 add_subdirectory(tests)
endif()
if(MEDIUM_EXAMPLES AND MEDIUM_GRAPHICS)
 add_subdirectory(examples)
endif()
