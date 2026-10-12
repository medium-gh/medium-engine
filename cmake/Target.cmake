# The warnings, checks and size options every target of the engine is built with. A project that
# builds on the engine can give its own targets the same by calling medium_target() on them.
set(MEDIUM_ENGINE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." CACHE INTERNAL "Where the engine's sources are")
function(medium_target target)
 if(MSVC)
  # /utf-8 stops MSVC reading these UTF-8 sources as the machine's ANSI code page.
  target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  if(MEDIUM_RELEASE_SIZE_OPTIMIZATIONS)
   target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/Gy> $<$<CONFIG:Release>:/Gw>)
   target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
  endif()
  # Every test executable reports a failed debug-runtime check to stderr instead of waiting on a
  # dialog nobody will click (tests/support/quiet_asserts.cpp).
  get_target_property(kind ${target} TYPE)
  if(kind STREQUAL "EXECUTABLE" AND target MATCHES "_tests$")
   target_sources(${target} PRIVATE ${MEDIUM_ENGINE_DIR}/tests/support/quiet_asserts.cpp)
  endif()
  if(MEDIUM_SANITIZERS)
   target_compile_options(${target} PUBLIC /fsanitize=address)
  endif()
 else()
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror=return-type)
  # libstdc++'s own checks - an empty optional dereferenced, an iterator past its end - in every
  # unoptimised build, whatever the compiler's default. GCC 15 turned them on for -O0 and GCC 13
  # (the CI and cloud compiler) does not, which is how two tests passed there by undefined
  # behaviour and failed here. No ABI change: this is not _GLIBCXX_DEBUG.
  target_compile_definitions(${target} PRIVATE $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)
  if(MEDIUM_RELEASE_SIZE_OPTIMIZATIONS)
   target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-ffunction-sections> $<$<CONFIG:Release>:-fdata-sections>)
   if(APPLE)
    target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-Wl,-dead_strip>)
   else()
    target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-Wl,--gc-sections>)
   endif()
  endif()
  if(MEDIUM_SANITIZERS)
   # Modest optimisation makes whole-character validation practical while retaining Debug
   # symbols/assertions and all sanitizer instrumentation. Keep useful error call stacks and
   # fail on the first UB report rather than allowing CTest to record a recovering process as OK.
   target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:-O1> -fno-optimize-sibling-calls)
   target_compile_options(${target} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer
                          -fno-sanitize-recover=undefined)
   target_link_options(${target} PUBLIC -fsanitize=address,undefined)
  endif()
 endif()
endfunction()
