#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace med {
using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr float tick_dt = 1.0f / 60.0f;
struct Vec2 {
    float x{}, y{};
    Vec2 operator+(Vec2 b) const { return {x + b.x, y + b.y}; }
    Vec2 operator-(Vec2 b) const { return {x - b.x, y - b.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2 &operator+=(Vec2 b) {
        x += b.x;
        y += b.y;
        return *this;
    }
    bool operator==(const Vec2 &) const = default;
};
inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float length(Vec2 v) { return std::sqrt(dot(v, v)); }
inline Vec2 normalized(Vec2 v) {
    float n = length(v);
    return n > 0.0001f ? v * (1 / n) : Vec2{};
}
inline Vec2 clamp_length(Vec2 v) {
    float n = length(v);
    return n > 1 ? v * (1 / n) : v;
}
inline bool finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
struct Rect {
    float x{}, y{}, w{}, h{};
    bool contains(Vec2 p) const { return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h; }
    bool operator==(const Rect &) const = default;
};
struct Handle {
    uint32_t index{}, generation{};
    bool operator==(const Handle &) const = default;
};
template <class T, size_t N> class Pool {
    struct Slot {
        std::optional<T> value;
        uint32_t generation = 1;
    };
    std::array<Slot, N> slots_{};

  public:
    Handle create(T value) {
        for (uint32_t i = 0; i < N; ++i)
            if (!slots_[i].value) {
                slots_[i].value = std::move(value);
                return {i, slots_[i].generation};
            }
        throw std::runtime_error("entity capacity exceeded");
    }
    T *get(Handle h) {
        return h.index < N && slots_[h.index].generation == h.generation && slots_[h.index].value
                   ? &*slots_[h.index].value
                   : nullptr;
    }
    const T *get(Handle h) const { return const_cast<Pool *>(this)->get(h); }
    bool destroy(Handle h) {
        if (!get(h))
            return false;
        slots_[h.index].value.reset();
        if (++slots_[h.index].generation == 0)
            ++slots_[h.index].generation;
        return true;
    }
    template <class F> void each(F f) {
        for (uint32_t i = 0; i < N; ++i)
            if (slots_[i].value)
                f(Handle{i, slots_[i].generation}, *slots_[i].value);
    }
    template <class F> void each(F f) const {
        for (uint32_t i = 0; i < N; ++i)
            if (slots_[i].value)
                f(Handle{i, slots_[i].generation}, *slots_[i].value);
    }

    // ---- Whole-pool state, for snapshot and restore ----
    //
    // Every slot, occupied or not. An empty slot's generation is part of the identity the next
    // create() hands out, so a restore that put back only the live entries would issue a handle
    // that collided with one from before it - which is exactly the bug a generation counter exists
    // to prevent. These are the only way in or out apart from create/destroy, deliberately: a pool
    // that can be written slot by slot from anywhere is a pool whose handles mean nothing.
    static constexpr size_t capacity = N;
    struct SlotState {
        bool occupied = false;
        uint32_t generation = 1;
    };
    SlotState slot(size_t index) const { return {slots_[index].value.has_value(), slots_[index].generation}; }
    const T *peek(size_t index) const { return slots_[index].value ? &*slots_[index].value : nullptr; }
    void restore_slot(size_t index, uint32_t generation, std::optional<T> value) {
        if (index >= N || generation == 0)
            throw std::runtime_error("Invalid pool slot restore");
        slots_[index] = {std::move(value), generation};
    }
};
class FixedClock {
    double accumulator_{};

  public:
    uint64_t dropped_ticks{};
    template <class F> int advance(double elapsed, F tick) {
        if (!std::isfinite(elapsed) || elapsed < 0)
            throw std::runtime_error("invalid elapsed time");
        accumulator_ += std::min(elapsed, 0.25);
        int count = 0;
        constexpr double step = 1.0 / 60.0;
        while (accumulator_ + 1e-12 >= step && count < 8) {
            tick();
            accumulator_ -= step;
            ++count;
        }
        if (accumulator_ >= step) {
            auto dropped = static_cast<uint64_t>(accumulator_ / step);
            dropped_ticks += dropped;
            accumulator_ -= static_cast<double>(dropped) * step;
        }
        return count;
    }
    float alpha() const { return static_cast<float>(std::max(0.0, accumulator_) * 60); }
    void reset() { accumulator_ = 0; }
};
enum class LogLevel { Info, Warning, Error };
void log(LogLevel level, const std::string &message);
// Bounded, so a hostile or corrupt file cannot ask for unbounded memory. 16 MiB: a save at every
// bound (thousands of flags, a thousand rooms of world facts) and a dialogue file carrying several
// languages both fit, which the old 1 MiB did not.
inline constexpr size_t max_json_bytes = 16 * 1024 * 1024;
Json read_json(const fs::path &path);
// Replaces `path` with `bytes` so that a reader, a crash or a second writer only ever sees one
// complete version. The previous contents, if any, are kept at `<path>.bak`, written the same way.
// Temporaries are unique to this process and call (`<path>.<pid>.<n>.tmp`), created exclusively
// and never through a link, and removed again when a write fails. Throws on any failure, leaving
// `path` as it was.
void atomic_write(const fs::path &path, const std::string &bytes);
// Removes temporaries `atomic_write` left beside `path` when a process died mid-write, including
// the older fixed `<path>.tmp`. For recovery, while the caller holds whatever lock keeps other
// writers out: it cannot tell a dead writer's temporary from a live one's.
void remove_temporaries(const fs::path &path);
// True when a single path component is safe and portable across the supported desktop
// platforms. Windows resolves reserved device names (CON, NUL, COM1...) regardless of
// directory, extension or letter case, and silently strips trailing dots and spaces, so those
// names are refused on every platform rather than producing content that only works on Linux.
bool portable_filename(std::string_view name);

// ---- Text ----
// The next Unicode code point of UTF-8 `text` at `at`, advancing `at` past it. A malformed or
// truncated sequence, an overlong form or a surrogate reads as U+FFFD and advances one byte, so a bad
// byte costs one replacement character and never the rest of the line.
char32_t next_codepoint(std::string_view text, size_t &at);
bool valid_utf8(std::string_view text);
void append_utf8(std::string &out, char32_t codepoint);
// `text` broken into lines no wider than `width` by `measure`: at spaces where it can, inside a word
// only where the word alone is too wide, and always at a newline. A width of zero or less breaks only
// at newlines.
std::vector<std::string> wrap_text(std::string_view text, float width,
                                   const std::function<float(std::string_view)> &measure);
// The traversal and action features an actor may be granted.
//
// EXTENSIBILITY's movement seam requires a validated capability set, and is specific about the
// direction of the failure: content names the features an actor has, and an unsupported name must
// **fail validation rather than doing nothing**. That is the whole point of the type. A room that
// asked for a capability this build does not implement and loaded anyway would produce an actor
// that silently cannot do what it was authored to do, and the defect would surface weeks later as
// a complaint that an enemy "feels wrong" rather than as a load error naming the file and the word.
//
// Only features that exist today are listed. A capability for something unimplemented would be a
// name content could pass validation with and still get nothing from, which is precisely the
// failure this set exists to prevent.
// The last four are traversal abilities. Content may take any
// of them away from an actor here, like any capability; whether the player has *learned* one is
// progression, held apart on the actor (`Actor::learned`), and both must say yes.
enum class Capability : uint8_t {
    Move,
    Jump,
    Attack,
    Dodge,
    Guard,
    Parry,
    Heal,
    Slide,
    Slam,
    Dash,
    WallJump,
    DoubleJump,
    LedgeGrab
};
inline constexpr size_t capability_count = 13;
// The capabilities that are learned rather than innate: a player starts without them.
inline constexpr std::array<Capability, 4> traversal_abilities{Capability::Dash, Capability::WallJump,
                                                               Capability::DoubleJump, Capability::LedgeGrab};
// In a stable order, for presentation and for round-tripping.
std::span<const std::string_view> capability_names();
std::string_view capability_name(Capability capability);
// Throws on a name this build does not implement. Content validation calls this.
Capability capability_from_name(std::string_view name);

// Which capabilities one actor has. A new set has all of them.
//
// Defaulting to "everything" makes an authored grant a subtraction rather than a checklist every
// scene has to restate, and means a capability added to this enum later does not silently switch
// itself off for every actor already authored.
class CapabilitySet {
    uint32_t bits_ = (1u << capability_count) - 1;

  public:
    constexpr CapabilitySet() = default;
    static constexpr CapabilitySet none() {
        CapabilitySet set;
        set.bits_ = 0;
        return set;
    }
    constexpr bool has(Capability capability) const {
        return (bits_ >> static_cast<uint32_t>(capability)) & 1u;
    }
    // True when nothing has been taken away. The scene writer uses it to leave the key out.
    constexpr bool complete() const { return bits_ == (1u << capability_count) - 1; }
    constexpr void grant(Capability capability) { bits_ |= 1u << static_cast<uint32_t>(capability); }
    constexpr void revoke(Capability capability) { bits_ &= ~(1u << static_cast<uint32_t>(capability)); }
    constexpr uint32_t bits() const { return bits_; }
    // Throws on a bit this build has no capability for, so a snapshot or a save written by a build
    // with more capabilities is refused rather than restored with the extra ones quietly dropped.
    static CapabilitySet from_bits(uint32_t bits);
    bool operator==(const CapabilitySet &) const = default;
};
// Content form: an array of capability names, in any order, without repeats. Both throw on
// anything this build cannot represent exactly.
CapabilitySet capabilities_from_json(const Json &value);
Json capabilities_to_json(const CapabilitySet &set);
struct Object {
    std::string id, kind;
    Vec2 position;
    Vec2 size{40, 40};
    std::string target;
    // Authoring layer. Purely an editor organisation aid: it does not affect simulation or draw
    // order. Omitted from the file when 0, so existing scenes round-trip byte-identically.
    int layer = 0;
    // What an actor spawned from this object may do. Same omission rule as `layer`: written only
    // when something has been taken away, so every scene authored before capabilities existed
    // round-trips byte-identically and no version bump is needed.
    CapabilitySet capabilities{};
    // Which way an actor spawned here starts facing: 1 right, -1 left. It became worth authoring
    // the moment enemies gained directional sight - a sentinel placed to guard an approach and
    // facing away from it cannot see anyone coming, and no amount of tuning fixes a guard looking
    // at a wall. Written only when it is not the default, by the same rule as `layer`.
    int facing = 1;
    // Which behaviour an enemy spawned here uses - one of `archetype_names()`; empty means the
    // default sentinel. Only an enemy or a boss may name one. Written only when set.
    std::string archetype{};
    // Half-width of the stretch an unaware enemy walks around its home, in world units; 0 stands
    // still. Only an enemy or a boss may patrol. Written only when set.
    float patrol = 0;
    // A moving solid: the offset to the far end of its travel, and the seconds a round trip
    // takes. It moves out and back along a straight line, a triangle wave in time, so where it is is
    // a function of the tick alone - nothing about it needs saving or snapshotting. Only a wall or a
    // platform may move. Written only when set, like every optional field.
    Vec2 travel{};
    float period = 0;
    // The Renown tier (0..7) from which an enemy or boss placed here appears at all: the room's
    // composition at higher tiers, authored rather than invented. 0, the default, is always there.
    // Only an enemy or a boss may have one. Written only when set.
    int min_tier = 0;
    // Which actor file draws it (M2), by the file's stem - "wanderer" for `wanderer.anim.json` - the
    // way a prefab or a cutscene is named. Empty draws with the project's first actor file, which is
    // also the player's. Only something drawn as a figure may name one (`kind_takes_art`): an enemy,
    // a dummy, a boss, an NPC or a decoration. Presentation only - nothing simulated reads it.
    // Written only when set.
    std::string art{};
    bool operator==(const Object &) const = default;
};
// The kinds drawn from an actor file, and so the ones that may name their `art`.
bool kind_takes_art(std::string_view kind);
// Whether `art` has the shape of an actor file's stem: letters, digits, '_' and '-'. Whether such a
// file exists is the project's question (the cooker and the Problems panel ask it).
bool art_key(std::string_view art);
// One object's content form, shared by scenes and prefabs.
Object object_from_json(const Json &value);
Json object_json(const Object &object);
// Where a moving solid is displaced from its authored position at `tick` (zero for one that does
// not move). Integer arithmetic on the tick, so every machine agrees.
Vec2 travel_offset(const Object &object, uint64_t tick);
// The enemy behaviours content can name, in the order the simulation numbers them. Shared so
// the loader, the editor and the simulation cannot disagree about what exists.
std::span<const std::string_view> archetype_names();
// The kinds that run the enemy AI, and so may carry an archetype or a patrol.
bool kind_is_enemy(const std::string &kind);
// An authored region that overrides camera framing while the focus point is inside it.
//
// Every setting is optional and a zone states only what it changes, inheriting the rest from
// the room default. That is deliberate: a zone that wrote out every field would silently
// re-author itself the day a new field is added with a different default.
struct CameraZone {
    std::string id;
    Rect area;        // the region the focus must be inside for this zone to apply
    int priority = 0; // higher wins where zones overlap; equal priorities break by authored order
    float blend_in = 0.5f, blend_out = 0.5f; // seconds
    bool snap = false;                       // adopt immediately instead of blending
    std::optional<Vec2> dead_zone;
    std::optional<float> look_ahead;
    std::optional<float> follow_rate;
    std::optional<float> zoom;
    std::optional<float> vertical_bias; // positive frames further down
    std::optional<Vec2> lock;           // hard lock to a fixed world point
    std::optional<Rect> bounds;         // sub-rectangle tighter than the room
    bool operator==(const CameraZone &) const = default;
};
// Wind: where hair and loose cloth move in a breeze. A room has a wind strength of its own (0,
// still, to 1) and may add zones that are windier - an open parapet in a sheltered hall. What it changes is
// presentation only: a standing figure plays its `idle.wind` clip instead of `idle` where the wind is above
// zero. Nothing simulated reads it.
struct WindZone {
    std::string id;
    Rect area;
    float strength = 1;
    bool operator==(const WindZone &) const = default;
};
// One field a prefab instance changes on one of the prefab's children.
//
// Optional per field, the same shape as a camera zone's overrides and for the same reason: an
// override states only what it changes, so a placement keeps tracking the prefab for everything
// else. Writing every field out would freeze each placement at the prefab's values on the day it
// was placed, which is the opposite of what a prefab is for.
struct PrefabOverride {
    std::string child; // the prefab child's own ID, not the expanded one
    std::optional<Vec2> position;
    std::optional<Vec2> size;
    std::optional<std::string> target;
    std::optional<int> layer;
    bool operator==(const PrefabOverride &) const = default;
};
// A placement of a prefab, in a room or inside another prefab.
struct PrefabInstance {
    std::string id;     // stable, unique among the instances beside it
    std::string prefab; // the prefab's ID, which is also its file stem
    Vec2 position;      // the origin the prefab's contents are placed relative to
    std::vector<PrefabOverride> overrides;
    bool operator==(const PrefabInstance &) const = default;
};
// A reusable group of objects, authored once and placed many times.
//
// Contents are relative to the instance origin, so a prefab can be placed anywhere. A prefab may
// place other prefabs, which is what makes a cycle possible - and why expansion looks for one.
struct Prefab {
    std::string id;
    std::vector<Object> objects;
    std::vector<PrefabInstance> instances;
    bool operator==(const Prefab &) const = default;
};
// A condition on the player's progress, in the story's vocabulary: a quest at a stage, a
// flag set or not, a fact a room knows, items carried, a Renown tier. One form every data file shares
// - a conversation's node, a room's cutscene trigger - read and written here, and judged by
// the game, which knows what a save holds.
enum class ConditionKind : uint8_t {
    Stage,  // quest `a` is at stage `b` (which may be `unstarted`)
    Flag,   // flag `a` is set
    NoFlag, // flag `a` is not set
    Fact,   // room `a` knows fact `b`
    Item,   // at least `amount` of item `a` is carried
    Renown  // the Renown tier is at least `amount` (some endings are for the renowned)
};
struct Condition {
    ConditionKind kind = ConditionKind::Flag;
    std::string a, b;
    int amount = 0;
    bool operator==(const Condition &) const = default;
};
// `{"quest", "stage"}`, `{"flag"}`, `{"no_flag"}`, `{"fact": "room/fact"}`, `{"item", "count"}` or
// `{"renown"}`. Throws, naming `where`, on anything else.
Condition condition_from_json(const Json &json, const std::string &where);
Json condition_json(const Condition &condition);
// Throws, naming `where`, on a condition whose names are not save keys (letters, digits, '_', '-',
// '.') or whose number is out of range: an item count 1..99, a tier I..VII. Whether a named quest,
// stage, item or fact exists is the project's question - the story's loader and the cooker ask it.
void check_condition(const Condition &condition, const std::string &where);
// What makes a cutscene fire. Authored on the room rather than inside the cutscene, because
// the same sequence can legitimately be reachable from more than one place.
struct CutsceneTrigger {
    std::string id;       // stable, unique within the scene
    std::string cutscene; // the cutscene's stable ID, which is also its file stem
    // "entry" fires on entering the room, "volume" when the player is inside `area`, "quest"
    // once progress has reached `quest`, "condition" the moment its `conditions` all hold while the
    // player is in the room. A string rather than an enum for the same reason Object::kind is one:
    // it round-trips legibly and validate_scene owns the vocabulary.
    std::string when = "entry";
    Rect area{};   // "volume" only
    int quest = 0; // "quest" only: fires once the quest counter reaches this
    // One-shot across save and reload, not merely across a session. Persisted as a progress
    // flag, so it survives quitting mid-cutscene.
    bool once = true;
    // What must also hold for it to fire, whatever its `when`: the story's conditions, so an
    // entry can wait for a quest's stage, a fact or a flag rather than only for the `quest` counter.
    // All must hold. Written as `"if"`, and only when there are any, so older rooms round-trip.
    std::vector<Condition> conditions{};
    bool operator==(const CutsceneTrigger &) const = default;
};
// The most conditions one trigger may carry, as for a dialogue node.
inline constexpr size_t max_trigger_conditions = 8;
// The trigger kinds validate_scene accepts, in a stable order for presentation.
std::span<const std::string_view> trigger_kinds();
// Straight multiply-blend colour, 0-1 per channel. Here rather than in render.hpp because a
// frame authors a tint and content must not depend on the renderer.
// Which typeface text is set in: the body face (`font.ttf`) for everything read at a glance - numbers,
// rows, notices - or the display face (`font_display.ttf`, the game's titles: a menu's heading, a room's
// name as it is entered). A renderer without a display face sets display text in the body face.
enum class TextFace : uint8_t { Body, Display };
struct Color {
    float r = 1, g = 1, b = 1, a = 1;
    bool operator==(const Color &) const = default;
};
// Coverage of an untextured effect quad. Shared draw data, independent of SDL and gameplay.
enum class SpriteMask : uint8_t { Quad, Glow, Spark, Dust, Ring, Ribbon };
// A picture behind or in front of a room, drawn with parallax. Presentation only.
struct Backdrop {
    std::string image;   // a PNG inside the project, registered in the manifest
    Vec2 position{};     // its top-left, in world units, when the camera looks at the world origin
    Vec2 parallax{1, 1}; // how much of the camera's motion it follows: 1 fixed in the world, 0 to the screen
    float scale = 1;     // world units per image pixel
    bool repeat_x = false; // tiled across the view
    bool front = false;    // in front of the room and its figures: foliage, a pillar passing close
    Color tint{};
    // A part that turns: about `pivot`, in its image's pixels - which may lie outside the image,
    // a bell hung on chains turning about the chains' top - `angle` degrees clockwise from how it is painted.
    // `motion` names what turns it as the room is played (`StageProps`): "hand", a clock's hand creeping on
    // and lurching forward at each blow a boss lands; "bell", a bell still but for the wind, swinging and
    // ringing when it is struck. Written only when set; a turning part is never repeated.
    std::string motion{};
    Vec2 pivot{};
    float angle = 0;
    bool operator==(const Backdrop &) const = default;
};
// The motions a backdrop may name.
inline constexpr std::array<std::string_view, 2> backdrop_motions{"hand", "bell"};
// A point of light: its colour times its intensity, falling off to nothing at its radius.
struct PointLight {
    Vec2 position{};
    float radius = 240;
    Color color{1, 0.9f, 0.7f, 1};
    float intensity = 1;
    bool operator==(const PointLight &) const = default;
};
// A grid of tiles painted over a room: each cell empty (0) or tile `n` of a tileset image (1-based,
// row by row, `tile` pixels square). Tiles listed in `solid` make walls in the room as played, those in
// `platforms` one-way platforms, merged into as few rectangles as cover them (`tiles.hpp`); every other
// tile is scenery.
struct TileMap {
    std::string tileset; // a PNG inside the project, registered in the manifest
    int tile = 16;       // pixels per tile in the image
    float cell = 32;     // world units per cell, a multiple of the authoring grid
    Vec2 origin{};       // the world position of cell (0, 0)'s top-left
    int columns = 0, rows = 0;
    std::vector<uint16_t> cells; // columns * rows, row by row
    std::vector<uint16_t> solid{}, platforms{};
    bool operator==(const TileMap &) const = default;
};
struct Scene {
    std::string id;
    std::vector<Object> objects;
    // Written to the file only when non-empty, so every room authored before the camera
    // director round-trips byte-identically. Cutscene triggers follow the same rule.
    std::vector<CameraZone> camera_zones{};
    std::vector<CutsceneTrigger> cutscene_triggers{};
    // Prefab placements. Expanded into concrete objects at load; the room file stores the
    // placement, never the expansion, so editing the prefab updates every room that uses it.
    std::vector<PrefabInstance> prefabs{};
    // The music track played in this room: an ID from the project's `sounds.json`. Empty plays
    // none - or keeps what was playing, for a corridor between two rooms that share a track. Written
    // only when set, like everything above.
    std::string music{};
    // An endless room (the sandbox): the ID of a still wall - its floor - that the simulation keeps
    // under the player, re-centred sideways in whole steps as the player walks, so the floor never
    // ends in either direction. Empty for every ordinary room; written only when set.
    std::string endless{};
    // Its atmosphere: pictures behind and in front, the light everywhere (white is none) and
    // points of light, multiplied over the world. Written only when set.
    std::vector<Backdrop> backdrops{};
    Color ambient{};
    std::vector<PointLight> lights{};
    // Its tile terrain, drawn in order on the solid layer; written only when there is some.
    std::vector<TileMap> tiles{};
    // Its wind: the room's own strength, 0 to 1, and windier zones. Written only when set.
    float wind = 0;
    std::vector<WindZone> wind_zones{};
    // The room's art draws its own ground: no generic stone is drawn for its walls under its
    // pictures - no second ledge below a painted floor, no worn lip over a painted top. Written only when
    // set.
    bool ground_art = false;
    bool operator==(const Scene &) const = default;
};
// The wind at a point: the strongest of the room's own and of every zone containing the point.
float wind_at(const Scene &scene, Vec2 point);
// A named point on the sprite that equipment hangs from, in canvas pixels.
struct Attachment {
    std::string id;
    Vec2 point{};
    bool operator==(const Attachment &) const = default;
};
// One frame of a clip.
//
// A frame is not just an atlas index. Real 2D animation holds key poses longer than in-between
// ones, shifts a pose a few pixels without redrawing it, and moves the socket equipment hangs
// from as the arm swings. All three are per frame here, and all three are optional: a frame
// authored as a bare integer still means "this cell, for the clip's default hold".
struct AnimationFrame {
    int atlas = 0; // which cell of the facing row
    int ticks = 0; // 0 means "use the clip's ticks_per_frame"
    Vec2 offset{}; // canvas-pixel shift, for a pose that moves without being redrawn
    // Socket positions for this frame, overriding the actor's rest pose. A hand that stays put
    // while the arm swings is the single most visible way attached equipment looks wrong.
    std::vector<Attachment> attachments;
    // ---- Per-frame transform and colour ----
    // The four things 2D animation tooling is expected to give an artist without a redraw.
    // Every one of them is presentation: none may change where a hit lands or how far an actor
    // moves, and nothing downstream reads them for anything but drawing.
    //
    // Mirroring is separate from the facing rows. A row is which art is drawn; this flips the
    // cell that was chosen, which is how a two-frame sway becomes four, or a thrown weapon
    // tumbles. Both apply about the frame's own centre, so a flip does not also move the pose.
    bool flip_x = false, flip_y = false;
    // Squash and stretch, multiplying the drawn size about the feet pivot. Negative values are
    // refused; mirroring is `flip_x`, so that a scale of -1 cannot mean two things.
    Vec2 scale{1, 1};
    float angle = 0; // degrees clockwise about the pivot, for a tilt or a swing
    // What the flip, scale and rotation happen about, in canvas pixels. Unset means the
    // actor's feet, which is where a character leans and squashes from. A thrown weapon
    // tumbling about its own middle, or a door swinging on its hinge, needs a different one -
    // and there is no way to express either by moving the art, because the art is where the
    // pose is. This never moves the drawing on its own: with no other transform authored, a
    // pivot anywhere draws exactly the same pixels.
    std::optional<Vec2> pivot;
    // Multiplied into the sprite's colour, so the default is opaque white and a file that
    // authors nothing here draws exactly as it did before. A hit flash, a fade, a ghost.
    Color tint{};
    bool operator==(const AnimationFrame &) const = default;
};
// Something the presentation layer should do at a moment in a clip: a footstep, a swing sound,
// a dust puff.
//
// Visual and audio only. EXTENSIBILITY's presentation boundary and requirement 6.2 both say
// the same thing from different directions: authoritative attack windows are simulation data,
// never animation callbacks. An event here can play a sound; it can never deal damage, and
// nothing reads it to decide when a hit lands.
struct AnimationEvent {
    int tick = 0; // ticks from the start of the clip
    std::string id;
    bool operator==(const AnimationEvent &) const = default;
};
// How a clip behaves when it reaches its end.
enum class LoopMode {
    Once,    // hold the last frame; an attack must not snap back mid-recovery
    Loop,    // wrap to the start
    PingPong // play forwards then backwards, which halves the art for a sway or a breath
};
// One authored clip: a named sequence of frames with its own timing and loop behaviour.
//
// The file has authored clips since v2; the loader used to flatten them into a single frame
// list with one global rate and two hardcoded frame indices, so an artist could add a clip and
// the game would silently ignore it.
struct AnimationClip {
    std::string id;
    std::vector<AnimationFrame> frames;
    int ticks_per_frame = 8; // default hold for frames that do not state their own
    LoopMode loop = LoopMode::Loop;
    // Multiplier on playback rate, so one authored clip can be reused faster or slower without
    // duplicating its frames. Applied to time, never to the authored holds themselves.
    float speed = 1;
    std::vector<AnimationEvent> events;
    // Which frame a Loop or PingPong clip returns to, so the frames before it play once as a
    // lead-in and only the rest cycle. A charge-up that winds up and then holds is the usual
    // case, and without it an artist has to split one pose into two clips and keep them in
    // step. Ignored by Once, where there is nothing to return to.
    int loop_from = 0;
    // Total ticks for one pass, honouring per-frame holds. A ping-pong pass is the forward
    // pass; the full cycle is twice this less the two shared end frames.
    int pass_ticks() const;
    // Ticks before `loop_from`, so the lead-in can be skipped on the second pass.
    int lead_ticks() const;
    bool operator==(const AnimationClip &) const = default;
};
// What a clip looks like at one moment: everything the draw call needs and nothing it does not.
struct SampledFrame {
    int atlas = 0;
    Vec2 offset{};
    size_t index = 0; // which authored frame, so an editor can say where it is
    bool flip_x = false, flip_y = false;
    Vec2 scale{1, 1};
    float angle = 0;
    Color tint{};
    std::optional<Vec2> pivot;
};
// A packed frame: a rectangle of one of the file's texture pages,
// and where on the frame's canvas it sits. An actor file with regions reads each frame's `atlas` as an
// index into them instead of a grid cell, so frames are trimmed to what they contain and may run over
// several pages - what a baked 3D character needs, with hundreds of frames per layer.
//
// A frame with nothing drawn in it (a mouth hidden that frame, a piece off the canvas) is an *empty* region:
// zero wide or zero high (the bake writes [page, 0, 0, 0, 0, 0, 0]). It covers nothing and draws nothing.
struct AtlasRegion {
    int page = 0;                   // 0 is the file's `texture`, 1.. its `pages`
    int x = 0, y = 0, w = 1, h = 1; // pixels on that page
    int left = 0, top = 0;          // its top-left on the frame's canvas, in `cell_pixels` pixels
    bool empty() const { return w <= 0 || h <= 0; }
    bool operator==(const AtlasRegion &) const = default;
};
struct Animation {
    Vec2 feet{};
    int right_row = 1, left_row = 2;
    // Cells a side of the sheet: 4 unless the file says otherwise (`limits::max_atlas_grid`). A frame's
    // cell and a facing's row count in this grid.
    int grid = 4;
    // The left facing is the right one drawn mirrored about each frame's pivot (the feet), rather
    // than a row of its own: art drawn facing one way, as the editor study's sheets are. The rows
    // must then be the same. Sockets mirror with the art, so the right hand is used either way.
    bool mirror_left = false;
    // Packed frames, when the file has them: the regions a frame's `atlas` indexes, the pixels a
    // side of the 128-unit canvas they were cut from, the pixel size every page shares, and how many
    // pages there are (the `texture` and each of `pages`). Empty for every grid file.
    std::vector<AtlasRegion> regions{};
    // The canvas's side in units: 128 for every drawn file; a packed file may be larger (up to
    // `limits::max_packed_canvas`) - a baked swing reaches well above the head, and frames are trimmed, so
    // a larger canvas costs nothing. Feet, sockets and pivots lie on it.
    float canvas = 128;
    int cell_pixels = 0;
    Vec2 page_size{};
    int page_count = 1;
    std::vector<AnimationClip> clips;
    std::vector<Attachment> attachments;
    // Null when the file does not author it. Callers fall back rather than failing: an actor
    // with no "attack" clip must still be drawn, and the editor reports the gap.
    const AnimationClip *clip(std::string_view id) const;
    const Attachment *attachment(std::string_view id) const;
    // The socket for a frame: the frame's own override if it has one, otherwise the actor's
    // rest pose. Null when neither authors it.
    const Attachment *attachment(std::string_view id, const AnimationFrame &frame) const;
    bool operator==(const Animation &) const = default;
};
// Where a frame's cell lies on the sheet, in cells: the column, and the row counted on from the
// facing's row (see `limits::max_atlas_index`).
struct AtlasCell {
    int column = 0, row = 0;
};
AtlasCell atlas_cell(int atlas, int facing_row, int grid = 4);
// What a frame draws, whatever the file's layout: the page, the texture rectangle (0..1 of the page) and
// the part of the 128-unit canvas it covers. A grid cell is page 0 and the whole canvas; a region is its
// own page, rectangle and place. `row` is the facing's row (grids only). An empty region's image is empty:
// zero-sized on the page and on the canvas, so a quad placed from it covers nothing.
struct FrameImage {
    int page = 0;
    Rect uv{};
    Rect canvas{0, 0, 128, 128};
    bool empty() const { return !(canvas.w > 0 && canvas.h > 0); }
    bool operator==(const FrameImage &) const = default;
};
FrameImage frame_image(const Animation &animation, int atlas, int row);
// Which frame of a clip shows at `tick` ticks in, honouring per-frame holds, playback speed and
// the loop mode. A negative tick clamps to the start.
SampledFrame sample_clip(const AnimationClip &clip, int tick);
// Events whose time falls in (after, until]. Half-open at the start so advancing tick by tick
// fires each event exactly once, the same rule the cutscene director's audio cues follow.
std::vector<std::string> clip_events(const AnimationClip &clip, int after, int until);
// Throws on anything an actor file must not contain. Shared by the loader and the writer, so
// the editor cannot save a file the loader would then refuse to open.
void validate_animation(const Animation &animation);
Animation animation_from_json(const Json &json);
// Writes an animation back into the file it came from.
//
// Merged into `original` rather than regenerated, because the file carries fields the runtime
// model deliberately does not: the texture and .kra source names, the canvas size, the filter,
// the padding, `mirror_safe`. Regenerating would silently drop an artist's metadata, so this
// replaces only what it owns and leaves the rest exactly as written.
//
// Follows the same omit-at-default rule as every other authored format here: a frame with
// nothing but an atlas index writes as a bare integer, so a file that used none of the newer
// per-frame fields round-trips unchanged.
Json animation_json(const Animation &animation, const Json &original);
Scene scene_from_json(const Json &json);
Json scene_json(const Scene &scene);
// Authored-content limits. validate_scene enforces exactly these, and every editor field,
// snap and tool reads them from here. Restating a limit anywhere else is how a UI ends up
// letting someone author content the loader then refuses.
namespace limits {
inline constexpr size_t max_objects = 512, max_identifier = 64;
inline constexpr size_t max_backdrops = 16, max_lights = 32;
inline constexpr float max_parallax = 2, min_backdrop_scale = 0.05f, max_backdrop_scale = 20;
inline constexpr float min_light_radius = 8, max_light_radius = 4000, max_light_intensity = 4;
inline constexpr size_t max_tile_maps = 4;
inline constexpr int min_tile_pixels = 4, max_tile_pixels = 256, max_tile_grid = 512, max_tile = 4096;
inline constexpr float max_tile_cell = 256;
inline constexpr float max_position = 10000; // absolute, on both axes
inline constexpr float min_size = 1, max_size = 2000;
inline constexpr float grid = 8;    // authoring snap increment
inline constexpr int max_layer = 7; // layers 0..7
// Camera director. Stated here on the first commit for the same reason as every other limit:
// the inspector, the loader and validate_scene must never disagree about what is authorable.
inline constexpr size_t max_camera_zones = 64;
inline constexpr size_t max_wind_zones = 64;
inline constexpr float min_zoom = 0.5f, max_zoom = 2.0f; // >1 magnifies: less world is seen
inline constexpr float max_blend = 5;                    // seconds to fade a zone in or out
inline constexpr float max_dead_zone = 600;              // half-extent, world units
inline constexpr float max_look_ahead = 1000;
inline constexpr float min_follow_rate = 0.1f, max_follow_rate = 60; // exponential, per second
inline constexpr float max_vertical_bias = 1000;
inline constexpr int max_priority = 999;
// Actor animation. Same rule as everything above: validate_animation enforces exactly these and
// every Animator widget reads them, so the editor can author the whole format and nothing else.
// The two that carry a floor rather than "any positive number" carry one on purpose - a format
// whose lower bound cannot be written down is a format no widget can offer all of.
// A clip holds up to 256 frames: a baked 3D-pipeline clip is sampled at every frame of its Blender action
// (24 a second), so a 5-second idle alone is 120. Up to 128 clips: a character with
// several weapon classes, their chains and a sheathe passes 64.
inline constexpr size_t max_clips = 128, max_clip_frames = 256, max_clip_events = 64;
// A 512x512 sheet of 128x128 cells, four by four. A frame's cell counts from its facing's row: 0..3
// are that row, and 4..15 continue on the rows below it, so a file that draws one facing (and mirrors
// it for the other) can hold sixteen poses. Every frame must still land on the sheet.
inline constexpr int atlas_columns = 4, atlas_rows = 4;
inline constexpr int max_atlas_index = atlas_columns * atlas_rows - 1;
inline constexpr int max_atlas_row = atlas_rows - 1;
// A file may cut its sheet into a finer square grid (`"grid"`), for an actor with more poses than
// sixteen: the same 128-unit canvas a cell, any pixel size, up to sixteen cells a side.
inline constexpr int max_atlas_grid = 16;
// Packed frames: at most this many regions and pages in one actor file, and pages and canvases
// no larger than a texture may be.
inline constexpr size_t max_regions = 8192;
inline constexpr int max_pages = 32, max_page_pixels = 8192;
inline constexpr float max_packed_canvas = 512;
inline constexpr int max_clip_ticks = 600; // a clip's default hold, and a frame's own hold
inline constexpr float min_clip_speed = 0.01f, max_clip_speed = 16;
inline constexpr float min_frame_scale = 0.01f, max_frame_scale = 8;
inline constexpr float max_canvas = 128;       // feet, sockets and pivots live on one cell
inline constexpr float max_frame_offset = 128; // per-frame nudge, either direction
inline constexpr float max_frame_angle = 360;  // degrees, either direction
// Cutscene / sequence director.
inline constexpr size_t max_cutscene_keys = 256;    // per track
inline constexpr size_t max_cutscene_triggers = 32; // per room
inline constexpr float max_cutscene_duration = 600; // seconds
inline constexpr size_t max_dialogue_line = 512;    // characters
} // namespace limits
// The object kinds validate_scene accepts, in a stable order for presentation. Shared so an
// editor cannot offer a kind the loader rejects, or omit one it allows.
std::span<const std::string_view> object_kinds();
// The kinds that become simulation actors. A capability grant on any other kind is refused rather
// than ignored, for the same reason an unknown capability name is.
bool kind_becomes_actor(const std::string &kind);
// Where a door leads: a room, and optionally a named entrance in it, written "room/entrance". A
// door naming only a room arrives at that room's spawn, which is what every door did before
// entrances existed. Parsing never throws; `validate_scene` is what refuses a malformed target.
struct DoorTarget {
    std::string room, entrance;
    bool operator==(const DoorTarget &) const = default;
};
DoorTarget door_target(std::string_view target);
std::string door_target_string(const DoorTarget &target);
void validate_scene(const Scene &scene);

// The prefabs a project contains. A prefab that will not load is recorded rather than thrown,
// the same way a broken cutscene is: a room that places it degrades to placing nothing, and the
// editor reports which file is wrong.
struct PrefabLibrary {
    std::vector<Prefab> prefabs;
    std::vector<std::string> errors;
    const Prefab *find(const std::string &id) const;
};
PrefabLibrary load_prefabs(const fs::path &project);
// Throws on anything a prefab file must not contain. Shared by the loader and the writer, the
// same way validate_scene and validate_animation are, so an editor cannot save a prefab the
// loader would then refuse. It deliberately says nothing about cycles: a prefab that places
// itself is a file that loads and expands to nothing, which expansion and the cooker report as
// the build fault it is.
void validate_prefab(const Prefab &prefab);
Prefab prefab_from_json(const Json &json);
Json prefab_json(const Prefab &prefab);
// Expands every placement in a scene into concrete objects.
//
// A child's expanded ID is "<instance>.<child>", so two placements of the same prefab cannot
// collide and a reference into a placement stays stable when the prefab is renamed. Problems -
// a missing prefab, a cycle, an override naming a child that is not there - are appended to
// `problems` rather than thrown, because a room that places a broken prefab must still open.
// `owners`, when given, receives one {expanded object ID, top-level placement ID} pair per
// object the expansion produced. An editor needs it to treat a placement's contents as one
// thing - to outline them, or to select the placement when one is clicked. Deriving the same
// answer from the "<instance>." prefix looks equivalent and is not: an authored object may
// already be called "ledge.upper", and a placement named "ledge" would then appear to own half
// the room. That is not hypothetical; it is what the first version of the outline did.
std::vector<Object> expand_prefabs(const Scene &scene, const PrefabLibrary &library,
                                   std::vector<std::string> &problems,
                                   std::vector<std::pair<std::string, std::string>> *owners = nullptr);
class SceneHistory {
    std::vector<Scene> undo_, redo_;
    Scene saved_;
    std::optional<Scene> gesture_;

  public:
    Scene current;
    explicit SceneHistory(Scene scene) : saved_(scene), current(std::move(scene)) {}
    void edit(const std::function<void(Scene &)> &change);
    void begin_gesture();
    void preview(const std::function<void(Scene &)> &change);
    bool commit_gesture();
    bool cancel_gesture();
    bool gesturing() const { return gesture_.has_value(); }
    bool undo();
    bool redo();
    bool dirty() const { return current != saved_; }
    void saved() { saved_ = current; }
    const Scene &saved_state() const { return saved_; }
    // Named accessor so callers can read the document the same way through SceneHistory and
    // through the editor's EditorDocument, which does not expose a mutable scene.
    const Scene &scene() const { return current; }
};
struct CanvasView {
    Rect area;
    Vec2 canvas{1280, 720};
    Rect fitted() const;
    Vec2 to_canvas(Vec2 screen) const;
    Vec2 to_screen(Vec2 point) const;
    bool contains(Vec2 screen) const { return fitted().contains(screen); }
};
// The window size a display scale asks for, in the units SDL_CreateWindow takes.
//
// A 1280x720 window on a display set to 125% renders about 20% smaller than every app around
// it, because those apps scale their content and this one did not: SDL reports
// display_scale 1.25 with pixel_density 1.0, so logical points and physical pixels are the
// same and nothing was consuming the 1.25. Asking for a proportionally larger window puts the
// canvas back at the physical size the rest of the desktop is drawn at.
//
// Pure, and separate from the window, so the decision is testable on a host whose own display
// scale is 1.0 - which is every Linux machine this has been run on so far.
Vec2 scaled_window_size(Vec2 canvas, float display_scale);
// How the editor divides its viewport region between the authoring view and the running game.
// `Single` is the historical behaviour: one view filling the region.
enum class ViewportSplit { Single, SideBySide, Stacked };
struct ViewportPair {
    Rect scene, game;
    bool operator==(const ViewportPair &) const = default;
};
// The on-screen rectangles for the two views. With `Single` the scene fills the region and the
// game rectangle is empty, which is how a caller knows not to draw it. A region too small to
// divide falls back to `Single` rather than producing two unusable slivers.
ViewportPair split_viewport(Rect region, ViewportSplit split);
struct EditorLayout {
    Rect viewport, project, inspector;
};
EditorLayout editor_layout(Vec2 size);
std::optional<size_t> pick_object(const Scene &scene, Vec2 point);
// Picks only among objects the predicate accepts, so a hidden or locked layer does not swallow
// a click meant for what is behind it. Filtering after the fact would return nothing instead.
std::optional<size_t> pick_object(const Scene &scene, Vec2 point,
                                  const std::function<bool(const Object &)> &pickable);
// Centered extents, the same rectangle pick_object hit-tests. Framing and picking must agree or
// the editor can frame a region the creator cannot click.
Rect object_bounds(const Object &object);
// Union of every object's bounds; an empty scene yields the whole authoring canvas.
Rect scene_bounds(const Scene &scene);
// What a sweep touched (contact facts). The booleans are what every caller has always read;
// the rest is what traversal needs next: the surface normal of the floor and of the wall, and
// *which* object each was, as an index into the span swept against - stable for as long as that
// span is, which for the simulation is until the room changes - so a moving platform can carry
// what stands on it and a rule can ask what kind of floor it is.
struct MoveResult {
    Vec2 position;
    bool floor{}, ceiling{}, wall{};
    Vec2 floor_normal{}, wall_normal{};
    int floor_object = -1, wall_object = -1;
};
// Solid kinds. A `platform` is one-way: solid only to something coming down onto its top, so it
// is jumped up through and stood on, and `through_platforms` drops through it.
bool kind_is_solid(std::string_view kind);
MoveResult sweep_move(Vec2 origin, Vec2 displacement, float radius, float half_height,
                      std::span<const Object> objects, bool through_platforms = false);
Vec2 slide_move(Vec2 origin, Vec2 displacement, float radius, std::span<const Object> objects);
// Which solids lie along each band of a room's width, in scene order: a room's walls and
// platforms by the x they can occupy - a mover's over its whole travel - so a sweep asks only about
// the ones near it. A room of 512 objects swept every one of them for every move of every actor.
// Built from the room as it is. Callers must rebuild when solid membership, object order or covered
// x bounds change, even at the same count. Count mismatch is detected by the query and falls back;
// other geometry edits require the owner's invalidation/coverage check.
struct SolidIndex {
    float left = 0, band = 128;   // world x where band 0 begins, and each band's width
    size_t objects = 0;           // how many objects the room had when this was built
    std::vector<uint32_t> starts; // band b lists ids[starts[b] .. starts[b + 1])
    std::vector<uint16_t> ids;    // object indices, ascending within each band
    bool operator==(const SolidIndex &) const = default;
};
SolidIndex index_solids(std::span<const Object> objects, float band = 128);
// The same sweep, asking the index which objects to consider. The same answer as the full scan, to
// the last bit: the index leaves out only solids the scan would have passed over.
MoveResult sweep_move(Vec2 origin, Vec2 displacement, float radius, float half_height,
                      std::span<const Object> objects, const SolidIndex &index,
                      bool through_platforms = false);
Vec2 slide_move(Vec2 origin, Vec2 displacement, float radius, std::span<const Object> objects,
                const SolidIndex &index);
std::vector<uint8_t> read_bytes(const fs::path &path, size_t max_size = 64 * 1024 * 1024);
uint64_t hash_bytes(std::span<const uint8_t> bytes);
} // namespace med
