include(FetchContent)
# Every archive is pinned by commit and SHA256. The hash is checked whichever way the archive
# arrives, so a mirror can make a build possible where codeload.github.com is unreachable but can
# never make it build something else.
#
# MEDIUM_DEPENDENCY_MIRROR (cache variable, or the environment variable of the same name) names a
# directory holding `<name>-<commit>.tar.gz`: byte for byte the archive codeload serves.
set(MEDIUM_DEPENDENCY_MIRROR "" CACHE PATH
    "Directory of pinned dependency archives to use instead of codeload.github.com")
# The environment is read on every configure, not only the first: a build directory configured
# once without the variable would otherwise have cached it empty and ignored it from then on.
set(medium_mirror "${MEDIUM_DEPENDENCY_MIRROR}")
if(NOT medium_mirror AND DEFINED ENV{MEDIUM_DEPENDENCY_MIRROR})
 set(medium_mirror "$ENV{MEDIUM_DEPENDENCY_MIRROR}")
endif()
function(medium_dependency name repository commit sha256)
 if(medium_mirror)
  set(url "file://${medium_mirror}/${name}-${commit}.tar.gz")
 else()
  set(url "https://codeload.github.com/${repository}/tar.gz/${commit}")
 endif()
 FetchContent_Declare(${name} URL ${url} URL_HASH SHA256=${sha256} DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endfunction()
medium_dependency(sdl libsdl-org/SDL 7f3ae3d57459e59943a4ecfefc8f6277ec6bf540 7a8347c770b90b33daac2352858ca03f9c9a2ccc8ce711054870361d1a6b32e5)
medium_dependency(box2d erincatto/box2d 8c661469c9507d3ad6fbd2fea3f1aa71669c2fe3 dee7bcf0f50b3dcccf67678ce14ffe3ce02583b51b8b03e447ab30a84c3436ef)
medium_dependency(json nlohmann/json 65ee68451d8eb2b5f3a30b410476ab83deb3289b 13ef31d691947940a08909f8e0772f1d7d68e5da1678ee812a49c4bb0c996b2f)
medium_dependency(gns ValveSoftware/GameNetworkingSockets 2cb93a06350bb065db53abdb0d87cf297e0bfd34 826a55aba46b421d1198851f1a70ef63dca27f0bf91db23730321ae2f92d6294)
medium_dependency(stb nothings/stb 2c980bb59875b0d32144a71867fbdebb2f77cd20 9a955b1b49a4410088a2e0ee2a9c057c3c907d0c1d75454144cb980aca0ba515)
