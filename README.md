# medium-engine

A small 2D engine for side-view action games, written in C++20. It is the engine layer of
[MEDIAN](https://mediumdev.com/games/median), a game in development at MEDIUM, published on its
own so it can be read, built and used.

It is early. The API changes when the game needs it to, and nothing here is versioned for
stability yet.

## What is in it

Three libraries, each usable without the ones after it:

| Library         | Header                                | What it does                                                                                                   |
| --------------- | ------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| `medium_core`   | `medium/core.hpp`                     | Vectors and rectangles, a fixed-step clock, JSON files written atomically, UTF-8, and the content model: rooms (`Scene`), objects, prefabs, animations, camera and wind zones, with loading, saving and validation |
|                 | `medium/tiles.hpp`                    | Tile maps, and the walls and one-way platforms they become, merged into as few rectangles as cover them        |
|                 | `medium/audio.hpp`, `audio_file.hpp`  | A mixer with buses, a voice cap, fades and positional placement that allocates nothing while rendering; WAV and Ogg Vorbis loading |
|                 | `medium/residency.hpp`                | Which assets are loaded, what each is built from, and staged hot reload that never swaps mid-frame             |
|                 | `medium/profile.hpp`                  | Where a frame's time goes, per phase, with percentiles over a rolling window                                   |
| `medium_render` | `medium/render.hpp`                   | A window and a sorted, batched sprite renderer on SDL_GPU: textures and atlases, text, blend modes, clipping, several views of one canvas, frame capture |
| `medium_net`    | `medium/network.hpp`                  | Reliable and unreliable messages between a server and its clients over GameNetworkingSockets                   |

`medium_core` has no window and no GPU, so everything in it can be tested headless.

## What is not in it yet

MEDIAN's level, animation and world editor, and its runtime systems (simulation, snapshots and
reconciliation, camera, cutscenes, navigation, input) are still part of the game's own code. They
will move here as they are separated from it.

## Building

You need CMake 3.25 or newer, a C++20 compiler (GCC 13, Clang 17 or MSVC 2022) and Ninja. The
renderer also needs `glslangValidator` on your `PATH` (it ships with the Vulkan SDK; on Debian and
Ubuntu it is the `glslang-tools` package).

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
./build/debug/examples/medium_hello build/debug/assets
```

Dependencies are downloaded at configure time, each pinned by commit and SHA-256
(`cmake/Dependencies.cmake`). Nothing else is fetched.

| Preset    | What it builds                                             |
| --------- | ---------------------------------------------------------- |
| `debug`   | Everything but networking, with tests and the example      |
| `release` | The same, optimised                                        |
| `core`    | `medium_core` and its tests only: no SDL, no shader compiler |
| `asan`    | `core` under AddressSanitizer and UndefinedBehaviorSanitizer |
| `network` | `core` plus `medium_net` (needs OpenSSL and protobuf)      |

## Using it in a project

```cmake
include(FetchContent)
FetchContent_Declare(medium_engine
  GIT_REPOSITORY https://github.com/medium-gh/medium-engine.git
  GIT_TAG        <a commit>)
FetchContent_MakeAvailable(medium_engine)

add_executable(game main.cpp)
target_link_libraries(game PRIVATE medium::render)
add_dependencies(game medium_shaders)
```

The renderer loads `shaders/sprite.vert.spv`, `shaders/sprite.frag.spv` and `font.ttf` from the
asset directory you give it. The build writes the shaders to `MEDIUM_SHADER_OUTPUT_DIR`
(`<build>/assets/shaders` by default); the font is yours to supply.
[`examples/hello.cpp`](examples/hello.cpp) is a complete program.

## Layout

```
include/medium/   public headers
src/              one source file per header
shaders/          the sprite pipeline's vertex and fragment shaders (GLSL)
tests/            one executable per suite; none opens a window
examples/         hello.cpp, and the font it draws with
cmake/            pinned dependencies and shared compiler settings
docs/             how the pieces fit together
```

[`docs/OVERVIEW.md`](docs/OVERVIEW.md) explains the design in more detail.

## Licence

MIT. See [LICENSE](LICENSE). The libraries it builds on, and the font the example uses, keep their
own licences: see [THIRD_PARTY.md](THIRD_PARTY.md).
