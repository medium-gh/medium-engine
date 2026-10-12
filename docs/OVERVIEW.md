# How medium-engine fits together

The engine is three libraries stacked in one direction: `medium_render` and `medium_net` are built
on `medium_core`, and `medium_core` depends on neither. Nothing in the engine knows about any
particular game.

```
your game ──► medium_render ──► medium_core ──► Box2D, JSON for Modern C++, stb
          └─► medium_net ─────┘
```

Everything is in the namespace `med`.

## medium_core

No window, no GPU, no threads of its own. Everything here can be run in a test.

**Basics** (`core.hpp`). `Vec2`, `Rect`, generational `Handle`s, a `FixedClock` that turns real
time into whole simulation ticks, logging, and file helpers: `read_json` reports the file and the
reason when it fails, and `atomic_write` writes beside the target and renames over it, so a crash
never leaves half a file.

**The content model** (`core.hpp`). A room is a `Scene`: objects, prefab instances, camera zones,
wind zones, cutscene triggers, backdrops, point lights and tile maps. An `Animation` is clips of
frames with events, attachments and atlas regions. Each has a `_from_json` and a `_json`, and a
`validate_` that throws with a message naming what is wrong and where. Sizes are bounded by the
constants in `med::limits`, so a file cannot ask for more than the engine will hold.

**Queries** (`core.hpp`). A `SolidIndex` (`index_solids`) buckets a room's solids by column so a sweep or an overlap
test visits only the ones it could touch; shape tests go through Box2D.

**Tiles** (`tiles.hpp`). A tile map is a grid of cells. `tile_objects` turns the solid cells into
walls and the platform cells into one-way platforms, merged greedily into as few rectangles as
cover them. Those are ordinary objects to everything else and are never written back to the file.

**Audio** (`audio.hpp`, `audio_file.hpp`). `Mixer` has four buses, at most 32 voices, fades and
positional gain. Its `render` runs on the audio driver's thread, so it allocates nothing, locks
nothing and touches no file; the audio suite checks that by counting allocations across a render.
`decode_sound` reads WAV and Ogg Vorbis into a `Sample`; `open_music` streams a track as it plays.

**Residency** (`residency.hpp`). `AssetGraph` records what each asset is built from and depends
on. `ResidencyTable` loads and validates a new version of a changed asset while the old one is
still in use, and swaps at a moment the caller chooses, so a reload never happens mid-frame and a
bad file never replaces a good one.

**Profiling** (`profile.hpp`). `Profiler` attributes each frame's time to phases (input,
simulation, draw, submit, present and so on) and keeps percentiles over a rolling window, without
allocating. `against` compares a profile with a `FrameBudget` and says which phases went over.

## medium_render

`Renderer` owns the window and the SDL_GPU device. You queue sprites, rectangles and text during a
frame and call `present`, which sorts them by layer and depth, batches them by texture and blend
mode, and draws.

- **The canvas.** Everything is placed on a 1280 by 720 canvas, whatever size the window is.
  `screen_to_canvas` and `canvas_to_screen` convert pointer positions.
- **Sprites.** A `Sprite` is a textured rectangle with a UV rectangle, colour, layer, depth, angle,
  an optional clip rectangle and a blend: normal, additive or multiply.
- **Textures.** Load a file, upload pixels, or create one to rewrite each frame; several images can
  be packed into one atlas. A texture can be marked as pixel art to keep hard edges at any scale.
- **Text.** Two faces from TrueType files, with kerning, wrapping and shrink-to-fit, covering
  ASCII, Latin-1 and Latin Extended-A.
- **Views.** Up to three views of the canvas can be drawn in one frame, each to its own rectangle
  of the window or to a texture a UI can show. An editor uses this to draw an authoring view and
  the running game side by side.
- **Capture.** `capture` writes the next frame to a PNG, which is how the renderer is tested.

It loads `shaders/sprite.vert.spv`, `shaders/sprite.frag.spv` and `font.ttf` from the asset
directory given to its constructor. The shaders are compiled from `shaders/` by the build.

## medium_net

`Endpoint` is one end of a connection over GameNetworkingSockets: a server that accepts peers, or
a client that connects to one. `send` is reliable or unreliable per message, `poll` returns what
arrived, and lag and loss can be injected for testing. Messages are bounded by
`max_message_bytes`. It is a transport only; what a game sends is the game's business.

## Conventions

- One header in `include/medium/` has one source file in `src/` and, where it can be tested
  headless, one suite in `tests/`.
- Errors are exceptions carrying a message a person can act on. Loaders name the file and the
  field.
- Comments say why something is the way it is. Where a comment tells a story about an earlier
  mistake, it is there because the mistake is easy to make again.
- Formatting is `clang-format` with the file at the repository root.
