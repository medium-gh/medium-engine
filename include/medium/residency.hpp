#pragma once
#include "medium/core.hpp"
#include <set>
#include <thread>
namespace med {
// Which assets are loaded, what they are built from, and when a new version may take their place.
//
// The runtime used to have none of this. It watched two file paths by name, reloaded them inline
// the moment a timestamp moved, and had no idea that anything depended on anything else - so
// changing a texture an animation referenced left the animation resident and stale, and the only
// way to notice was to look at the screen.
//
// Three separate things are needed and they are separated here on purpose:
//
// * **What depends on what** (`AssetGraph`), so changing one file invalidates everything built on
//   it, in an order where a dependency is rebuilt before its dependents.
// * **Staging apart from activation** (`ResidencyTable`), so a new version is loaded and validated
//   while the old one is still being drawn, and the swap is a separate, cheap, chosen moment.
// * **Who may talk to the GPU** (`UploadOwner`), stated and enforced rather than assumed.

// Where one asset has got to.
enum class Residency : uint8_t {
    Absent,   // never loaded
    Staged,   // a candidate is prepared and waiting for an activation boundary
    Resident, // in use
    Failed    // a candidate was rejected; whatever was resident stays resident
};
const char *residency_name(Residency state);

// The assets the runtime watches, the files each is built from, and what each depends on.
//
// Separate from the cooker's `index.json`, which describes a *package*. This describes what is
// live, which during authoring is a project directory that changes under the running process.
class AssetGraph {
  public:
    struct Entry {
        std::string id;
        std::vector<fs::path> files;        // sources this asset is built from
        std::vector<std::string> requires_; // ids this asset is built on top of
    };

  private:
    std::vector<Entry> entries_;

  public:
    void add(Entry entry);
    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    const Entry *find(std::string_view id) const;
    std::vector<std::string> ids() const;
    // Throws on a duplicate id, a dependency that is not in the graph, or a cycle. A cycle is
    // named rather than merely detected: "these assets depend on each other" is not actionable.
    void validate() const;
    // Everything that must be restaged when these files change, **in dependency order** - a
    // dependency appears before anything built on it. Rebuilding a dependent against a stale
    // dependency and then rebuilding the dependency is two loads to reach the wrong answer once.
    std::vector<std::string> stale_from(std::span<const fs::path> changed) const;
    // The same, from asset ids rather than files: what else falls over if these are restaged.
    std::vector<std::string> dependents_of(std::span<const std::string> ids) const;
};

// Per-asset state, with the last good version retained through a failure.
//
// Retention is the whole point of separating staging from activation. A candidate that does not
// load must leave the resident one alone: an authoring session where a half-saved file blanks the
// character is worse than one where the character is a few seconds out of date.
class ResidencyTable {
    struct Slot {
        Residency state = Residency::Absent;
        uint64_t resident_revision = 0;
        uint64_t staged_revision = 0;
        std::string failure;
    };
    std::map<std::string, Slot, std::less<>> slots_;
    uint64_t next_revision_ = 1;

  public:
    Residency state(std::string_view id) const;
    // What went wrong with the last candidate, empty when nothing did.
    std::string failure(std::string_view id) const;
    // The revision currently in use, and the one waiting. Zero means none.
    uint64_t resident(std::string_view id) const;
    uint64_t staged(std::string_view id) const;

    // A candidate has been prepared. Returns its revision, which the caller uses to match the
    // prepared bytes to the slot when activating.
    uint64_t stage(std::string_view id);
    // A candidate was rejected. The resident version is untouched and the reason is kept.
    void fail(std::string_view id, std::string reason);
    // Everything staged, in the order it was staged. The activation boundary reads this.
    std::vector<std::string> pending() const;
    // The staged candidate becomes resident. Throws when nothing is staged, because activating
    // what was never prepared is a bug that would otherwise present as a missing asset.
    void activate(std::string_view id);
    // Activates everything pending, and returns what it activated.
    std::vector<std::string> activate_pending();
    void forget(std::string_view id);
};

// The thread permitted to touch the GPU.
//
// Constructed on that thread; every upload asserts against it. The renderer has always been
// main-thread owned and the rule has always been true - it was simply never *stated*, so the first
// background loader anyone added would have discovered it by crashing somewhere inside a driver,
// which is the least informative place to find out.
class UploadOwner {
    std::thread::id owner_;

  public:
    UploadOwner() : owner_(std::this_thread::get_id()) {}
    std::thread::id owner() const { return owner_; }
    bool owned() const { return std::this_thread::get_id() == owner_; }
    // Throws from any other thread, naming what was attempted.
    void check(const char *what) const;
};
} // namespace med
