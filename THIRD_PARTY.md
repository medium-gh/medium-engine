# Third-party software

medium-engine downloads these at configure time, each pinned by commit and SHA-256 in
[`cmake/Dependencies.cmake`](cmake/Dependencies.cmake). None of their source is in this
repository. Each keeps its own licence, which applies to any binary you build with it.

| Library                | Used by                          | Licence                         | Source                                             |
| ---------------------- | -------------------------------- | ------------------------------- | -------------------------------------------------- |
| SDL 3                  | `medium_render`                  | zlib                            | https://github.com/libsdl-org/SDL                  |
| Box2D 3                | `medium_core` (shape queries)    | MIT                             | https://github.com/erincatto/box2d                 |
| JSON for Modern C++    | `medium_core`                    | MIT                             | https://github.com/nlohmann/json                   |
| stb (image, truetype, vorbis) | `medium_core`, `medium_render` | MIT or public domain         | https://github.com/nothings/stb                    |
| GameNetworkingSockets  | `medium_net` (optional)          | BSD 3-Clause                    | https://github.com/ValveSoftware/GameNetworkingSockets |

`medium_net` also links the OpenSSL and protobuf installed on the machine that builds it.

## Font

`examples/assets/font.ttf` is JetBrains Mono, copyright 2020 The JetBrains Mono Project Authors,
under the SIL Open Font License 1.1: [`licenses/JetBrainsMono-OFL-1.1.txt`](licenses/JetBrainsMono-OFL-1.1.txt).
It is there so the example has something to draw text with; the engine itself ships no font.
