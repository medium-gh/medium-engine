#include "medium/core.hpp"
#include <algorithm>
#include <bit>
#include <iostream>
#include <limits>

using namespace med;

namespace {
bool exact(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }
bool exact(Vec2 a, Vec2 b) { return exact(a.x, b.x) && exact(a.y, b.y); }
bool exact(const MoveResult &a, const MoveResult &b) {
    return exact(a.position, b.position) && a.floor == b.floor && a.ceiling == b.ceiling &&
           a.wall == b.wall && exact(a.floor_normal, b.floor_normal) && exact(a.wall_normal, b.wall_normal) &&
           a.floor_object == b.floor_object && a.wall_object == b.wall_object;
}
// A fixed integer generator keeps every failing oracle fixture reproducible across standard libraries.
struct Random {
    uint32_t state = 0x4d454449u;
    uint32_t next() { return state = state * 1664525u + 1013904223u; }
    int integer(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1)); }
    float quarter(int lo, int hi) { return static_cast<float>(integer(lo, hi)) * 0.25f; }
};
} // namespace

int main() {
    size_t checks = 0;
    try {
        auto check = [&](bool pass, const std::string &message) {
            ++checks;
            if (!pass)
                throw std::runtime_error(message);
        };
        auto compare = [&](const std::vector<Object> &objects, const SolidIndex &index, Vec2 origin,
                           Vec2 delta, float radius, float half, bool through, const std::string &context) {
            const auto full = sweep_move(origin, delta, radius, half, objects, through);
            const auto indexed = sweep_move(origin, delta, radius, half, objects, index, through);
            check(exact(full, indexed),
                  context + ": indexed sweep differs from full scan (position " +
                      std::to_string(full.position.x) + "," + std::to_string(full.position.y) + " versus " +
                      std::to_string(indexed.position.x) + "," + std::to_string(indexed.position.y) + ")");
            return full;
        };

        // The first wall pushes across many bands, then every later narrow wall extends that push.
        // A candidate list captured at the start of a pass cannot finish this cascade in eight passes.
        std::vector<Object> cascade{{"first", "wall", {0, 0}, {200, 2000}, ""}};
        for (int n = 1; n <= 64; ++n)
            cascade.push_back({"wall." + std::to_string(n),
                               "wall",
                               {-100.0f - static_cast<float>(n) + 0.5f, 0},
                               {1, 2000},
                               ""});
        const auto pushed =
            compare(cascade, index_solids(cascade, 4), {}, {}, 1, 0, false, "ordered penetration cascade");
        check(pushed.position.x < -164, "The oracle traverses the entire forward cascade in one pass");
        std::reverse(cascade.begin(), cascade.end());
        const auto reversed =
            compare(cascade, index_solids(cascade, 4), {}, {}, 1, 0, false, "reverse penetration cascade");
        check(reversed.position.x > -104 && reversed.position.x < -100,
              "Reverse order retains the full scan's eight-pass limit, with no earlier-index revisits");
        std::rotate(cascade.begin(), cascade.begin() + 17, cascade.end());
        compare(cascade, index_solids(cascade, 4), {}, {0, 0.5f}, 1, 20, false,
                "rotated penetration cascade");

        std::vector<Object> room{{"floor.0", "wall", {0, 30}, {800, 60}, ""},
                                 {"floor.1", "wall", {0, 30}, {800, 60}, ""},
                                 {"pillar", "wall", {100, -100}, {40, 200}, ""},
                                 {"plank", "platform", {-100, -80}, {120, 12}, ""},
                                 {"decor", "decoration", {0, -40}, {300, 300}, ""}};
        const auto index = index_solids(room, 16);
        const auto floor =
            compare(room, index, {0, -100}, {0, 200}, 12, 20, false, "equal-fraction contact order");
        check(floor.floor && floor.floor_object == 0,
              "Equal-fraction contacts retain the first original scene index");
        compare(room, index, {0, -100}, {200, 0}, 12, 20, false, "wall contact and normal");
        compare(room, index, {-100, -160}, {0, 220}, 12, 20, false, "one-way landing");
        compare(room, index, {-100, -160}, {0, 220}, 12, 20, true, "one-way drop through");
        compare(room, index, {-100, -20}, {0, -220}, 12, 20, false, "one-way upward passage");
        for (int band = -1; band <= static_cast<int>(index.starts.size()); ++band)
            for (const float epsilon : {-0.0001f, 0.0f, 0.0001f}) {
                const float x = index.left + static_cast<float>(band) * index.band + epsilon;
                for (const Vec2 delta : {Vec2{}, Vec2{0, 0.5f}, Vec2{32, 120}, Vec2{-32, 120}, Vec2{8192, 0}})
                    compare(room, index, {x, -32}, delta, 12, 20, false,
                            "band boundary " + std::to_string(band));
            }
        compare(room, index, {-20000, -32}, {0, 0.5f}, 12, 20, false, "outside every band");
        compare(room, index, {-20000, -32}, {40000, 0}, 12, 20, false, "long-range full-scan fallback");
        const std::vector<Object> empty;
        compare(empty, index_solids(empty), {12, 34}, {5, -7}, 12, 20, false, "empty index fallback");
        auto changed = room;
        changed.push_back({"new", "wall", {-240, -100}, {30, 200}, ""});
        compare(changed, index, {-300, -100}, {120, 0}, 12, 20, false, "increased count fallback");
        changed.erase(changed.begin(), changed.begin() + 2);
        compare(changed, index, {-300, -100}, {120, 0}, 12, 20, false, "decreased count fallback");
        auto unsupported = index;
        unsupported.band = 0;
        compare(room, unsupported, {0, -100}, {0, 200}, 12, 20, false, "zero band fallback");
        unsupported.band = std::numeric_limits<float>::quiet_NaN();
        compare(room, unsupported, {0, -100}, {0, 200}, 12, 20, false, "nonfinite band fallback");
        unsupported = index;
        unsupported.left = std::numeric_limits<float>::infinity();
        compare(room, unsupported, {0, -100}, {0, 200}, 12, 20, false, "nonfinite left fallback");
        unsupported = index;
        unsupported.starts.resize(1);
        compare(room, unsupported, {0, -100}, {0, 200}, 12, 20, false, "no bands fallback");
        unsupported = index;
        unsupported.band = std::numeric_limits<float>::min();
        compare(room, unsupported, {0, -100}, {0, 200}, 12, 20, false,
                "overflowed band coordinates use the full scan");
        compare(room, index, {std::numeric_limits<float>::max(), 0}, {}, 12, 20, false,
                "very large query wholly outside every band");

        // The public builder must refuse unsupported geometry or impractical subdivision before
        // converting floats to indices or allocating its band lists. Only safe geometry is swept.
        auto builder_fallback = [&](const std::vector<Object> &objects, float width,
                                    const std::string &context) {
            const auto built = index_solids(objects, width);
            check(built.objects == objects.size() && built.starts.empty() && built.ids.empty(),
                  context + ": builder should return an empty full-scan index");
            return built;
        };
        const auto tiny = builder_fallback(room, std::numeric_limits<float>::min(),
                                           "tiny band with overflowed computed count");
        compare(room, tiny, {0, -100}, {0, 200}, 12, 20, false, "tiny builder-band full-scan parity");
        builder_fallback(room, 0.0001f, "finite excessive computed band count");
        std::vector<Object> bounded{{"wide", "wall", {}, {4093, 16}, ""}};
        check(index_solids(bounded, 1).starts.size() == 4097,
              "The last supported 4096-band subdivision remains usable");
        bounded[0].size.x = 4094;
        builder_fallback(bounded, 1, "one beyond the supported band count");
        bounded[0].size.x = 4093;
        bounded.resize(1025, bounded.front());
        builder_fallback(bounded, 1, "excessive repeated entries despite a supported band count");
        for (const float invalid :
             {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
              -std::numeric_limits<float>::infinity()}) {
            for (int component = 0; component < 6; ++component) {
                std::vector<Object> bad{{"invalid", "wall", {}, {20, 20}, ""}};
                switch (component) {
                case 0:
                    bad[0].position.x = invalid;
                    break;
                case 1:
                    bad[0].position.y = invalid;
                    break;
                case 2:
                    bad[0].size.x = invalid;
                    break;
                case 3:
                    bad[0].size.y = invalid;
                    break;
                case 4:
                    bad[0].travel.x = invalid;
                    break;
                default:
                    bad[0].travel.y = invalid;
                    break;
                }
                builder_fallback(bad, 128, "nonfinite solid bounds component " + std::to_string(component));
            }
        }
        for (const float invalid : {0.0f, -1.0f})
            for (const bool horizontal : {false, true}) {
                std::vector<Object> bad{{"invalid-size", "wall", {}, {20, 20}, ""}};
                (horizontal ? bad[0].size.x : bad[0].size.y) = invalid;
                builder_fallback(bad, 128, "nonpositive solid size");
            }
        const float largest = std::numeric_limits<float>::max();
        std::vector<Object> overflow{{"overflow", "wall", {largest, 0}, {20, 20}, ""}};
        overflow[0].travel.x = largest;
        builder_fallback(overflow, 128, "finite coordinates overflow their travel extent");
        const std::vector<Object> wide_span{{"negative", "wall", {-largest, 0}, {20, 20}, ""},
                                            {"positive", "wall", {largest, 0}, {20, 20}, ""}};
        builder_fallback(wide_span, 128, "finite bounds overflow their combined span");

        // Keep one index from the authored homes through complete positive/negative travel cycles.
        std::vector<Object> authored{{"floor", "wall", {0, 600}, {2000, 60}, ""},
                                     {"right", "wall", {-350, 0}, {64, 24}, ""},
                                     {"left", "platform", {250, -80}, {80, 12}, ""},
                                     {"lift", "wall", {50, 100}, {40, 18}, ""}};
        authored[1].travel = {480, 60};
        authored[1].period = 2;
        authored[2].travel = {-480, 0};
        authored[2].period = 3;
        authored[3].travel = {0, -240};
        authored[3].period = 4;
        const auto moving_index = index_solids(authored, 64);
        auto moving = authored;
        for (uint64_t tick = 0; tick <= 480; ++tick) {
            for (size_t n = 1; n < authored.size(); ++n) {
                moving[n].position = authored[n].position + travel_offset(authored[n], tick);
                const Vec2 top = moving[n].position - Vec2{0, moving[n].size.y / 2 + 32};
                compare(moving, moving_index, top, {4, 0.5f}, 12, 20, false,
                        "moving travel tick " + std::to_string(tick));
                compare(moving, moving_index, moving[n].position, {}, 3, 0, false,
                        "moving penetration tick " + std::to_string(tick));
            }
        }

        Random random;
        for (int scene = 0; scene < 24; ++scene) {
            std::vector<Object> objects;
            for (int n = 0; n < 8 + scene * 3; ++n) {
                const int kind = random.integer(0, 5);
                objects.push_back({"random." + std::to_string(n),
                                   kind == 0   ? "decoration"
                                   : kind == 1 ? "platform"
                                               : "wall",
                                   {random.quarter(-2400, 2400), random.quarter(-1600, 1600)},
                                   {random.quarter(4, 800), random.quarter(4, 1600)},
                                   ""});
            }
            for (const float width : {32.0f, 64.0f, 128.0f, 257.0f}) {
                const auto random_index = index_solids(objects, width);
                for (int query = 0; query < 70; ++query) {
                    Vec2 origin{random.quarter(-2800, 2800), random.quarter(-2000, 2000)};
                    if (query % 2 == 0) {
                        const auto &inside = objects[static_cast<size_t>(
                            random.integer(0, static_cast<int>(objects.size()) - 1))];
                        origin = inside.position + Vec2{inside.size.x * random.quarter(-2, 2),
                                                        inside.size.y * random.quarter(-2, 2)};
                    }
                    Vec2 delta{random.quarter(-256, 256), random.quarter(-256, 256)};
                    if (query % 13 == 0)
                        delta.x *= 32;
                    if (query % 7 == 0)
                        delta = {};
                    const float radius = random.quarter(1, 80), half = random.quarter(0, 160);
                    for (const bool through : {false, true})
                        compare(objects, random_index, origin, delta, radius, half, through,
                                "random scene " + std::to_string(scene) + " query " + std::to_string(query) +
                                    " width " + std::to_string(width));
                }
            }
        }
        std::cout << "PASS " << checks << " exact SolidIndex oracle checks\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL after " << checks << " SolidIndex checks: " << e.what() << '\n';
        return 1;
    }
}
