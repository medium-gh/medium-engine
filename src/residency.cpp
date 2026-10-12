#include "medium/residency.hpp"
namespace med {
const char *residency_name(Residency state) {
    switch (state) {
    case Residency::Absent:
        return "absent";
    case Residency::Staged:
        return "staged";
    case Residency::Resident:
        return "resident";
    case Residency::Failed:
        return "failed";
    }
    return "absent";
}

void AssetGraph::add(Entry entry) {
    if (entry.id.empty())
        throw std::runtime_error("An asset with no id cannot be depended on");
    entries_.push_back(std::move(entry));
}
const AssetGraph::Entry *AssetGraph::find(std::string_view id) const {
    for (const auto &entry : entries_)
        if (entry.id == id)
            return &entry;
    return nullptr;
}
std::vector<std::string> AssetGraph::ids() const {
    std::vector<std::string> out;
    for (const auto &entry : entries_)
        out.push_back(entry.id);
    return out;
}
void AssetGraph::validate() const {
    std::set<std::string> seen;
    for (const auto &entry : entries_)
        if (!seen.insert(entry.id).second)
            throw std::runtime_error("Two assets share the id " + entry.id);
    for (const auto &entry : entries_)
        for (const auto &needed : entry.requires_)
            if (!find(needed))
                throw std::runtime_error(entry.id + " depends on " + needed + ", which is not here");
    // Cycles, named by their members. "There is a cycle" is not something anyone can act on.
    std::map<std::string, int> marks;
    std::vector<std::string> path;
    std::function<void(const std::string &)> walk = [&](const std::string &id) {
        if (marks[id] == 1) {
            std::string names;
            bool inside = false;
            for (const auto &step : path) {
                inside = inside || step == id;
                if (inside)
                    names += (names.empty() ? "" : " -> ") + step;
            }
            throw std::runtime_error("Assets depend on each other: " + names + " -> " + id);
        }
        if (marks[id] == 2)
            return;
        marks[id] = 1;
        path.push_back(id);
        if (const auto *entry = find(id))
            for (const auto &needed : entry->requires_)
                walk(needed);
        path.pop_back();
        marks[id] = 2;
    };
    for (const auto &entry : entries_)
        walk(entry.id);
}

std::vector<std::string> AssetGraph::stale_from(std::span<const fs::path> changed) const {
    std::vector<std::string> touched;
    for (const auto &entry : entries_)
        for (const auto &file : entry.files)
            if (std::find(changed.begin(), changed.end(), file) != changed.end()) {
                touched.push_back(entry.id);
                break;
            }
    return dependents_of(touched);
}

std::vector<std::string> AssetGraph::dependents_of(std::span<const std::string> ids) const {
    validate(); // ordering is only meaningful on a graph that has one
    // Everything reachable *upward* from the seeds: an asset is stale when anything it is built
    // on is stale.
    std::set<std::string> stale(ids.begin(), ids.end());
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto &entry : entries_) {
            if (stale.count(entry.id))
                continue;
            for (const auto &needed : entry.requires_)
                if (stale.count(needed)) {
                    stale.insert(entry.id);
                    grew = true;
                    break;
                }
        }
    }
    // Emitted in dependency order: a dependency before anything built on it. Rebuilding a
    // dependent first would load it against a version about to be replaced.
    std::vector<std::string> out;
    std::set<std::string> done;
    std::function<void(const std::string &)> emit = [&](const std::string &id) {
        if (done.count(id) || !stale.count(id))
            return;
        done.insert(id);
        if (const auto *entry = find(id))
            for (const auto &needed : entry->requires_)
                emit(needed);
        out.push_back(id);
    };
    // Seeded in declaration order so the result is stable run to run rather than depending on how
    // a set happened to order its strings.
    for (const auto &entry : entries_)
        emit(entry.id);
    return out;
}

Residency ResidencyTable::state(std::string_view id) const {
    const auto found = slots_.find(id);
    return found == slots_.end() ? Residency::Absent : found->second.state;
}
std::string ResidencyTable::failure(std::string_view id) const {
    const auto found = slots_.find(id);
    return found == slots_.end() ? std::string{} : found->second.failure;
}
uint64_t ResidencyTable::resident(std::string_view id) const {
    const auto found = slots_.find(id);
    return found == slots_.end() ? 0 : found->second.resident_revision;
}
uint64_t ResidencyTable::staged(std::string_view id) const {
    const auto found = slots_.find(id);
    return found == slots_.end() ? 0 : found->second.staged_revision;
}
uint64_t ResidencyTable::stage(std::string_view id) {
    if (id.empty())
        throw std::runtime_error("An asset with no id cannot be staged");
    auto &slot = slots_[std::string(id)];
    slot.state = Residency::Staged;
    slot.staged_revision = next_revision_++;
    slot.failure.clear();
    return slot.staged_revision;
}
void ResidencyTable::fail(std::string_view id, std::string reason) {
    auto &slot = slots_[std::string(id)];
    // The resident version is deliberately untouched. A candidate that will not load must not take
    // the working one with it.
    slot.state = slot.resident_revision ? Residency::Resident : Residency::Failed;
    slot.staged_revision = 0;
    slot.failure = std::move(reason);
}
std::vector<std::string> ResidencyTable::pending() const {
    std::vector<std::pair<uint64_t, std::string>> waiting;
    for (const auto &[id, slot] : slots_)
        if (slot.state == Residency::Staged)
            waiting.emplace_back(slot.staged_revision, id);
    std::sort(waiting.begin(), waiting.end()); // staging order, which is dependency order
    std::vector<std::string> out;
    for (auto &[revision, id] : waiting)
        out.push_back(std::move(id));
    return out;
}
void ResidencyTable::activate(std::string_view id) {
    auto found = slots_.find(id);
    if (found == slots_.end() || found->second.state != Residency::Staged)
        throw std::runtime_error("Nothing is staged for " + std::string(id) + " to activate");
    found->second.resident_revision = found->second.staged_revision;
    found->second.staged_revision = 0;
    found->second.state = Residency::Resident;
    found->second.failure.clear();
}
std::vector<std::string> ResidencyTable::activate_pending() {
    auto waiting = pending();
    for (const auto &id : waiting)
        activate(id);
    return waiting;
}
void ResidencyTable::forget(std::string_view id) { slots_.erase(std::string(id)); }

void UploadOwner::check(const char *what) const {
    if (!owned())
        throw std::runtime_error(std::string(what) + " was attempted off the thread that owns GPU "
                                                     "uploads; staging may happen anywhere, "
                                                     "activation may not");
}
} // namespace med
