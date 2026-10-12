#include "medium/tiles.hpp"
#include <algorithm>
namespace med {
namespace {
bool listed(std::span<const uint16_t> tiles, uint16_t tile) {
    return tile != 0 && std::find(tiles.begin(), tiles.end(), tile) != tiles.end();
}
} // namespace

std::optional<std::pair<int, int>> tile_cell(const TileMap &map, Vec2 world) {
    if (!std::isfinite(map.cell) || map.cell <= 0)
        return std::nullopt;
    // Check the floating coordinates before converting: a pointer far off the map, or a
    // non-finite point, must not overflow the conversion to an integer cell.
    const double column = (static_cast<double>(world.x) - map.origin.x) / map.cell;
    const double row = (static_cast<double>(world.y) - map.origin.y) / map.cell;
    if (!(column >= 0 && row >= 0 && column < map.columns && row < map.rows))
        return std::nullopt;
    return std::pair{static_cast<int>(column), static_cast<int>(row)};
}

std::vector<Object> tile_objects(const TileMap &map, size_t index) {
    std::vector<Object> out;
    const auto columns = static_cast<size_t>(std::max(0, map.columns)),
               rows = static_cast<size_t>(std::max(0, map.rows));
    if (map.cells.size() != columns * rows)
        return out;
    auto at = [&](size_t c, size_t r) { return map.cells[r * columns + c]; };
    const std::string prefix = std::string(tile_object_prefix) + std::to_string(index) + ".";
    auto emit = [&](const char *kind, size_t c, size_t r, size_t w, size_t h) {
        Object o;
        o.id = prefix + std::to_string(out.size());
        o.kind = kind;
        o.size = {static_cast<float>(w) * map.cell, static_cast<float>(h) * map.cell};
        o.position = {map.origin.x + static_cast<float>(c) * map.cell + o.size.x / 2,
                      map.origin.y + static_cast<float>(r) * map.cell + o.size.y / 2};
        out.push_back(std::move(o));
    };
    // No rectangle may be longer than an object may be: runs are cut at the object size limit.
    const auto most =
        static_cast<size_t>(std::max(1.0f, std::floor(limits::max_size / std::max(map.cell, 1.0f))));
    // Solid: the widest run from each uncovered solid cell, then as many rows below as repeat it whole.
    std::vector<bool> covered(columns * rows, false);
    for (size_t r = 0; r < rows; ++r)
        for (size_t c = 0; c < columns; ++c) {
            if (covered[r * columns + c] || !listed(map.solid, at(c, r)))
                continue;
            size_t w = 1;
            while (c + w < columns && w < most && !covered[r * columns + c + w] &&
                   listed(map.solid, at(c + w, r)))
                ++w;
            size_t h = 1;
            for (bool whole = true; whole && r + h < rows && h < most;) {
                for (size_t x = c; x < c + w && whole; ++x)
                    whole = !covered[(r + h) * columns + x] && listed(map.solid, at(x, r + h));
                if (whole)
                    ++h;
            }
            for (size_t y = r; y < r + h; ++y)
                for (size_t x = c; x < c + w; ++x)
                    covered[y * columns + x] = true;
            emit("wall", c, r, w, h);
        }
    // One-way platforms: runs along a row, never merged downward - each row is its own surface.
    for (size_t r = 0; r < rows; ++r)
        for (size_t c = 0; c < columns;) {
            if (!listed(map.platforms, at(c, r))) {
                ++c;
                continue;
            }
            size_t w = 1;
            while (c + w < columns && w < most && listed(map.platforms, at(c + w, r)))
                ++w;
            emit("platform", c, r, w, 1);
            c += w;
        }
    return out;
}

std::vector<Object> tile_objects(const Scene &room) {
    std::vector<Object> out;
    for (size_t i = 0; i < room.tiles.size(); ++i) {
        auto made = tile_objects(room.tiles[i], i);
        out.insert(out.end(), std::make_move_iterator(made.begin()), std::make_move_iterator(made.end()));
    }
    return out;
}

std::vector<std::pair<int, int>> cells_between(std::pair<int, int> from, std::pair<int, int> to) {
    std::vector<std::pair<int, int>> out;
    // Bresenham's line, stepping one cell at a time from `from` towards `to`.
    int x = from.first, y = from.second;
    const int dx = std::abs(to.first - x), dy = -std::abs(to.second - y);
    const int sx = x < to.first ? 1 : -1, sy = y < to.second ? 1 : -1;
    int error = dx + dy;
    while (x != to.first || y != to.second) {
        const int twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x += sx;
        }
        if (twice <= dx) {
            error += dx;
            y += sy;
        }
        out.emplace_back(x, y);
    }
    return out;
}
TileMap resized(const TileMap &map, int columns, int rows) {
    auto next = map;
    next.columns = std::max(0, columns);
    next.rows = std::max(0, rows);
    next.cells.assign(static_cast<size_t>(next.columns) * static_cast<size_t>(next.rows), 0);
    for (int r = 0; r < std::min(map.rows, next.rows); ++r)
        for (int c = 0; c < std::min(map.columns, next.columns); ++c) {
            const auto from =
                static_cast<size_t>(r) * static_cast<size_t>(map.columns) + static_cast<size_t>(c);
            if (from < map.cells.size())
                next.cells[static_cast<size_t>(r) * static_cast<size_t>(next.columns) +
                           static_cast<size_t>(c)] = map.cells[from];
        }
    return next;
}
Json tile_cells_json(const TileMap &map) {
    Json runs = Json::array();
    for (size_t i = 0; i < map.cells.size();) {
        size_t n = 1;
        while (i + n < map.cells.size() && map.cells[i + n] == map.cells[i])
            ++n;
        runs.push_back(map.cells[i]);
        runs.push_back(n);
        i += n;
    }
    return runs;
}
std::vector<uint16_t> tile_cells_from_json(const Json &runs, int columns, int rows) {
    if (!runs.is_array() || runs.size() % 2 != 0 || columns < 0 || rows < 0)
        throw std::runtime_error("Tile cells are [tile, count, tile, count, ...]");
    const auto total = static_cast<size_t>(columns) * static_cast<size_t>(rows);
    std::vector<uint16_t> cells;
    cells.reserve(total);
    for (size_t i = 0; i < runs.size(); i += 2) {
        const auto tile = runs.at(i).get<int64_t>(), count = runs.at(i + 1).get<int64_t>();
        if (tile < 0 || tile > limits::max_tile || count < 1 ||
            static_cast<size_t>(count) > total - cells.size())
            throw std::runtime_error("A tile run names a tile past " + std::to_string(limits::max_tile) +
                                     " or runs past the grid");
        cells.insert(cells.end(), static_cast<size_t>(count), static_cast<uint16_t>(tile));
    }
    if (cells.size() != total)
        throw std::runtime_error("Tile cells do not fill the grid: " + std::to_string(cells.size()) + " of " +
                                 std::to_string(total));
    return cells;
}
} // namespace med
