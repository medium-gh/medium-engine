#pragma once
#include "medium/core.hpp"
#include <span>
#include <string_view>
#include <vector>
namespace med {
// Tile terrain: what a room's tile maps (`Scene::tiles`) make of the room as played.
//
// A tile map is a grid of cells over the room, each empty or one tile of a tileset image. Hand-placed
// walls stop scaling long before a cave does - a ragged cave wall is dozens of rectangles to drag out
// one by one - so the cells whose tile is marked solid become walls, and those marked as platforms
// one-way platforms, **merged into as few rectangles as cover them**. Those walls are ordinary objects
// in the room as played - the mover, the navigation graph, the route query, the cooker's checks all see
// them - and are never written back into the room file, as prefab contents never are.

// A generated object's ID starts with this. The room's own objects may not use it.
inline constexpr std::string_view tile_object_prefix = "~tiles.";
inline bool from_tiles(const Object &object) { return object.id.starts_with(tile_object_prefix); }

// The cell (column, row) containing a world point, or nothing when it is off the map.
std::optional<std::pair<int, int>> tile_cell(const TileMap &map, Vec2 world);
// The walls and platforms `map` (the `index`th of its room) makes: solid cells merged greedily into
// rectangles - as wide as a run goes, then as deep as the same run repeats below - and platform cells
// merged along each row. Every solid cell is covered by exactly one wall and nothing else is.
std::vector<Object> tile_objects(const TileMap &map, size_t index);
// Every tile map's objects, in map order.
std::vector<Object> tile_objects(const Scene &room);

// The cells a straight stroke from `from` to `to` crosses, `to` included and `from` not: a brush moved
// faster than a cell a frame still paints every cell it passed over.
std::vector<std::pair<int, int>> cells_between(std::pair<int, int> from, std::pair<int, int> to);
// `map` with a grid of `columns` by `rows`, keeping every cell that still fits where it was.
TileMap resized(const TileMap &map, int columns, int rows);
// A tile map's cells as run-length pairs, [tile, count, tile, count, ...], the form the room file keeps
// them in; and back, refusing a run that does not fill the grid exactly.
Json tile_cells_json(const TileMap &map);
std::vector<uint16_t> tile_cells_from_json(const Json &runs, int columns, int rows);
} // namespace med
