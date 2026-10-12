#include "medium/core.hpp"
#include "medium/tiles.hpp"
#include <atomic>
#include <box2d/box2d.h>
#include <cctype>
#include <cerrno>
#include <fstream>
#include <iostream>
#include <set>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace med {
void log(LogLevel level, const std::string &message) {
    std::cerr << "[" << std::array{"info", "warning", "error"}[static_cast<size_t>(level)] << "] " << message
              << '\n';
}
std::vector<uint8_t> read_bytes(const fs::path &path, size_t max_size) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("Cannot open " + path.string());
    auto end = in.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > max_size)
        throw std::runtime_error("File exceeds limit: " + path.string());
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Read failed: " + path.string());
    return bytes;
}
Json read_json(const fs::path &path) {
    auto b = read_bytes(path, max_json_bytes);
    return Json::parse(b);
}
char32_t next_codepoint(std::string_view text, size_t &at) {
    const auto byte = [&](size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(at);
    const auto bad = [&] {
        ++at;
        return char32_t{0xFFFD};
    };
    if (lead < 0x80) {
        ++at;
        return lead;
    }
    const size_t length = lead >= 0xF0 && lead <= 0xF4   ? 4
                          : lead >= 0xE0 && lead <= 0xEF ? 3
                          : lead >= 0xC2 && lead < 0xE0  ? 2
                                                         : 0;
    if (length == 0 || at + length > text.size())
        return bad();
    char32_t code = lead & (length == 2 ? 0x1F : length == 3 ? 0x0F : 0x07);
    for (size_t i = 1; i < length; ++i) {
        if ((byte(at + i) & 0xC0) != 0x80)
            return bad();
        code = (code << 6) | (byte(at + i) & 0x3F);
    }
    // Overlong forms, surrogates and past the last code point are not characters.
    if ((length == 3 && code < 0x800) || (length == 4 && (code < 0x10000 || code > 0x10FFFF)) ||
        (code >= 0xD800 && code <= 0xDFFF))
        return bad();
    at += length;
    return code;
}
bool valid_utf8(std::string_view text) {
    for (size_t at = 0; at < text.size();) {
        const size_t from = at;
        if (next_codepoint(text, at) == 0xFFFD && !(at - from == 3 && text.substr(from, 3) == "\xEF\xBF\xBD"))
            return false;
    }
    return true;
}
void append_utf8(std::string &out, char32_t c) {
    if (c < 0x80)
        out.push_back(static_cast<char>(c));
    else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}
std::vector<std::string> wrap_text(std::string_view text, float width,
                                   const std::function<float(std::string_view)> &measure) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        const auto paragraph =
            text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (width <= 0) {
            lines.emplace_back(paragraph);
        } else {
            std::string line;
            size_t at = 0;
            while (at < paragraph.size()) {
                // The next word and the spaces before it.
                size_t word_end = paragraph.find(' ', at);
                if (word_end == std::string_view::npos)
                    word_end = paragraph.size();
                else if (word_end == at) {
                    ++word_end;
                    while (word_end < paragraph.size() && paragraph[word_end] == ' ')
                        ++word_end;
                    const auto next = paragraph.find(' ', word_end);
                    word_end = next == std::string_view::npos ? paragraph.size() : next;
                }
                const auto piece = paragraph.substr(at, word_end - at);
                if (line.empty() || measure(line + std::string(piece)) <= width) {
                    line += piece;
                    at = word_end;
                    // A single word wider than the line: broken between characters.
                    if (measure(line) > width) {
                        std::string head;
                        size_t from = 0;
                        std::string_view whole = line;
                        while (from < whole.size()) {
                            size_t next = from;
                            next_codepoint(whole, next);
                            if (!head.empty() &&
                                measure(head + std::string(whole.substr(from, next - from))) > width) {
                                lines.push_back(head);
                                head.clear();
                            }
                            head += whole.substr(from, next - from);
                            from = next;
                        }
                        line = head;
                    }
                    continue;
                }
                lines.push_back(line);
                line.clear();
                // A new line does not begin with the spaces that ended the last.
                while (at < paragraph.size() && paragraph[at] == ' ')
                    ++at;
            }
            lines.push_back(line);
        }
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return lines;
}
bool portable_filename(std::string_view name) {
    if (name.empty() || name.size() > 80 || name == "." || name == ".." ||
        name.find("..") != std::string_view::npos || name.back() == '.' || name.back() == ' ')
        return false;
    if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") !=
        std::string_view::npos)
        return false;
    // Windows matches a device name against the component up to its first dot, case-insensitively.
    std::string stem(name.substr(0, name.find('.')));
    for (char &c : stem)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    constexpr std::string_view devices[]{"con", "prn", "aux", "nul", "conin$", "conout$"};
    if (std::find(std::begin(devices), std::end(devices), stem) != std::end(devices))
        return false;
    // COM0-COM9 and LPT0-LPT9 are devices; longer names such as com10 are ordinary files.
    return !(stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' &&
             stem[3] <= '9');
}
uint64_t hash_bytes(std::span<const uint8_t> b) {
    uint64_t h = 14695981039346656037ULL;
    for (auto v : b) {
        h ^= v;
        h *= 1099511628211ULL;
    }
    return h;
}
namespace {
// Distinct for every call in every process, so two writers - two threads, or a running client and
// an editor saving the same progress or bindings file - never share a temporary. A shared fixed
// `<path>.tmp` let one truncate the other's half-written file and rename a mixture into place.
std::atomic<uint64_t> temporary_serial{0};
fs::path unique_temporary(const fs::path &path, const char *suffix) {
#ifdef _WIN32
    const auto process = static_cast<uint64_t>(GetCurrentProcessId());
#else
    const auto process = static_cast<uint64_t>(::getpid());
#endif
    auto temporary = path;
    temporary += "." + std::to_string(process) + "." + std::to_string(++temporary_serial) + suffix;
    return temporary;
}
// Creates `path` exclusively - failing if anything, a link included, is already there - writes
// `bytes`, and reaches the device before returning.
void write_new_file(const fs::path &path, const std::string &bytes) {
#ifdef _WIN32
    HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create temporary file: " + path.string());
    size_t written = 0;
    bool ok = true;
    while (ok && written < bytes.size()) {
        DWORD chunk = 0;
        const auto want = static_cast<DWORD>(std::min<size_t>(bytes.size() - written, 1u << 30));
        ok = WriteFile(handle, bytes.data() + written, want, &chunk, nullptr) && chunk > 0;
        written += chunk;
    }
    // WriteFile only reaches the OS cache; reach the device before anything is replaced.
    ok = ok && FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok)
        throw std::runtime_error("Write failed: " + path.string());
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0)
        throw std::runtime_error("Cannot create temporary file: " + path.string());
    size_t written = 0;
    bool ok = true;
    while (ok && written < bytes.size()) {
        const auto chunk = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (chunk < 0 && errno == EINTR)
            continue;
        ok = chunk > 0;
        if (ok)
            written += static_cast<size_t>(chunk);
    }
    ok = ok && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok)
        throw std::runtime_error("Write failed: " + path.string());
#endif
}
void replace_file(const fs::path &from, const fs::path &to) {
#ifdef _WIN32
    // Windows refuses to replace a file another handle holds open without FILE_SHARE_DELETE - a
    // second writer reading it for its backup, a reader, a virus scanner. Those holds are brief, so
    // the replacement is retried for up to about half a second before it is reported as a failure.
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return;
        const auto error = GetLastError();
        if ((error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) || attempt == 50)
            throw std::runtime_error("Atomic replacement failed: " + to.string());
        Sleep(10);
    }
#else
    fs::rename(from, to);
#endif
}
// The current contents, for the backup. On Windows the file is opened sharing read, write *and
// delete*, so this read never stops another writer replacing the file underneath it; an ifstream
// shares only read, and a second writer's replacement failed for as long as it was open.
bool read_for_backup(const fs::path &path, std::string &out) {
#ifdef _WIN32
    HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    bool ok = true;
    char buffer[65536];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(handle, buffer, sizeof(buffer), &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0)
            break;
        out.append(buffer, got);
    }
    CloseHandle(handle);
    return ok;
#else
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
#endif
}
} // namespace
void atomic_write(const fs::path &path, const std::string &bytes) {
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path());
    const auto temporary = unique_temporary(path, ".tmp");
    // Named to end in ".tmp" too, so everything that already skips temporaries skips this one.
    const auto backup_temporary = unique_temporary(path, ".bak.tmp");
    try {
        write_new_file(temporary, bytes);
        // The previous version, kept the same way: written whole to a temporary and renamed, so a
        // second writer backing up at the same moment cannot interleave with this one inside
        // `.bak`, and the copy is on the device before the file it preserves is replaced.
        std::string previous;
        const bool had_previous = read_for_backup(path, previous);
        if (had_previous) {
            auto backup = path; // built natively: a narrow round trip corrupts non-ASCII Windows paths
            backup += ".bak";
            write_new_file(backup_temporary, previous);
            replace_file(backup_temporary, backup);
        }
        replace_file(temporary, path);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        fs::remove(backup_temporary, ignored);
        throw;
    }
#ifndef _WIN32
    auto parent = path.parent_path().empty() ? fs::path(".") : path.parent_path();
    int dir = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir >= 0) {
        ::fsync(dir);
        ::close(dir);
    }
#endif
}
void remove_temporaries(const fs::path &path) {
    const auto directory = path.parent_path().empty() ? fs::path(".") : path.parent_path();
    const auto stem = path.filename().string() + ".";
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (name.size() > stem.size() && name.starts_with(stem) && name.ends_with(".tmp")) {
            std::error_code ignored;
            fs::remove(it->path(), ignored);
        }
    }
}
std::span<const std::string_view> capability_names() {
    static constexpr std::string_view names[]{"move",      "jump",        "attack",    "dodge", "guard",
                                              "parry",     "heal",        "slide",     "slam",  "dash",
                                              "wall_jump", "double_jump", "ledge_grab"};
    static_assert(std::size(names) == capability_count,
                  "Every Capability needs a name: the name is what content refers to it by, and an "
                  "unnamed one could neither be granted nor round-tripped");
    return names;
}
std::string_view capability_name(Capability capability) {
    return capability_names()[static_cast<size_t>(capability)];
}
Capability capability_from_name(std::string_view name) {
    const auto names = capability_names();
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name)
            return static_cast<Capability>(i);
    throw std::runtime_error("Unknown capability: " + std::string(name));
}
CapabilitySet CapabilitySet::from_bits(uint32_t bits) {
    if (bits >= (1u << capability_count))
        throw std::runtime_error("Capability set names a capability this build does not implement");
    CapabilitySet set = none();
    for (size_t i = 0; i < capability_count; ++i)
        if ((bits >> i) & 1u)
            set.grant(static_cast<Capability>(i));
    return set;
}
std::span<const std::string_view> archetype_names() {
    static constexpr std::string_view names[]{"sentinel", "brute", "kiter", "leaper", "echo"};
    return names;
}
bool kind_is_enemy(const std::string &kind) { return kind == "enemy" || kind == "boss"; }
bool kind_takes_art(std::string_view kind) {
    return kind == "enemy" || kind == "dummy" || kind == "boss" || kind == "npc" || kind == "decoration";
}
namespace {
// An archetype or patrol on something that never runs the AI would do nothing, which is exactly
// the silent outcome validation exists to refuse.
void check_behaviour(const Object &o, const std::string &where) {
    if (!o.archetype.empty()) {
        const auto names = archetype_names();
        if (std::find(names.begin(), names.end(), o.archetype) == names.end())
            throw std::runtime_error("Unknown archetype '" + o.archetype + "' on " + o.id + where);
        if (!kind_is_enemy(o.kind))
            throw std::runtime_error("An archetype on a " + o.kind + " would do nothing: " + o.id + where);
    }
    if (!std::isfinite(o.patrol) || o.patrol < 0 || o.patrol > limits::max_size)
        throw std::runtime_error("Patrol on " + o.id + where + " must be 0.." +
                                 std::to_string(static_cast<int>(limits::max_size)));
    if (o.patrol > 0 && !kind_is_enemy(o.kind))
        throw std::runtime_error("A patrol on a " + o.kind + " would do nothing: " + o.id + where);
    if (o.min_tier < 0 || o.min_tier > 7)
        throw std::runtime_error("A Renown tier on " + o.id + where + " is 0..7");
    if (o.min_tier > 0 && !kind_is_enemy(o.kind))
        throw std::runtime_error("A Renown tier on a " + o.kind + " would do nothing: " + o.id + where);
    // Motion: only a solid moves, and one that moves goes somewhere at a speed a player can meet.
    if (!finite(o.travel) || !std::isfinite(o.period) || o.period < 0 || o.period > 60 ||
        std::abs(o.travel.x) > limits::max_size || std::abs(o.travel.y) > limits::max_size)
        throw std::runtime_error("Travel on " + o.id + where +
                                 " must be within the room, over 0.5..60 seconds");
    if (o.period > 0 && (o.period < 0.5f || length(o.travel) < 1))
        throw std::runtime_error("A moving " + o.kind +
                                 " needs a travel and a period of at least 0.5 s: " + o.id + where);
    if (o.period == 0 && (o.travel.x != 0 || o.travel.y != 0))
        throw std::runtime_error("Travel without a period would never move: " + o.id + where);
    if (o.period > 0 && !kind_is_solid(o.kind))
        throw std::runtime_error("Only a wall or a platform can move: " + o.id + where);
    // Art names an actor file by its stem, and only on something drawn as a figure: on a wall it
    // would draw nothing, which is the silent outcome every rule here refuses.
    if (!o.art.empty() && !art_key(o.art))
        throw std::runtime_error("Art must name an actor file by its stem: " + o.id + where);
    if (!o.art.empty() && !kind_takes_art(o.kind))
        throw std::runtime_error("Art on a " + o.kind + " would draw nothing: " + o.id + where);
    // A relic teaches one traversal ability, once, and records `taken.<id>` in its room's facts: so
    // it names an ability a player learns rather than one every fighter has, and its ID is a save
    // key with room for the prefix.
    if (o.kind == "relic") {
        std::string learnable;
        bool teaches = false;
        for (const auto ability : traversal_abilities) {
            learnable += (learnable.empty() ? "" : ", ") + std::string(capability_name(ability));
            teaches = teaches || capability_name(ability) == o.target;
        }
        if (!teaches)
            throw std::runtime_error("Relic " + o.id + where + " must teach one of " + learnable + ", not '" +
                                     o.target + "'");
        constexpr size_t fact_prefix = 6; // "taken."
        if (o.id.size() + fact_prefix > limits::max_identifier ||
            o.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") !=
                std::string::npos)
            throw std::runtime_error("Relic ID must be a simple key of at most " +
                                     std::to_string(limits::max_identifier - fact_prefix) +
                                     " characters, since its taking is saved as a world fact: " + o.id +
                                     where);
    }
}
} // namespace
namespace {
bool simple_key(const std::string &part) {
    return !part.empty() && part.size() <= limits::max_identifier &&
           part.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
               std::string::npos;
}
// An entrance a door can name: a simple key, or simple keys joined by single dots - which is what
// prefab expansion makes of an entrance placed inside a prefab ("<placement>.<child>", R14).
bool entrance_key(const std::string &id) {
    if (id.empty() || id.size() > limits::max_identifier)
        return false;
    size_t start = 0;
    for (;;) {
        const auto dot = id.find('.', start);
        if (!simple_key(id.substr(start, dot == std::string::npos ? std::string::npos : dot - start)))
            return false;
        if (dot == std::string::npos)
            return true;
        start = dot + 1;
    }
}
} // namespace
bool art_key(std::string_view art) { return simple_key(std::string(art)); }
namespace {
// "room" or "room/entrance".
bool valid_door_target(const std::string &target) {
    const auto where = door_target(target);
    return simple_key(where.room) && (target.find('/') == std::string::npos || entrance_key(where.entrance));
}
} // namespace
DoorTarget door_target(std::string_view target) {
    const auto slash = target.find('/');
    if (slash == std::string_view::npos)
        return {std::string(target), {}};
    return {std::string(target.substr(0, slash)), std::string(target.substr(slash + 1))};
}
std::string door_target_string(const DoorTarget &target) {
    return target.entrance.empty() ? target.room : target.room + "/" + target.entrance;
}
bool kind_becomes_actor(const std::string &kind) {
    return kind == "spawn" || kind == "enemy" || kind == "dummy" || kind == "boss";
}
// Content form of a capability set: an array of names, in any order, without repeats.
//
// A repeat is refused rather than folded away. Writing one twice means the author believes the
// list says something it does not, and the two readings differ the moment the vocabulary grows.
CapabilitySet capabilities_from_json(const Json &value) {
    if (!value.is_array() || value.size() > capability_count)
        throw std::runtime_error("Capabilities must be an array of capability names");
    CapabilitySet set = CapabilitySet::none();
    for (const auto &entry : value) {
        if (!entry.is_string())
            throw std::runtime_error("Capabilities must be an array of capability names");
        const auto capability = capability_from_name(entry.get<std::string>());
        if (set.has(capability))
            throw std::runtime_error("Capability listed twice: " + entry.get<std::string>());
        set.grant(capability);
    }
    return set;
}
Json capabilities_to_json(const CapabilitySet &set) {
    Json out = Json::array();
    for (size_t i = 0; i < capability_count; ++i)
        if (set.has(static_cast<Capability>(i)))
            out.push_back(std::string(capability_name(static_cast<Capability>(i))));
    return out;
}
std::span<const std::string_view> object_kinds() {
    static constexpr std::string_view kinds[]{"wall",     "spawn",    "door", "checkpoint", "enemy",
                                              "dummy",    "boss",     "npc",  "decoration", "lever",
                                              "entrance", "platform", "relic"};
    return kinds;
}
std::span<const std::string_view> trigger_kinds() {
    static constexpr std::string_view kinds[]{"entry", "volume", "quest", "condition"};
    return kinds;
}
namespace {
// Save keys: quest, stage, flag, fact and item names become save entries, so they have that shape.
bool save_key(const std::string &value) {
    return !value.empty() && value.size() <= limits::max_identifier &&
           value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") ==
               std::string::npos;
}
// A fact is written "room/fact" in the files: one string reads as one place.
std::pair<std::string, std::string> split_fact(const std::string &value, const std::string &where) {
    const auto slash = value.find('/');
    if (slash == std::string::npos || value.find('/', slash + 1) != std::string::npos)
        throw std::runtime_error(where + ": a fact is written \"room/fact\", not '" + value + "'");
    return {value.substr(0, slash), value.substr(slash + 1)};
}
} // namespace
Condition condition_from_json(const Json &j, const std::string &where) {
    if (!j.is_object())
        throw std::runtime_error(where + ": a condition is an object");
    Condition c;
    if (j.contains("quest")) {
        c.kind = ConditionKind::Stage;
        c.a = j.at("quest").get<std::string>();
        c.b = j.at("stage").get<std::string>();
    } else if (j.contains("flag")) {
        c.kind = ConditionKind::Flag;
        c.a = j.at("flag").get<std::string>();
    } else if (j.contains("no_flag")) {
        c.kind = ConditionKind::NoFlag;
        c.a = j.at("no_flag").get<std::string>();
    } else if (j.contains("fact")) {
        c.kind = ConditionKind::Fact;
        std::tie(c.a, c.b) = split_fact(j.at("fact").get<std::string>(), where);
    } else if (j.contains("item")) {
        c.kind = ConditionKind::Item;
        c.a = j.at("item").get<std::string>();
        c.amount = j.value("count", 1);
    } else if (j.contains("renown")) {
        c.kind = ConditionKind::Renown;
        c.amount = j.at("renown").get<int>();
    } else
        throw std::runtime_error(where +
                                 ": a condition is one of quest+stage, flag, no_flag, fact, item or renown");
    return c;
}
Json condition_json(const Condition &c) {
    switch (c.kind) {
    case ConditionKind::Stage:
        return {{"quest", c.a}, {"stage", c.b}};
    case ConditionKind::Flag:
        return {{"flag", c.a}};
    case ConditionKind::NoFlag:
        return {{"no_flag", c.a}};
    case ConditionKind::Fact:
        return {{"fact", c.a + "/" + c.b}};
    case ConditionKind::Item:
        return c.amount == 1 ? Json{{"item", c.a}} : Json{{"item", c.a}, {"count", c.amount}};
    case ConditionKind::Renown:
        return {{"renown", c.amount}};
    }
    return {};
}
void check_condition(const Condition &c, const std::string &where) {
    auto key = [&](const std::string &value, const char *what) {
        if (!save_key(value))
            throw std::runtime_error(where + " " + what + ": '" + value +
                                     "' is not a key (letters, digits, '_', '-', '.')");
    };
    switch (c.kind) {
    case ConditionKind::Stage:
        key(c.a, "quest");
        key(c.b, "stage");
        break;
    case ConditionKind::Fact:
        key(c.a, "fact room");
        key(c.b, "fact");
        break;
    case ConditionKind::Item:
        key(c.a, "item");
        if (c.amount < 1 || c.amount > 99)
            throw std::runtime_error(where + ": an item count is 1..99");
        break;
    case ConditionKind::Renown:
        if (c.amount < 1 || c.amount > 7)
            throw std::runtime_error(where + " waits for a Renown tier outside I..VII");
        break;
    case ConditionKind::Flag:
    case ConditionKind::NoFlag:
        key(c.a, "flag");
        break;
    }
}
namespace {
// Defined with the rest of the prefab code below; declared here because validate_scene needs it
// and moving the whole prefab section above the scene section would bury it.
void validate_instances(const std::vector<PrefabInstance> &instances, const char *where);
} // namespace
void validate_scene(const Scene &s) {
    if (s.id.empty() || s.id.size() > limits::max_identifier ||
        s.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
            std::string::npos ||
        s.objects.size() > limits::max_objects)
        throw std::runtime_error("Invalid scene id or object capacity");
    // Atmosphere: presentation, but still content the loader bounds.
    if (s.backdrops.size() > limits::max_backdrops || s.lights.size() > limits::max_lights)
        throw std::runtime_error("Room '" + s.id + "' has more than " +
                                 std::to_string(limits::max_backdrops) + " backdrops or " +
                                 std::to_string(limits::max_lights) + " lights");
    auto colour_ok = [](Color c) {
        for (const float v : {c.r, c.g, c.b, c.a})
            if (!std::isfinite(v) || v < 0 || v > 1)
                return false;
        return true;
    };
    auto placed = [](Vec2 p) {
        return finite(p) && std::abs(p.x) <= limits::max_position && std::abs(p.y) <= limits::max_position;
    };
    if (!colour_ok(s.ambient))
        throw std::runtime_error("Room '" + s.id + "' ambient light is a colour with channels 0..1");
    if (!s.endless.empty() && std::none_of(s.objects.begin(), s.objects.end(), [&](const Object &o) {
            return o.id == s.endless && o.kind == "wall" && o.travel.x == 0 && o.travel.y == 0 &&
                   o.period <= 0;
        }))
        throw std::runtime_error("Room '" + s.id + "' is endless along '" + s.endless +
                                 "', which is not one of its still walls");
    for (size_t i = 0; i < s.backdrops.size(); ++i) {
        const auto &b = s.backdrops[i];
        const std::string who = "Room '" + s.id + "' backdrop " + std::to_string(i + 1);
        const fs::path image(b.image);
        bool portable = !b.image.empty() && image.is_relative() && image.extension() == ".png";
        for (const auto &part : image)
            portable = portable && portable_filename(part.string());
        if (!portable)
            throw std::runtime_error(who + ": its image is a .png inside the project, not '" + b.image + "'");
        if (!placed(b.position) || !finite(b.parallax) || b.parallax.x < 0 || b.parallax.y < 0 ||
            b.parallax.x > limits::max_parallax || b.parallax.y > limits::max_parallax ||
            !std::isfinite(b.scale) || b.scale < limits::min_backdrop_scale ||
            b.scale > limits::max_backdrop_scale || !colour_ok(b.tint))
            throw std::runtime_error(
                who + ": its position, parallax (0..2), scale (0.05..20) or tint is out of range");
        if (!b.motion.empty() &&
            std::find(backdrop_motions.begin(), backdrop_motions.end(), b.motion) == backdrop_motions.end())
            throw std::runtime_error(who + ": its motion is \"hand\" or \"bell\", not '" + b.motion + "'");
        if (!finite(b.pivot) || std::abs(b.pivot.x) > limits::max_position ||
            std::abs(b.pivot.y) > limits::max_position || !std::isfinite(b.angle) || std::abs(b.angle) > 360)
            throw std::runtime_error(who + ": its pivot or its angle (-360..360 degrees) is out of range");
        if (b.repeat_x && (!b.motion.empty() || b.angle != 0))
            throw std::runtime_error(who + ": a repeated backdrop does not turn");
    }
    for (size_t i = 0; i < s.lights.size(); ++i) {
        const auto &l = s.lights[i];
        if (!placed(l.position) || !std::isfinite(l.radius) || l.radius < limits::min_light_radius ||
            l.radius > limits::max_light_radius || !std::isfinite(l.intensity) || l.intensity < 0 ||
            l.intensity > limits::max_light_intensity || !colour_ok(l.color))
            throw std::runtime_error(
                "Room '" + s.id + "' light " + std::to_string(i + 1) +
                ": its position, radius (8..4000), intensity (0..4) or colour is out of range");
    }
    // Tile terrain.
    if (s.tiles.size() > limits::max_tile_maps)
        throw std::runtime_error("Room '" + s.id + "' has more than " +
                                 std::to_string(limits::max_tile_maps) + " tile maps");
    for (size_t i = 0; i < s.tiles.size(); ++i) {
        const auto &m = s.tiles[i];
        const std::string who = "Room '" + s.id + "' tile map " + std::to_string(i + 1);
        const fs::path image(m.tileset);
        bool portable = !m.tileset.empty() && image.is_relative() && image.extension() == ".png";
        for (const auto &part : image)
            portable = portable && portable_filename(part.string());
        if (!portable)
            throw std::runtime_error(who + ": its tileset is a .png inside the project, not '" + m.tileset +
                                     "'");
        if (m.tile < limits::min_tile_pixels || m.tile > limits::max_tile_pixels || !std::isfinite(m.cell) ||
            m.cell < limits::grid || m.cell > limits::max_tile_cell || std::fmod(m.cell, limits::grid) != 0 ||
            !placed(m.origin) || m.columns < 1 || m.rows < 1 || m.columns > limits::max_tile_grid ||
            m.rows > limits::max_tile_grid)
            throw std::runtime_error(who +
                                     ": its tile (4..256 px), cell (a multiple of 8 up to 256), origin or "
                                     "grid (1..512 a side) is out of range");
        if (m.cells.size() != static_cast<size_t>(m.columns) * static_cast<size_t>(m.rows))
            throw std::runtime_error(who + ": its cells do not fill its grid");
        if (!placed(m.origin +
                    Vec2{m.cell * static_cast<float>(m.columns), m.cell * static_cast<float>(m.rows)}))
            throw std::runtime_error(who + " reaches past the edge of the world");
        for (const auto cell : m.cells)
            if (cell > limits::max_tile)
                throw std::runtime_error(who + " names a tile past " + std::to_string(limits::max_tile));
        for (const auto &list : {m.solid, m.platforms})
            for (const auto tile : list)
                if (tile < 1 || tile > limits::max_tile)
                    throw std::runtime_error(who + ": a solid or platform tile is 1.." +
                                             std::to_string(limits::max_tile));
        for (const auto tile : m.solid)
            if (std::find(m.platforms.begin(), m.platforms.end(), tile) != m.platforms.end())
                throw std::runtime_error(who + ": tile " + std::to_string(tile) +
                                         " cannot be both solid and a platform");
    }
    // The terrain's walls carry IDs the room's own objects may not: a room as played holds exactly
    // the ones its tiles make (`expand_prefabs` appends them), and nothing else may claim one.
    if (std::any_of(s.objects.begin(), s.objects.end(), [](const Object &o) { return from_tiles(o); })) {
        const auto made = tile_objects(s);
        for (const auto &o : s.objects)
            if (from_tiles(o) && std::find(made.begin(), made.end(), o) == made.end())
                throw std::runtime_error("Object IDs beginning '~tiles.' are the tile terrain's: " + o.id);
    }
    if (!s.music.empty() &&
        (s.music.size() > limits::max_identifier ||
         s.music.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") !=
             std::string::npos))
        throw std::runtime_error("Room '" + s.id + "' music names a track by its ID, not '" + s.music + "'");
    std::set<std::string> ids;
    const auto kinds = object_kinds();
    auto known_kind = [&kinds](const std::string &kind) {
        return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
    };
    for (const auto &o : s.objects) {
        if (o.id.empty() || o.id.size() > limits::max_identifier || !ids.insert(o.id).second ||
            !known_kind(o.kind) || !finite(o.position) || !finite(o.size) || o.size.x < limits::min_size ||
            o.size.y < limits::min_size || o.size.x > limits::max_size || o.size.y > limits::max_size ||
            std::abs(o.position.x) > limits::max_position || std::abs(o.position.y) > limits::max_position ||
            o.layer < 0 || o.layer > limits::max_layer)
            throw std::runtime_error("Invalid object: " + o.id);
        if (o.kind == "door" && !valid_door_target(o.target))
            throw std::runtime_error("Door requires a simple stable scene ID, optionally /entrance");
        // An entrance is where a door arrives, and its ID is what a door names after the slash.
        if (o.kind == "entrance" && !entrance_key(o.id))
            throw std::runtime_error("Entrance ID must be a key a door can name: " + o.id);
        // A capability grant on a wall or a decoration would do nothing, which is the one outcome
        // a validated capability set exists to prevent. Refused by name rather than dropped.
        if (!o.capabilities.complete() && !kind_becomes_actor(o.kind))
            throw std::runtime_error("Capabilities on a " + o.kind + " would do nothing: " + o.id);
        // And a facing on scenery would do nothing either, for the same reason. An entrance's facing
        // is the way the player faces on arriving through it.
        if (o.facing != 1 && !kind_becomes_actor(o.kind) && o.kind != "entrance")
            throw std::runtime_error("A facing on a " + o.kind + " would do nothing: " + o.id);
        check_behaviour(o, "");
    }
    // A lever records its own ID as a fact about this room, so the ID has to be usable as a save
    // key; and the gate it opens is a wall *in this room*, because a lever that named a wall
    // somewhere else would record a fact nothing ever reads. No target is allowed - the fact is
    // still recorded, for whatever else comes to read it.
    // A boss's defeat is a world fact too, "defeated.<id>", so its ID has the same shape and
    // leaves room for the prefix. Its target names a boss definition, which the room cannot check -
    // the project's boss file can, and the cooker and the editor do.
    for (const auto &o : s.objects) {
        if (o.kind != "boss")
            continue;
        constexpr size_t fact_prefix = 9; // "defeated."
        if (o.id.size() + fact_prefix > limits::max_identifier ||
            o.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") !=
                std::string::npos)
            throw std::runtime_error("Boss ID must be a simple key of at most " +
                                     std::to_string(limits::max_identifier - fact_prefix) +
                                     " characters, since its defeat is saved as a world fact: " + o.id);
        if (!o.target.empty() &&
            o.target.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") !=
                std::string::npos)
            throw std::runtime_error("Boss " + o.id + " names definition '" + o.target +
                                     "', which is not a simple key");
    }
    for (const auto &o : s.objects) {
        if (o.kind != "lever")
            continue;
        if (o.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") !=
            std::string::npos)
            throw std::runtime_error("Lever ID must be a simple key, since it is saved as a world fact: " +
                                     o.id);
        if (!o.target.empty() && std::none_of(s.objects.begin(), s.objects.end(), [&](const Object &gate) {
                return gate.id == o.target && gate.kind == "wall";
            }))
            throw std::runtime_error("Lever " + o.id + " opens '" + o.target +
                                     "', which is not a wall in room " + s.id);
    }
    // Wind: a strength is 0 (still) to 1, and a zone a real region with an ID of its own.
    if (!std::isfinite(s.wind) || s.wind < 0 || s.wind > 1)
        throw std::runtime_error("Room " + s.id + " has a wind outside 0..1");
    if (s.wind_zones.size() > limits::max_wind_zones)
        throw std::runtime_error("Wind zone capacity exceeded");
    std::set<std::string> wind_ids;
    for (const auto &z : s.wind_zones) {
        if (z.id.empty() || z.id.size() > limits::max_identifier || !wind_ids.insert(z.id).second)
            throw std::runtime_error("Invalid wind zone id: " + z.id);
        if (!finite({z.area.x, z.area.y}) || !finite({z.area.w, z.area.h}) || z.area.w <= 0 ||
            z.area.h <= 0 || std::abs(z.area.x) > limits::max_position ||
            std::abs(z.area.y) > limits::max_position ||
            std::abs(z.area.x + z.area.w) > limits::max_position ||
            std::abs(z.area.y + z.area.h) > limits::max_position)
            throw std::runtime_error("Invalid wind zone area: " + z.id);
        if (!std::isfinite(z.strength) || z.strength < 0 || z.strength > 1)
            throw std::runtime_error("Wind zone " + z.id + " has a strength outside 0..1");
    }
    if (s.camera_zones.size() > limits::max_camera_zones)
        throw std::runtime_error("Camera zone capacity exceeded");
    std::set<std::string> zone_ids;
    for (const auto &z : s.camera_zones) {
        // A zone is addressed by ID from a trigger and from the editor overlay, so a duplicate
        // or empty ID would let one zone silently stand in for another.
        if (z.id.empty() || z.id.size() > limits::max_identifier || !zone_ids.insert(z.id).second)
            throw std::runtime_error("Invalid camera zone id: " + z.id);
        // Inverted or zero-area regions are rejected outright rather than reported, because a
        // zone the focus can never be inside is indistinguishable from one that is missing.
        if (!finite({z.area.x, z.area.y}) || !finite({z.area.w, z.area.h}) || z.area.w <= 0 ||
            z.area.h <= 0 || std::abs(z.area.x) > limits::max_position ||
            std::abs(z.area.y) > limits::max_position ||
            std::abs(z.area.x + z.area.w) > limits::max_position ||
            std::abs(z.area.y + z.area.h) > limits::max_position)
            throw std::runtime_error("Invalid camera zone area: " + z.id);
        if (!std::isfinite(z.blend_in) || !std::isfinite(z.blend_out) || z.blend_in < 0 || z.blend_out < 0 ||
            z.blend_in > limits::max_blend || z.blend_out > limits::max_blend ||
            z.priority < -limits::max_priority || z.priority > limits::max_priority)
            throw std::runtime_error("Invalid camera zone blend or priority: " + z.id);
        auto in_range = [](const std::optional<float> &v, float low, float high) {
            return !v || (std::isfinite(*v) && *v >= low && *v <= high);
        };
        if (!in_range(z.look_ahead, 0, limits::max_look_ahead) ||
            !in_range(z.follow_rate, limits::min_follow_rate, limits::max_follow_rate) ||
            !in_range(z.zoom, limits::min_zoom, limits::max_zoom) ||
            !in_range(z.vertical_bias, -limits::max_vertical_bias, limits::max_vertical_bias))
            throw std::runtime_error("Camera zone override out of range: " + z.id);
        if (z.dead_zone && (!finite(*z.dead_zone) || z.dead_zone->x < 0 || z.dead_zone->y < 0 ||
                            z.dead_zone->x > limits::max_dead_zone || z.dead_zone->y > limits::max_dead_zone))
            throw std::runtime_error("Camera zone dead zone out of range: " + z.id);
        if (z.lock && (!finite(*z.lock) || std::abs(z.lock->x) > limits::max_position ||
                       std::abs(z.lock->y) > limits::max_position))
            throw std::runtime_error("Camera zone lock out of range: " + z.id);
        if (z.bounds && (!finite({z.bounds->x, z.bounds->y}) || !finite({z.bounds->w, z.bounds->h}) ||
                         z.bounds->w <= 0 || z.bounds->h <= 0))
            throw std::runtime_error("Camera zone bounds are empty or inverted: " + z.id);
    }
    if (s.prefabs.size() > limits::max_objects)
        throw std::runtime_error("Prefab placement capacity exceeded");
    validate_instances(s.prefabs, s.id.c_str());
    if (s.cutscene_triggers.size() > limits::max_cutscene_triggers)
        throw std::runtime_error("Cutscene trigger capacity exceeded");
    std::set<std::string> trigger_ids;
    const auto whens = trigger_kinds();
    for (const auto &t : s.cutscene_triggers) {
        if (t.id.empty() || t.id.size() > limits::max_identifier || !trigger_ids.insert(t.id).second)
            throw std::runtime_error("Invalid cutscene trigger id: " + t.id);
        if (std::find(whens.begin(), whens.end(), t.when) == whens.end())
            throw std::runtime_error("Unknown cutscene trigger kind: " + t.when);
        // The cutscene ID becomes a filename, so it goes through the same portability gate as
        // every other authored name. A Linux-authored trigger naming `con` would otherwise
        // produce a room that cannot be opened on Windows.
        if (t.cutscene.empty() || t.cutscene.size() > limits::max_identifier ||
            t.cutscene.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos ||
            !portable_filename(t.cutscene + ".cutscene.json"))
            throw std::runtime_error("Cutscene trigger requires a simple portable cutscene ID: " + t.id);
        if (t.when == "volume" &&
            (!finite({t.area.x, t.area.y}) || !finite({t.area.w, t.area.h}) || t.area.w <= 0 ||
             t.area.h <= 0 || std::abs(t.area.x) > limits::max_position ||
             std::abs(t.area.y) > limits::max_position))
            throw std::runtime_error("Cutscene trigger volume is empty or inverted: " + t.id);
        if (t.when == "quest" && (t.quest < 1 || t.quest > 999))
            throw std::runtime_error("Cutscene trigger quest threshold out of range: " + t.id);
        if (t.conditions.size() > max_trigger_conditions)
            throw std::runtime_error("Cutscene trigger " + t.id + " has more than " +
                                     std::to_string(max_trigger_conditions) + " conditions");
        // A "condition" trigger fires on its conditions alone, so without one it would fire the
        // moment the room loads - which is what "entry" says, and says plainly.
        if (t.when == "condition" && t.conditions.empty())
            throw std::runtime_error("Cutscene trigger " + t.id + " fires on its conditions and has none");
        for (const auto &c : t.conditions)
            check_condition(c, "Cutscene trigger " + t.id);
    }
}
const AnimationClip *Animation::clip(std::string_view id) const {
    for (const auto &entry : clips)
        if (entry.id == id)
            return &entry;
    return nullptr;
}
const Attachment *Animation::attachment(std::string_view id) const {
    for (const auto &entry : attachments)
        if (entry.id == id)
            return &entry;
    return nullptr;
}
const Attachment *Animation::attachment(std::string_view id, const AnimationFrame &frame) const {
    // The frame wins. A socket that stays put while the arm swings is the most visible way
    // attached equipment looks wrong, so a per-frame override is the whole point of having one.
    for (const auto &entry : frame.attachments)
        if (entry.id == id)
            return &entry;
    return attachment(id);
}
int AnimationClip::pass_ticks() const {
    int total = 0;
    for (const auto &frame : frames)
        total += frame.ticks > 0 ? frame.ticks : std::max(1, ticks_per_frame);
    return std::max(total, 1);
}
int AnimationClip::lead_ticks() const {
    if (loop_from <= 0)
        return 0;
    int total = 0;
    for (size_t i = 0; i < frames.size() && i < static_cast<size_t>(loop_from); ++i)
        total += frames[i].ticks > 0 ? frames[i].ticks : std::max(1, ticks_per_frame);
    // Never the whole clip: a lead-in that consumed every frame would leave nothing to cycle,
    // and the clip would freeze on its last frame while claiming to loop.
    return std::min(total, pass_ticks() - 1);
}
SampledFrame sample_clip(const AnimationClip &clip, int tick) {
    if (clip.frames.empty())
        return {};
    // Speed scales time rather than the authored holds: an artist tuning a hold should not have
    // their number silently multiplied, and a speed of 2 should mean "twice as fast" exactly.
    const float rate = std::isfinite(clip.speed) && clip.speed > 0 ? clip.speed : 1.0f;
    const float time = static_cast<float>(std::max(0, tick)) * rate;
    // Preserve the float timing used by ordinary clips, but reduce or clamp it before any
    // integer conversion: a long-running clip at speed 8 can exceed INT_MAX. The fallback
    // also keeps an unvalidated, very large positive speed from producing infinity.
    const double scaled = std::floor(std::isfinite(time) ? static_cast<double>(time)
                                                         : static_cast<double>(std::max(0, tick)) * rate);
    const int pass = clip.pass_ticks();
    const auto count = clip.frames.size();
    int at = 0;
    bool backwards = false;
    switch (clip.loop) {
    case LoopMode::Once:
        at = static_cast<int>(std::min(scaled, static_cast<double>(pass - 1)));
        // Hold the last frame rather than snapping to the first.
        break;
    case LoopMode::Loop: {
        // The lead-in plays once; after it, only the remainder cycles. With no lead-in this is
        // exactly the plain modulo it has always been.
        const int lead = clip.lead_ticks();
        at = scaled < lead ? static_cast<int>(scaled)
                           : lead + static_cast<int>(std::fmod(scaled - lead, std::max(1, pass - lead)));
        break;
    }
    case LoopMode::PingPong: {
        // A full cycle is forwards then backwards. The two end frames are shared rather than
        // shown twice, which is what stops a ping-pong stuttering at each turn. A lead-in is
        // played once and then excluded from the swing, so the wind-up does not replay every
        // time the pose comes back.
        const int lead = clip.lead_ticks();
        if (scaled < lead) {
            at = static_cast<int>(scaled);
            break;
        }
        const int span = std::max(1, pass - lead);
        const double cycle = static_cast<double>(span) * 2;
        const double within = std::fmod(scaled - lead, cycle);
        backwards = within >= span;
        at = lead + static_cast<int>(backwards ? cycle - 1 - within : within);
        break;
    }
    }
    int elapsed = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto &frame = clip.frames[i];
        elapsed += frame.ticks > 0 ? frame.ticks : std::max(1, clip.ticks_per_frame);
        if (at < elapsed)
            return {frame.atlas, frame.offset, i,          frame.flip_x, frame.flip_y,
                    frame.scale, frame.angle,  frame.tint, frame.pivot};
    }
    const auto &last = clip.frames.back();
    return {last.atlas, last.offset, count - 1, last.flip_x, last.flip_y,
            last.scale, last.angle,  last.tint, last.pivot};
}
std::vector<std::string> clip_events(const AnimationClip &clip, int after, int until) {
    std::vector<std::string> fired;
    if (until <= after)
        return fired;
    for (const auto &event : clip.events)
        if (event.tick > after && event.tick <= until)
            fired.push_back(event.id);
    return fired;
}
namespace {
LoopMode loop_from_json(const Json &value) {
    // Accepts the bool the v2 files already contain, so no authored file has to change to keep
    // working, and a string for the modes a bool cannot express.
    if (value.is_boolean())
        return value.get<bool>() ? LoopMode::Loop : LoopMode::Once;
    const std::string name = value.get<std::string>();
    if (name == "loop")
        return LoopMode::Loop;
    if (name == "once")
        return LoopMode::Once;
    if (name == "pingpong")
        return LoopMode::PingPong;
    throw std::runtime_error("Unknown loop mode: " + name);
}
std::vector<Attachment> attachments_from_json(const Json &object) {
    std::vector<Attachment> out;
    std::set<std::string> ids;
    for (const auto &[id, point] : object.items()) {
        if (id.empty() || id.size() > limits::max_identifier || !ids.insert(id).second)
            throw std::runtime_error("Invalid attachment id: " + id);
        Attachment attachment{id, {point.at(0), point.at(1)}};
        if (!finite(attachment.point) || attachment.point.x < 0 || attachment.point.x > 128 ||
            attachment.point.y < 0 || attachment.point.y > 128)
            throw std::runtime_error("Attachment outside the canvas: " + id);
        out.push_back(std::move(attachment));
    }
    // Sorted so a loaded animation never depends on JSON object order.
    std::sort(out.begin(), out.end(), [](const Attachment &x, const Attachment &y) { return x.id < y.id; });
    return out;
}
} // namespace
namespace {
Json attachments_json(const std::vector<Attachment> &attachments) {
    Json out = Json::object();
    for (const auto &attachment : attachments)
        out[attachment.id] = {attachment.point.x, attachment.point.y};
    return out;
}
const char *loop_name(LoopMode mode) {
    switch (mode) {
    case LoopMode::Once:
        return "once";
    case LoopMode::PingPong:
        return "pingpong";
    case LoopMode::Loop:
        break;
    }
    return "loop";
}
} // namespace
Json animation_json(const Animation &animation, const Json &original) {
    validate_animation(animation);
    Json j = original;
    j["feet"] = {animation.feet.x, animation.feet.y};
    j["facing_rows"] = {{"right", animation.right_row}, {"left", animation.left_row}};
    // Written only when set, so every file drawn with a row per facing is byte-identical.
    if (animation.mirror_left)
        j["mirror_left"] = true;
    else
        j.erase("mirror_left");
    if (animation.grid != limits::atlas_columns)
        j["grid"] = animation.grid;
    else
        j.erase("grid");
    // The canvas, whole units written as integers so every drawn file keeps its [128, 128] byte for byte.
    if (animation.canvas == std::round(animation.canvas))
        j["canvas"] = {static_cast<int>(animation.canvas), static_cast<int>(animation.canvas)};
    else
        j["canvas"] = {animation.canvas, animation.canvas};
    // Packed frames are the bake's; written back as they came so an editor save keeps them. The page
    // file names (`pages`) are the file's own metadata and stay as written.
    if (!animation.regions.empty()) {
        Json regions = Json::array();
        for (const auto &r : animation.regions)
            regions.push_back({r.page, r.x, r.y, r.w, r.h, r.left, r.top});
        j["regions"] = std::move(regions);
        j["cell_pixels"] = animation.cell_pixels;
        j["page_size"] = {animation.page_size.x, animation.page_size.y};
    } else {
        j.erase("regions");
        j.erase("cell_pixels");
        j.erase("page_size");
    }
    j["attachments"] = attachments_json(animation.attachments);
    Json clips = Json::object();
    for (const auto &clip : animation.clips) {
        Json body = {{"ticks_per_frame", clip.ticks_per_frame}, {"loop", loop_name(clip.loop)}};
        if (clip.speed != 1)
            body["speed"] = clip.speed;
        if (clip.loop_from != 0)
            body["loop_from"] = clip.loop_from;
        Json frames = Json::array();
        for (const auto &frame : clip.frames) {
            // A frame that says nothing beyond its cell writes as a bare integer, so a file
            // that used none of the per-frame fields comes back byte-comparable.
            // "A frame that says nothing but its cell" compared against a default-built one,
            // so a field added later cannot be forgotten here and silently dropped on save.
            AnimationFrame plain;
            plain.atlas = frame.atlas;
            if (frame == plain) {
                frames.push_back(frame.atlas);
                continue;
            }
            Json entry = {{"atlas", frame.atlas}};
            if (frame.ticks != 0)
                entry["ticks"] = frame.ticks;
            if (!(frame.offset == Vec2{}))
                entry["offset"] = {frame.offset.x, frame.offset.y};
            if (!frame.attachments.empty())
                entry["attachments"] = attachments_json(frame.attachments);
            if (frame.flip_x)
                entry["flip_x"] = true;
            if (frame.flip_y)
                entry["flip_y"] = true;
            if (!(frame.scale == Vec2{1, 1}))
                entry["scale"] = {frame.scale.x, frame.scale.y};
            if (frame.angle != 0)
                entry["angle"] = frame.angle;
            if (frame.pivot)
                entry["pivot"] = {frame.pivot->x, frame.pivot->y};
            if (!(frame.tint == Color{}))
                entry["tint"] = {frame.tint.r, frame.tint.g, frame.tint.b, frame.tint.a};
            frames.push_back(std::move(entry));
        }
        body["frames"] = std::move(frames);
        if (!clip.events.empty()) {
            Json events = Json::array();
            for (const auto &event : clip.events)
                events.push_back({{"tick", event.tick}, {"id", event.id}});
            body["events"] = std::move(events);
        }
        clips[clip.id] = std::move(body);
    }
    j["animations"] = std::move(clips);
    return j;
}
AtlasCell atlas_cell(int atlas, int facing_row, int grid) {
    const int side = std::max(1, grid);
    return {atlas % side, facing_row + atlas / side};
}
FrameImage frame_image(const Animation &a, int atlas, int row) {
    FrameImage image;
    if (!a.regions.empty()) {
        // A packed frame: its own page and rectangle, placed where it was cut from on the canvas.
        const auto &r =
            a.regions[static_cast<size_t>(std::clamp(atlas, 0, static_cast<int>(a.regions.size()) - 1))];
        const float pw = std::max(1.0f, a.page_size.x), ph = std::max(1.0f, a.page_size.y);
        const float unit = a.canvas / static_cast<float>(std::max(1, a.cell_pixels));
        image.page = r.page;
        image.uv = {static_cast<float>(r.x) / pw, static_cast<float>(r.y) / ph, static_cast<float>(r.w) / pw,
                    static_cast<float>(r.h) / ph};
        image.canvas = {static_cast<float>(r.left) * unit, static_cast<float>(r.top) * unit,
                        static_cast<float>(r.w) * unit, static_cast<float>(r.h) * unit};
        return image;
    }
    const float cell = 1.0f / static_cast<float>(std::max(1, a.grid));
    const auto at = atlas_cell(atlas, row, a.grid);
    image.uv = {static_cast<float>(at.column) * cell, static_cast<float>(at.row) * cell, cell, cell};
    return image;
}
void validate_animation(const Animation &a) {
    if (a.grid < 1 || a.grid > limits::max_atlas_grid)
        throw std::runtime_error("Animation grid is 1 to 16 cells a side");
    for (int index : {a.right_row, a.left_row})
        if (index < 0 || index > a.grid - 1)
            throw std::runtime_error("Animation row outside atlas");
    if (a.mirror_left && a.left_row != a.right_row)
        throw std::runtime_error("A mirrored left facing draws the right facing's row: make them the same");
    // A drawn file's canvas is 128 units; only a packed one may be larger.
    if (!std::isfinite(a.canvas) || a.canvas < limits::max_canvas ||
        a.canvas > (a.regions.empty() ? limits::max_canvas : limits::max_packed_canvas))
        throw std::runtime_error("The canvas is 128 units, or up to 512 for packed frames");
    if (!finite(a.feet) || a.feet.x < 0 || a.feet.x > a.canvas || a.feet.y < 0 || a.feet.y > a.canvas)
        throw std::runtime_error("Invalid feet pivot");
    // Packed frames: every region on a page the file has, inside it, and inside the canvas it was cut
    // from - an empty one (nothing drawn that frame) as well, at no size. A file of regions draws one facing
    // and mirrors it (the bake renders the right side only).
    if (a.page_count < 1 || a.page_count > limits::max_pages)
        throw std::runtime_error("An actor file has 1 to " + std::to_string(limits::max_pages) + " pages");
    if (!a.regions.empty()) {
        if (a.regions.size() > limits::max_regions)
            throw std::runtime_error("Too many packed frames");
        if (!a.mirror_left)
            throw std::runtime_error("Packed frames draw one facing: set mirror_left");
        if (a.cell_pixels < 1 || a.cell_pixels > limits::max_page_pixels || !finite(a.page_size) ||
            a.page_size.x < 1 || a.page_size.y < 1 || a.page_size.x > limits::max_page_pixels ||
            a.page_size.y > limits::max_page_pixels)
            throw std::runtime_error("Packed frames need a canvas size and a page size");
        for (size_t i = 0; i < a.regions.size(); ++i) {
            const auto &r = a.regions[i];
            if (r.page < 0 || r.page >= a.page_count || r.x < 0 || r.y < 0 || r.w < 0 || r.h < 0 ||
                static_cast<double>(r.x) + r.w > a.page_size.x ||
                static_cast<double>(r.y) + r.h > a.page_size.y || r.left < 0 || r.top < 0 ||
                static_cast<int64_t>(r.left) + r.w > a.cell_pixels ||
                static_cast<int64_t>(r.top) + r.h > a.cell_pixels)
                throw std::runtime_error("Packed frame " + std::to_string(i) +
                                         " is off its page or its canvas");
        }
    } else if (a.page_count != 1)
        throw std::runtime_error("Only packed frames use more than one page");
    auto check_attachments = [&a](const std::vector<Attachment> &attachments, const char *where) {
        std::set<std::string> ids;
        for (const auto &attachment : attachments) {
            if (attachment.id.empty() || attachment.id.size() > limits::max_identifier ||
                !ids.insert(attachment.id).second)
                throw std::runtime_error(std::string("Invalid attachment id in ") + where);
            if (!finite(attachment.point) || attachment.point.x < 0 || attachment.point.x > a.canvas ||
                attachment.point.y < 0 || attachment.point.y > a.canvas)
                throw std::runtime_error("Attachment outside the canvas: " + attachment.id);
        }
    };
    check_attachments(a.attachments, "the actor");
    if (!a.attachment("hand") || !a.attachment("hand_left"))
        throw std::runtime_error("An actor needs both hand attachments");
    if (a.clips.size() > limits::max_clips)
        throw std::runtime_error("Clip capacity exceeded");
    std::set<std::string> clip_ids;
    for (const auto &clip : a.clips) {
        if (clip.id.empty() || clip.id.size() > limits::max_identifier || !clip_ids.insert(clip.id).second)
            throw std::runtime_error("Invalid clip id: " + clip.id);
        if (clip.ticks_per_frame < 1 || clip.ticks_per_frame > limits::max_clip_ticks ||
            clip.frames.empty() || clip.frames.size() > limits::max_clip_frames ||
            clip.events.size() > limits::max_clip_events || !std::isfinite(clip.speed) ||
            clip.speed < limits::min_clip_speed || clip.speed > limits::max_clip_speed)
            throw std::runtime_error("Invalid clip timing, frame count or speed: " + clip.id);
        // A lead-in that swallowed the last frame would leave nothing to cycle, so the clip
        // would freeze while claiming to loop. Refused rather than clamped: the artist asked
        // for something the format cannot express, and silently doing something else is worse.
        if (clip.loop_from < 0 || static_cast<size_t>(clip.loop_from) >= clip.frames.size())
            throw std::runtime_error("Clip loop start outside its frames: " + clip.id);
        for (const auto &frame : clip.frames) {
            if (!a.regions.empty()) {
                if (frame.atlas < 0 || static_cast<size_t>(frame.atlas) >= a.regions.size())
                    throw std::runtime_error("Clip frame names no packed frame: " + clip.id);
            } else {
                if (frame.atlas < 0 || frame.atlas > a.grid * a.grid - 1)
                    throw std::runtime_error("Clip frame outside atlas: " + clip.id);
                // A cell past its row's four continues below, and must still be on the sheet from
                // every row a facing draws from.
                for (const int row : {a.right_row, a.left_row})
                    if (atlas_cell(frame.atlas, row, a.grid).row > a.grid - 1)
                        throw std::runtime_error("Clip frame " + std::to_string(frame.atlas) +
                                                 " runs off the sheet below row " + std::to_string(row) +
                                                 ": " + clip.id);
            }
            if (frame.ticks < 0 || frame.ticks > limits::max_clip_ticks || !finite(frame.offset) ||
                std::abs(frame.offset.x) > limits::max_frame_offset ||
                std::abs(frame.offset.y) > limits::max_frame_offset)
                throw std::runtime_error("Clip frame hold or offset out of range: " + clip.id);
            // Mirroring is flip_x, so a negative scale is refused rather than silently meaning
            // the same thing by a second route - two ways to say one thing is how an editor
            // and a loader come to disagree about what a file contains. The floor is a real
            // bound rather than "anything above zero": a format whose smallest legal value
            // cannot be written down is one no widget can offer the whole of.
            if (!finite(frame.scale) || frame.scale.x < limits::min_frame_scale ||
                frame.scale.y < limits::min_frame_scale || frame.scale.x > limits::max_frame_scale ||
                frame.scale.y > limits::max_frame_scale)
                throw std::runtime_error("Clip frame scale out of range: " + clip.id);
            if (!std::isfinite(frame.angle) || std::abs(frame.angle) > limits::max_frame_angle)
                throw std::runtime_error("Clip frame angle out of range: " + clip.id);
            // The same canvas range the feet pivot and every socket obey, so a pivot cannot
            // name a point the art does not contain.
            if (frame.pivot && (!finite(*frame.pivot) || frame.pivot->x < 0 || frame.pivot->x > a.canvas ||
                                frame.pivot->y < 0 || frame.pivot->y > a.canvas))
                throw std::runtime_error("Clip frame pivot outside the canvas: " + clip.id);
            for (float channel : {frame.tint.r, frame.tint.g, frame.tint.b, frame.tint.a})
                if (!std::isfinite(channel) || channel < 0 || channel > 1)
                    throw std::runtime_error("Clip frame tint out of range: " + clip.id);
            check_attachments(frame.attachments, clip.id.c_str());
        }
        for (const auto &event : clip.events) {
            if (event.id.empty() || event.id.size() > limits::max_identifier || event.tick < 0)
                throw std::runtime_error("Invalid clip event in " + clip.id);
            if (event.tick > clip.pass_ticks())
                throw std::runtime_error("Clip event past the end of " + clip.id);
        }
    }
    // The fallback chain every pose lookup ends with has to terminate on something real, so an
    // actor without an idle clip is refused rather than drawn as frame 0 by accident.
    if (!a.clip("idle"))
        throw std::runtime_error("An actor needs an idle clip to fall back to");
}
Animation animation_from_json(const Json &j) {
    const auto &canvas = j.at("canvas");
    if (j.at("version") != 2 || !canvas.is_array() || canvas.size() != 2 || !canvas.at(0).is_number() ||
        canvas.at(0) != canvas.at(1) || j.at("facings") != Json::array({"right", "left"}) ||
        !j.at("texture").is_string())
        throw std::runtime_error("Unsupported animation layout");
    Animation a;
    // Square; 128 units unless the file packs its frames (validated below).
    a.canvas = canvas.at(0).get<float>();
    a.feet = {j.at("feet").at(0), j.at("feet").at(1)};
    a.right_row = j.at("facing_rows").at("right");
    a.left_row = j.at("facing_rows").at("left");
    a.mirror_left = j.value("mirror_left", false);
    a.grid = j.value("grid", limits::atlas_columns);
    if (j.contains("regions")) {
        // Packed frames: [page, x, y, w, h, left, top] each, compact because a baked layer has hundreds.
        const auto &regions = j.at("regions");
        if (!regions.is_array() || regions.size() > limits::max_regions)
            throw std::runtime_error("Packed frames are a list");
        for (const auto &r : regions) {
            if (!r.is_array() || r.size() != 7)
                throw std::runtime_error("A packed frame is [page, x, y, w, h, left, top]");
            a.regions.push_back({r.at(0), r.at(1), r.at(2), r.at(3), r.at(4), r.at(5), r.at(6)});
        }
        a.cell_pixels = j.at("cell_pixels");
        a.page_size = {j.at("page_size").at(0), j.at("page_size").at(1)};
    }
    if (j.contains("pages")) {
        const auto &pages = j.at("pages");
        if (!pages.is_array() || pages.size() + 1 > static_cast<size_t>(limits::max_pages))
            throw std::runtime_error("Pages are a list of at most " + std::to_string(limits::max_pages - 1) +
                                     " more images");
        a.page_count = static_cast<int>(pages.size()) + 1;
    }
    a.attachments = attachments_from_json(j.at("attachments"));
    for (const auto &[id, body] : j.at("animations").items()) {
        AnimationClip clip;
        clip.id = id;
        clip.ticks_per_frame = body.at("ticks_per_frame");
        clip.loop = body.contains("loop") ? loop_from_json(body.at("loop")) : LoopMode::Loop;
        clip.speed = body.value("speed", 1.0f);
        clip.loop_from = body.value("loop_from", 0);
        // A frame is a bare integer or an object. The integer form is what every existing file
        // contains, so none of them has to change to keep working.
        for (const auto &entry : body.at("frames")) {
            AnimationFrame frame;
            if (entry.is_number_integer())
                frame.atlas = entry.get<int>();
            else {
                frame.atlas = entry.at("atlas");
                frame.ticks = entry.value("ticks", 0);
                if (entry.contains("offset"))
                    frame.offset = {entry.at("offset").at(0), entry.at("offset").at(1)};
                if (entry.contains("attachments"))
                    frame.attachments = attachments_from_json(entry.at("attachments"));
                frame.flip_x = entry.value("flip_x", false);
                frame.flip_y = entry.value("flip_y", false);
                if (entry.contains("scale"))
                    frame.scale = {entry.at("scale").at(0), entry.at("scale").at(1)};
                frame.angle = entry.value("angle", 0.0f);
                if (entry.contains("pivot"))
                    frame.pivot = Vec2{entry.at("pivot").at(0), entry.at("pivot").at(1)};
                if (entry.contains("tint"))
                    frame.tint = {entry.at("tint").at(0), entry.at("tint").at(1), entry.at("tint").at(2),
                                  entry.at("tint").at(3)};
            }
            clip.frames.push_back(std::move(frame));
        }
        for (const auto &entry : body.value("events", Json::array()))
            clip.events.push_back({entry.at("tick"), entry.at("id")});
        std::sort(clip.events.begin(), clip.events.end(),
                  [](const AnimationEvent &x, const AnimationEvent &y) { return x.tick < y.tick; });
        a.clips.push_back(std::move(clip));
    }
    std::sort(a.clips.begin(), a.clips.end(),
              [](const AnimationClip &x, const AnimationClip &y) { return x.id < y.id; });
    validate_animation(a);
    return a;
}
// One object's content form, shared by scenes and prefabs (they were two copies, and a field added
// to one and not the other is how a prefab quietly drops what a room keeps). Optional fields are
// written only when they carry information, so files authored before each existed round-trip
// byte-identically.
Object object_from_json(const Json &v) {
    Object o;
    o.id = v.at("id");
    o.kind = v.at("kind");
    o.position = {v.at("position").at(0), v.at("position").at(1)};
    o.size = {v.at("size").at(0), v.at("size").at(1)};
    o.target = v.value("target", "");
    o.layer = v.value("layer", 0);  // absent means the base layer
    if (v.contains("capabilities")) // absent means every capability
        o.capabilities = capabilities_from_json(v.at("capabilities"));
    o.facing = v.value("facing", 1) >= 0 ? 1 : -1; // absent means facing right
    o.archetype = v.value("archetype", "");
    o.patrol = v.value("patrol", 0.0f);
    if (v.contains("travel"))
        o.travel = {v.at("travel").at(0), v.at("travel").at(1)};
    o.period = v.value("period", 0.0f);
    o.min_tier = v.value("min_tier", 0);
    o.art = v.value("art", "");
    return o;
}
Json object_json(const Object &o) {
    Json entry = {{"id", o.id},
                  {"kind", o.kind},
                  {"position", {o.position.x, o.position.y}},
                  {"size", {o.size.x, o.size.y}},
                  {"target", o.target}};
    if (o.layer != 0)
        entry["layer"] = o.layer;
    if (!o.capabilities.complete()) // only when something was taken away
        entry["capabilities"] = capabilities_to_json(o.capabilities);
    if (o.facing < 0)
        entry["facing"] = o.facing;
    if (!o.archetype.empty())
        entry["archetype"] = o.archetype;
    if (o.patrol > 0)
        entry["patrol"] = o.patrol;
    if (o.period > 0) {
        entry["travel"] = {o.travel.x, o.travel.y};
        entry["period"] = o.period;
    }
    if (o.min_tier > 0)
        entry["min_tier"] = o.min_tier;
    if (!o.art.empty())
        entry["art"] = o.art;
    return entry;
}
float wind_at(const Scene &scene, Vec2 point) {
    float wind = scene.wind;
    for (const auto &z : scene.wind_zones)
        if (point.x >= z.area.x && point.x <= z.area.x + z.area.w && point.y >= z.area.y &&
            point.y <= z.area.y + z.area.h)
            wind = std::max(wind, z.strength);
    return wind;
}
Scene scene_from_json(const Json &j) {
    if (j.at("version") != 1)
        throw std::runtime_error("Unsupported scene version");
    Scene s;
    s.id = j.at("id");
    s.music = j.value("music", "");
    s.endless = j.value("endless", "");
    // Atmosphere, each written only when set.
    auto colour_from = [](const Json &c) {
        if (!c.is_array() || (c.size() != 3 && c.size() != 4))
            throw std::runtime_error("A colour is [r, g, b] or [r, g, b, a]");
        return Color{c.at(0).get<float>(), c.at(1).get<float>(), c.at(2).get<float>(),
                     c.size() == 4 ? c.at(3).get<float>() : 1.0f};
    };
    auto pair_from = [](const Json &v) { return Vec2{v.at(0).get<float>(), v.at(1).get<float>()}; };
    for (const auto &v : j.value("backdrops", Json::array())) {
        Backdrop b;
        b.image = v.at("image").get<std::string>();
        if (v.contains("position"))
            b.position = pair_from(v.at("position"));
        if (v.contains("parallax"))
            b.parallax = pair_from(v.at("parallax"));
        b.scale = v.value("scale", 1.0f);
        b.repeat_x = v.value("repeat_x", false);
        b.front = v.value("front", false);
        if (v.contains("tint"))
            b.tint = colour_from(v.at("tint"));
        b.motion = v.value("motion", std::string{});
        if (v.contains("pivot"))
            b.pivot = pair_from(v.at("pivot"));
        b.angle = v.value("angle", 0.0f);
        s.backdrops.push_back(std::move(b));
    }
    if (j.contains("ambient"))
        s.ambient = colour_from(j.at("ambient"));
    for (const auto &v : j.value("tiles", Json::array())) {
        TileMap m;
        m.tileset = v.at("tileset").get<std::string>();
        m.tile = v.value("tile", m.tile);
        m.cell = v.value("cell", m.cell);
        m.origin = pair_from(v.at("origin"));
        m.columns = v.at("columns").get<int>();
        m.rows = v.at("rows").get<int>();
        if (m.columns < 1 || m.rows < 1 || m.columns > limits::max_tile_grid ||
            m.rows > limits::max_tile_grid)
            throw std::runtime_error("A tile map's grid is 1.." + std::to_string(limits::max_tile_grid) +
                                     " a side");
        m.cells = tile_cells_from_json(v.at("cells"), m.columns, m.rows);
        m.solid = v.value("solid", std::vector<uint16_t>{});
        m.platforms = v.value("platforms", std::vector<uint16_t>{});
        s.tiles.push_back(std::move(m));
    }
    for (const auto &v : j.value("lights", Json::array())) {
        PointLight l;
        l.position = pair_from(v.at("position"));
        l.radius = v.value("radius", l.radius);
        if (v.contains("color"))
            l.color = colour_from(v.at("color"));
        l.intensity = v.value("intensity", l.intensity);
        s.lights.push_back(l);
    }
    for (const auto &v : j.at("objects"))
        s.objects.push_back(object_from_json(v));
    for (const auto &v : j.value("camera_zones", Json::array())) {
        CameraZone z;
        z.id = v.at("id");
        z.area = {v.at("area").at(0), v.at("area").at(1), v.at("area").at(2), v.at("area").at(3)};
        z.priority = v.value("priority", 0);
        z.blend_in = v.value("blend_in", 0.5f);
        z.blend_out = v.value("blend_out", 0.5f);
        z.snap = v.value("snap", false);
        // An absent key is "this zone does not override that setting", which is not the same
        // as an authored value equal to the default - the zone must keep inheriting if the
        // room default later changes.
        if (v.contains("dead_zone"))
            z.dead_zone = Vec2{v.at("dead_zone").at(0), v.at("dead_zone").at(1)};
        if (v.contains("look_ahead"))
            z.look_ahead = v.at("look_ahead").get<float>();
        if (v.contains("follow_rate"))
            z.follow_rate = v.at("follow_rate").get<float>();
        if (v.contains("zoom"))
            z.zoom = v.at("zoom").get<float>();
        if (v.contains("vertical_bias"))
            z.vertical_bias = v.at("vertical_bias").get<float>();
        if (v.contains("lock"))
            z.lock = Vec2{v.at("lock").at(0), v.at("lock").at(1)};
        if (v.contains("bounds"))
            z.bounds =
                Rect{v.at("bounds").at(0), v.at("bounds").at(1), v.at("bounds").at(2), v.at("bounds").at(3)};
        s.camera_zones.push_back(std::move(z));
    }
    s.wind = j.value("wind", 0.0f);
    s.ground_art = j.value("ground_art", false);
    for (const auto &v : j.value("wind_zones", Json::array())) {
        WindZone z;
        z.id = v.at("id");
        z.area = {v.at("area").at(0), v.at("area").at(1), v.at("area").at(2), v.at("area").at(3)};
        z.strength = v.value("strength", 1.0f);
        s.wind_zones.push_back(std::move(z));
    }
    for (const auto &v : j.value("prefabs", Json::array())) {
        PrefabInstance instance;
        instance.id = v.at("id");
        instance.prefab = v.at("prefab");
        instance.position = {v.at("position").at(0), v.at("position").at(1)};
        for (const auto &entry : v.value("overrides", Json::array())) {
            PrefabOverride o;
            o.child = entry.at("child");
            if (entry.contains("position"))
                o.position = Vec2{entry.at("position").at(0), entry.at("position").at(1)};
            if (entry.contains("size"))
                o.size = Vec2{entry.at("size").at(0), entry.at("size").at(1)};
            if (entry.contains("target"))
                o.target = entry.at("target").get<std::string>();
            if (entry.contains("layer"))
                o.layer = entry.at("layer").get<int>();
            instance.overrides.push_back(std::move(o));
        }
        s.prefabs.push_back(std::move(instance));
    }
    for (const auto &v : j.value("cutscene_triggers", Json::array())) {
        CutsceneTrigger t;
        t.id = v.at("id");
        t.cutscene = v.at("cutscene");
        t.when = v.value("when", "entry");
        if (v.contains("area"))
            t.area = {v.at("area").at(0), v.at("area").at(1), v.at("area").at(2), v.at("area").at(3)};
        t.quest = v.value("quest", 0);
        t.once = v.value("once", true);
        for (const auto &c : v.value("if", Json::array()))
            t.conditions.push_back(condition_from_json(c, "Cutscene trigger " + t.id));
        s.cutscene_triggers.push_back(std::move(t));
    }
    validate_scene(s);
    return s;
}
Json scene_json(const Scene &s) {
    validate_scene(s);
    Json j = {{"version", 1}, {"id", s.id}, {"objects", Json::array()}};
    for (const auto &o : s.objects)
        j["objects"].push_back(object_json(o));
    if (!s.music.empty())
        j["music"] = s.music;
    if (!s.endless.empty())
        j["endless"] = s.endless;
    auto colour_json = [](Color c) { return Json::array({c.r, c.g, c.b, c.a}); };
    if (!s.backdrops.empty()) {
        j["backdrops"] = Json::array();
        const Backdrop defaults;
        for (const auto &b : s.backdrops) {
            Json entry{{"image", b.image},
                       {"position", {b.position.x, b.position.y}},
                       {"parallax", {b.parallax.x, b.parallax.y}}};
            if (b.scale != defaults.scale)
                entry["scale"] = b.scale;
            if (b.repeat_x)
                entry["repeat_x"] = true;
            if (b.front)
                entry["front"] = true;
            if (b.tint != defaults.tint)
                entry["tint"] = colour_json(b.tint);
            if (!b.motion.empty())
                entry["motion"] = b.motion;
            if (b.pivot != defaults.pivot)
                entry["pivot"] = {b.pivot.x, b.pivot.y};
            if (b.angle != defaults.angle)
                entry["angle"] = b.angle;
            j["backdrops"].push_back(std::move(entry));
        }
    }
    if (s.ambient != Color{})
        j["ambient"] = colour_json(s.ambient);
    if (!s.tiles.empty()) {
        j["tiles"] = Json::array();
        for (const auto &m : s.tiles) {
            Json entry{{"tileset", m.tileset},
                       {"tile", m.tile},
                       {"cell", m.cell},
                       {"origin", {m.origin.x, m.origin.y}},
                       {"columns", m.columns},
                       {"rows", m.rows},
                       {"cells", tile_cells_json(m)}};
            if (!m.solid.empty())
                entry["solid"] = m.solid;
            if (!m.platforms.empty())
                entry["platforms"] = m.platforms;
            j["tiles"].push_back(std::move(entry));
        }
    }
    if (!s.lights.empty()) {
        j["lights"] = Json::array();
        for (const auto &l : s.lights)
            j["lights"].push_back({{"position", {l.position.x, l.position.y}},
                                   {"radius", l.radius},
                                   {"color", colour_json(l.color)},
                                   {"intensity", l.intensity}});
    }
    // Same rule as `layer`: the key appears only when it carries information, so rooms
    // authored before the camera director round-trip byte-identically and no schema bump is
    // needed. Within a zone each override is likewise written only when it is set.
    if (!s.camera_zones.empty()) {
        j["camera_zones"] = Json::array();
        for (const auto &z : s.camera_zones) {
            Json entry = {{"id", z.id}, {"area", {z.area.x, z.area.y, z.area.w, z.area.h}}};
            if (z.priority != 0)
                entry["priority"] = z.priority;
            if (z.blend_in != 0.5f)
                entry["blend_in"] = z.blend_in;
            if (z.blend_out != 0.5f)
                entry["blend_out"] = z.blend_out;
            if (z.snap)
                entry["snap"] = true;
            if (z.dead_zone)
                entry["dead_zone"] = {z.dead_zone->x, z.dead_zone->y};
            if (z.look_ahead)
                entry["look_ahead"] = *z.look_ahead;
            if (z.follow_rate)
                entry["follow_rate"] = *z.follow_rate;
            if (z.zoom)
                entry["zoom"] = *z.zoom;
            if (z.vertical_bias)
                entry["vertical_bias"] = *z.vertical_bias;
            if (z.lock)
                entry["lock"] = {z.lock->x, z.lock->y};
            if (z.bounds)
                entry["bounds"] = {z.bounds->x, z.bounds->y, z.bounds->w, z.bounds->h};
            j["camera_zones"].push_back(std::move(entry));
        }
    }
    // Wind, by the same rule: a still room writes nothing.
    if (s.wind != 0)
        j["wind"] = s.wind;
    if (s.ground_art)
        j["ground_art"] = true;
    if (!s.wind_zones.empty()) {
        j["wind_zones"] = Json::array();
        for (const auto &z : s.wind_zones) {
            Json entry = {{"id", z.id}, {"area", {z.area.x, z.area.y, z.area.w, z.area.h}}};
            if (z.strength != 1)
                entry["strength"] = z.strength;
            j["wind_zones"].push_back(std::move(entry));
        }
    }
    if (!s.prefabs.empty()) {
        // Same omit-at-default rule as everything else: a room that places no prefab writes no
        // key, so every room authored before prefabs round-trips unchanged.
        j["prefabs"] = Json::array();
        for (const auto &instance : s.prefabs) {
            Json entry = {{"id", instance.id},
                          {"prefab", instance.prefab},
                          {"position", {instance.position.x, instance.position.y}}};
            if (!instance.overrides.empty()) {
                entry["overrides"] = Json::array();
                for (const auto &o : instance.overrides) {
                    Json written = {{"child", o.child}};
                    if (o.position)
                        written["position"] = {o.position->x, o.position->y};
                    if (o.size)
                        written["size"] = {o.size->x, o.size->y};
                    if (o.target)
                        written["target"] = *o.target;
                    if (o.layer)
                        written["layer"] = *o.layer;
                    entry["overrides"].push_back(std::move(written));
                }
            }
            j["prefabs"].push_back(std::move(entry));
        }
    }
    if (!s.cutscene_triggers.empty()) {
        j["cutscene_triggers"] = Json::array();
        for (const auto &t : s.cutscene_triggers) {
            Json entry = {{"id", t.id}, {"cutscene", t.cutscene}};
            // Same rule as everywhere else: a key appears only when it carries information.
            if (t.when != "entry")
                entry["when"] = t.when;
            if (t.when == "volume")
                entry["area"] = {t.area.x, t.area.y, t.area.w, t.area.h};
            if (t.quest != 0)
                entry["quest"] = t.quest;
            if (!t.once)
                entry["once"] = false;
            if (!t.conditions.empty()) {
                entry["if"] = Json::array();
                for (const auto &c : t.conditions)
                    entry["if"].push_back(condition_json(c));
            }
            j["cutscene_triggers"].push_back(std::move(entry));
        }
    }
    return j;
}
void SceneHistory::edit(const std::function<void(Scene &)> &change) {
    if (gesture_)
        throw std::runtime_error("Commit or cancel the active edit first");
    auto candidate = current;
    change(candidate);
    validate_scene(candidate);
    if (candidate == current)
        return;
    if (undo_.size() == 128)
        undo_.erase(undo_.begin());
    undo_.push_back(current);
    current = std::move(candidate);
    redo_.clear();
}
void SceneHistory::begin_gesture() {
    if (gesture_)
        throw std::runtime_error("Edit gesture already active");
    gesture_ = current;
}
void SceneHistory::preview(const std::function<void(Scene &)> &change) {
    if (!gesture_)
        throw std::runtime_error("No active edit gesture");
    auto candidate = current;
    change(candidate);
    validate_scene(candidate);
    current = std::move(candidate);
}
bool SceneHistory::commit_gesture() {
    if (!gesture_)
        return false;
    auto final = current;
    current = *gesture_;
    gesture_.reset();
    edit([&](Scene &s) { s = final; });
    return true;
}
bool SceneHistory::cancel_gesture() {
    if (!gesture_)
        return false;
    current = *gesture_;
    gesture_.reset();
    return true;
}
bool SceneHistory::undo() {
    if (cancel_gesture())
        return true;
    if (undo_.empty())
        return false;
    redo_.push_back(current);
    current = undo_.back();
    undo_.pop_back();
    return true;
}
bool SceneHistory::redo() {
    if (gesture_)
        return false;
    if (redo_.empty())
        return false;
    undo_.push_back(current);
    current = redo_.back();
    redo_.pop_back();
    return true;
}
Rect CanvasView::fitted() const {
    if (!finite({area.x, area.y}) || !finite({area.w, area.h}) || !finite(canvas) || area.w <= 0 ||
        area.h <= 0 || canvas.x <= 0 || canvas.y <= 0)
        throw std::runtime_error("Invalid canvas viewport");
    float scale = std::min(area.w / canvas.x, area.h / canvas.y);
    return {area.x + (area.w - canvas.x * scale) / 2, area.y + (area.h - canvas.y * scale) / 2,
            canvas.x * scale, canvas.y * scale};
}
Vec2 CanvasView::to_canvas(Vec2 screen) const {
    auto view = fitted();
    return {(screen.x - view.x) * canvas.x / view.w, (screen.y - view.y) * canvas.y / view.h};
}
Vec2 CanvasView::to_screen(Vec2 point) const {
    auto view = fitted();
    return {view.x + point.x * view.w / canvas.x, view.y + point.y * view.h / canvas.y};
}
Vec2 scaled_window_size(Vec2 canvas, float display_scale) {
    // A scale the platform could not report, or one outside anything a desktop offers, is
    // treated as 1 rather than propagated: a zero or non-finite scale here produces a window
    // with no pixels in it.
    const float scale =
        std::isfinite(display_scale) && display_scale > 0 ? std::clamp(display_scale, 1.0f, 4.0f) : 1.0f;
    if (!finite(canvas) || canvas.x < 1 || canvas.y < 1)
        return {1280, 720};
    // Never scaled *down*: a display reporting less than 1 is either a misconfiguration or a
    // very small screen, and shrinking the window would make the game unreadable on both.
    return {std::round(canvas.x * scale), std::round(canvas.y * scale)};
}
ViewportPair split_viewport(Rect region, ViewportSplit split) {
    if (!finite({region.x, region.y}) || !finite({region.w, region.h}) || region.w < 1 || region.h < 1)
        return {{}, {}};
    // Below this a divided view is two slivers neither of which shows anything useful, so the
    // split silently falls back rather than presenting a broken workspace.
    constexpr float minimum = 240;
    const float gap = 4;
    if (split == ViewportSplit::SideBySide && region.w >= minimum * 2 + gap) {
        const float half = (region.w - gap) / 2;
        return {{region.x, region.y, half, region.h}, {region.x + half + gap, region.y, half, region.h}};
    }
    if (split == ViewportSplit::Stacked && region.h >= minimum + gap) {
        const float half = (region.h - gap) / 2;
        return {{region.x, region.y, region.w, half}, {region.x, region.y + half + gap, region.w, half}};
    }
    return {region, {}};
}
EditorLayout editor_layout(Vec2 size) {
    if (!finite(size) || size.x < 1 || size.y < 1)
        throw std::runtime_error("Invalid editor size");
    if (size.x >= 1400 && size.x / size.y > 1.4f)
        return {{260, 0, size.x - 550, size.y}, {0, 0, 260, size.y}, {size.x - 290, 0, 290, size.y}};
    float split = std::min(size.y * .57f, size.x * 720 / 1280);
    split = std::max(1.0f, split);
    return {{0, 0, size.x, split},
            {0, split, size.x * .5f, std::max(1.0f, size.y - split)},
            {size.x * .5f, split, size.x * .5f, std::max(1.0f, size.y - split)}};
}
Rect object_bounds(const Object &object) {
    return {object.position.x - object.size.x / 2, object.position.y - object.size.y / 2, object.size.x,
            object.size.y};
}
Rect scene_bounds(const Scene &scene) {
    if (scene.objects.empty())
        return {0, 0, 1280, 720};
    auto bounds = object_bounds(scene.objects.front());
    float right = bounds.x + bounds.w, bottom = bounds.y + bounds.h;
    for (const auto &object : scene.objects) {
        const auto box = object_bounds(object);
        bounds.x = std::min(bounds.x, box.x);
        bounds.y = std::min(bounds.y, box.y);
        right = std::max(right, box.x + box.w);
        bottom = std::max(bottom, box.y + box.h);
    }
    bounds.w = right - bounds.x;
    bounds.h = bottom - bounds.y;
    return bounds;
}
std::optional<size_t> pick_object(const Scene &scene, Vec2 point,
                                  const std::function<bool(const Object &)> &pickable) {
    std::optional<size_t> result;
    float depth = -INFINITY;
    for (size_t i = 0; i < scene.objects.size(); ++i) {
        const auto &o = scene.objects[i];
        if (pickable && !pickable(o))
            continue;
        if (object_bounds(o).contains(point) && o.position.y + o.size.y / 2 >= depth) {
            result = i;
            depth = o.position.y + o.size.y / 2;
        }
    }
    return result;
}
std::optional<size_t> pick_object(const Scene &scene, Vec2 point) { return pick_object(scene, point, {}); }
bool kind_is_solid(std::string_view kind) { return kind == "wall" || kind == "platform"; }
Vec2 travel_offset(const Object &object, uint64_t tick) {
    if (object.period <= 0)
        return {};
    // A whole number of ticks per round trip, so the wave is exact and periodic in integers.
    const auto ticks = std::max<uint64_t>(2, static_cast<uint64_t>(std::lround(object.period / tick_dt)));
    const auto at = tick % ticks;
    const auto out = std::min(at, ticks - at); // 0 at home, ticks/2 at the far end
    return object.travel * (2.0f * static_cast<float>(out) / static_cast<float>(ticks));
}
namespace {
// The mover's sweep, over whichever objects `visit(range, fn)` offers for an x-range: every
// object in the room, or the solid index's candidates for that range. Either way in ascending scene
// order and including every solid that can touch the range, so the two give the same answer to the
// last bit. `range` is a getter: penetration can move the origin while visiting, so newly reached
// later objects must join that same pass, without revisiting an earlier scene index.
template <class Visit>
MoveResult sweep_with(Vec2 origin, Vec2 displacement, float radius, float half_height,
                      std::span<const Object> objects, bool through_platforms, const Visit &visit) {
    MoveResult result;
    // Resolve authored overlap before casting: circle versus axis-aligned wall extents.
    for (int pass = 0; pass < 8; ++pass) {
        bool moved = false;
        visit([&] { return std::pair{origin.x - radius - 1, origin.x + radius + 1}; },
              [&](size_t index) {
                  const auto &o = objects[index];
                  // Nothing farther than the circle's reach from the object's box can overlap it or rest on
                  // it: passed over before the kind is even compared, exactly as the cast below is.
                  if (std::abs(origin.x - o.position.x) > o.size.x / 2 + radius + 1 ||
                      std::abs(origin.y - o.position.y) > o.size.y / 2 + half_height + radius + 1)
                      return;
                  // A one-way platform only ever pushes up, and only something resting on its top edge:
                  // anything deeper is passing through it from below or dropping through it on purpose.
                  if (o.kind == "platform" && !through_platforms) {
                      const float top = o.position.y - o.size.y / 2 - half_height;
                      const float left = o.position.x - o.size.x / 2, right = o.position.x + o.size.x / 2;
                      // Resting means sunk at most a couple of units past the top edge; anything deeper is
                      // passing through it, and snapping that onto the top would pop a jump onto a ledge
                      // its feet never cleared.
                      if (origin.x >= left - radius * 0.5f && origin.x <= right + radius * 0.5f &&
                          origin.y + radius > top && origin.y + radius < top + 2) {
                          origin.y = top - radius - 0.01f;
                          moved = true;
                      }
                      return;
                  }
                  if (o.kind != "wall")
                      return;
                  Vec2 lo = o.position - o.size * 0.5f - Vec2{0, half_height},
                       hi = o.position + o.size * 0.5f + Vec2{0, half_height};
                  Vec2 closest{std::clamp(origin.x, lo.x, hi.x), std::clamp(origin.y, lo.y, hi.y)};
                  Vec2 separation = origin - closest;
                  float distance = length(separation);
                  if (distance >= radius)
                      return;
                  if (distance > 0.00001f) {
                      origin += separation * ((radius - distance + 0.01f) / distance);
                  } else {
                      std::array<float, 4> gaps{origin.x - lo.x, hi.x - origin.x, origin.y - lo.y,
                                                hi.y - origin.y};
                      auto side =
                          static_cast<size_t>(std::min_element(gaps.begin(), gaps.end()) - gaps.begin());
                      if (side == 0)
                          origin.x = lo.x - radius - 0.01f;
                      else if (side == 1)
                          origin.x = hi.x + radius + 0.01f;
                      else if (side == 2)
                          origin.y = lo.y - radius - 0.01f;
                      else
                          origin.y = hi.y + radius + 0.01f;
                  }
                  moved = true;
              });
        if (!moved)
            break;
    }
    // Convex shape queries only: no physics world, impulses or uncontrolled motion.
    for (int iteration = 0; iteration < 4 && length(displacement) > 0.001f; ++iteration) {
        float fraction = 1;
        Vec2 normal{};
        int touched = -1;
        // The box the circle sweeps through, a unit wider all round. A solid wholly outside it cannot
        // be touched by this cast, so it is passed over before the shape query: in a room of 512
        // objects the queries were nearly all against walls nowhere near the mover, and they were the
        // whole cost of a tick. Exact - it only skips casts that cannot hit.
        const Vec2 end = origin + displacement;
        const Vec2 swept_lo{std::min(origin.x, end.x) - radius - 1, std::min(origin.y, end.y) - radius - 1};
        const Vec2 swept_hi{std::max(origin.x, end.x) + radius + 1, std::max(origin.y, end.y) + radius + 1};
        visit([&] { return std::pair{swept_lo.x, swept_hi.x}; },
              [&](size_t index) {
                  const auto &o = objects[index];
                  if (o.position.x + o.size.x / 2 < swept_lo.x || o.position.x - o.size.x / 2 > swept_hi.x ||
                      o.position.y + o.size.y / 2 + half_height < swept_lo.y ||
                      o.position.y - o.size.y / 2 - half_height > swept_hi.y)
                      return;
                  if (!kind_is_solid(o.kind) || (o.kind == "platform" && through_platforms))
                      return;
                  // One-way: only a downward move that starts at or above the top edge can land on it.
                  if (o.kind == "platform" &&
                      (displacement.y <= 0 ||
                       origin.y + radius > o.position.y - o.size.y / 2 - half_height + 0.5f))
                      return;
                  auto polygon = b2MakeOffsetBox(o.size.x / 2, o.size.y / 2 + half_height,
                                                 {o.position.x, o.position.y}, b2Rot_identity);
                  b2Vec2 point{origin.x, origin.y};
                  b2ShapeCastInput cast{};
                  cast.proxy = b2MakeProxy(&point, 1, radius);
                  cast.translation = {displacement.x, displacement.y};
                  cast.maxFraction = 1;
                  cast.canEncroach = true;
                  auto hit = b2ShapeCastPolygon(&cast, &polygon);
                  if (hit.hit && hit.fraction < fraction && (o.kind != "platform" || hit.normal.y < -.5f)) {
                      fraction = hit.fraction;
                      normal = {hit.normal.x, hit.normal.y};
                      touched = static_cast<int>(index);
                  }
              });
        origin += displacement * std::max(0.0f, fraction - 0.001f);
        if (fraction >= 1)
            break;
        if (normal.y < -.5f) {
            result.floor = true;
            result.floor_normal = normal;
            result.floor_object = touched;
        }
        result.ceiling |= normal.y > .5f;
        if (std::abs(normal.x) > .5f) {
            result.wall = true;
            result.wall_normal = normal;
            result.wall_object = touched;
        }
        displacement = displacement * (1 - fraction);
        float into = dot(displacement, normal);
        if (into < 0)
            displacement = displacement - normal * into;
    }
    result.position = origin;
    return result;
}
} // namespace
MoveResult sweep_move(Vec2 origin, Vec2 displacement, float radius, float half_height,
                      std::span<const Object> objects, bool through_platforms) {
    return sweep_with(origin, displacement, radius, half_height, objects, through_platforms,
                      [&](const auto &, const auto &fn) {
                          for (size_t i = 0; i < objects.size(); ++i)
                              fn(i);
                      });
}
SolidIndex index_solids(std::span<const Object> objects, float band) {
    SolidIndex index;
    index.objects = objects.size();
    index.band = band > 0 && std::isfinite(band) ? band : 128;
    if (objects.size() > 0xffff)
        return index; // more objects than an entry can name: the full scan serves
    // The x-extent each solid can occupy: a mover's over its whole travel, so the index holds for
    // every tick without being rebuilt as it moves.
    auto extent = [](const Object &o) {
        const float from = std::min(o.position.x, o.position.x + o.travel.x) - o.size.x / 2 - 1;
        const float to = std::max(o.position.x, o.position.x + o.travel.x) + o.size.x / 2 + 1;
        return std::pair{from, to};
    };
    bool any = false;
    float lo = 0, hi = 0;
    for (const auto &o : objects)
        if (kind_is_solid(o.kind)) {
            if (!finite(o.position) || !finite(o.size) || !finite(o.travel) || o.size.x <= 0 || o.size.y <= 0)
                return index;
            const auto [from, to] = extent(o);
            if (!std::isfinite(from) || !std::isfinite(to) || from > to)
                return index;
            lo = any ? std::min(lo, from) : from;
            hi = any ? std::max(hi, to) : to;
            any = true;
        }
    if (!any)
        return index;
    // Tiny positive bands and extreme coordinates are legal API inputs, but must not cause an
    // out-of-range integer conversion or an unbounded allocation. An empty index selects the
    // identical full scan; normal room/default-band arithmetic stays unchanged.
    constexpr size_t most_bands = 4096, most_entries = 4 * 1024 * 1024;
    const float band_count = std::floor((hi - lo) / index.band) + 1;
    if (!std::isfinite(band_count) || band_count < 1 || band_count > most_bands)
        return index;
    const auto bands = static_cast<size_t>(band_count);
    size_t entries = 0;
    for (const auto &o : objects)
        if (kind_is_solid(o.kind)) {
            const auto [from, to] = extent(o);
            const auto first = static_cast<size_t>(std::floor((from - lo) / index.band));
            const auto last = std::min(bands - 1, static_cast<size_t>(std::floor((to - lo) / index.band)));
            if (first > last || last - first + 1 > most_entries - entries)
                return index;
            entries += last - first + 1;
        }
    index.left = lo;
    std::vector<std::vector<uint16_t>> lists(bands);
    for (size_t i = 0; i < objects.size(); ++i)
        if (kind_is_solid(objects[i].kind)) {
            const auto [from, to] = extent(objects[i]);
            const auto first = static_cast<size_t>(std::floor((from - lo) / index.band));
            const auto last = std::min(bands - 1, static_cast<size_t>(std::floor((to - lo) / index.band)));
            for (size_t b = first; b <= last; ++b)
                lists[b].push_back(static_cast<uint16_t>(i));
        }
    index.starts.reserve(bands + 1);
    index.ids.reserve(entries);
    for (const auto &list : lists) {
        index.starts.push_back(static_cast<uint32_t>(index.ids.size()));
        index.ids.insert(index.ids.end(), list.begin(), list.end());
    }
    index.starts.push_back(static_cast<uint32_t>(index.ids.size()));
    return index;
}
MoveResult sweep_move(Vec2 origin, Vec2 displacement, float radius, float half_height,
                      std::span<const Object> objects, const SolidIndex &index, bool through_platforms) {
    // Count changes and unusable index parameters select the full scan. For a same-size room,
    // the caller must ensure the indexed envelopes still contain every current solid's x bounds;
    // object count alone cannot detect changed geometry or scene-index ordering.
    if (index.starts.size() < 2 || index.objects != objects.size() || !std::isfinite(index.left) ||
        !std::isfinite(index.band) || index.band <= 0)
        return sweep_move(origin, displacement, radius, half_height, objects, through_platforms);
    const auto bands = index.starts.size() - 1;
    return sweep_with(
        origin, displacement, radius, half_height, objects, through_platforms,
        [&](const auto &range, const auto &fn) {
            size_t next_scene = 0;
            auto bounds = range();
            for (;;) {
                const auto [lo_x, hi_x] = bounds;
                const float from = (lo_x - index.left) / index.band, to = (hi_x - index.left) / index.band;
                auto remaining = [&] {
                    for (size_t i = next_scene; i < objects.size(); ++i)
                        fn(i);
                };
                // Unsupported coordinates never go through a float-to-index conversion. The
                // remaining full scan also preserves order after an earlier penetration push.
                if (!std::isfinite(lo_x) || !std::isfinite(hi_x) || lo_x > hi_x || !std::isfinite(from) ||
                    !std::isfinite(to)) {
                    remaining();
                    return;
                }
                if (to < 0 || from >= static_cast<float>(bands))
                    return; // wholly outside every solid's reach
                const auto first = static_cast<size_t>(std::max(0.0f, std::floor(from)));
                const auto last = static_cast<size_t>(
                    std::min(static_cast<float>(bands - 1), std::max(0.0f, std::floor(to))));
                // A long move is rare; walking the remaining objects keeps scene order without
                // merging an arbitrary number of band lists.
                constexpr size_t most_merged = 4;
                if (last - first + 1 > most_merged) {
                    remaining();
                    return;
                }
                std::array<uint32_t, most_merged> at{}, end{};
                const size_t count = last - first + 1;
                for (size_t k = 0; k < count; ++k) {
                    at[k] = index.starts[first + k];
                    end[k] = index.starts[first + k + 1];
                }
                bool changed = false;
                for (;;) {
                    size_t best = count;
                    for (size_t k = 0; k < count; ++k)
                        if (at[k] < end[k] && (best == count || index.ids[at[k]] < index.ids[at[best]]))
                            best = k;
                    if (best == count)
                        break;
                    const auto next = index.ids[at[best]++];
                    // This both deduplicates bands and skips objects already considered before
                    // a push changed the range. A full scan does not revisit those this pass.
                    if (next < next_scene)
                        continue;
                    next_scene = static_cast<size_t>(next) + 1;
                    fn(next);
                    const auto updated = range();
                    if (updated != bounds) {
                        bounds = updated;
                        changed = true;
                        break;
                    }
                }
                if (!changed || next_scene == objects.size())
                    return;
                // A penetration push may have reached a later object outside the old bands.
                // Refresh now, in the same pass, and resume above the last visited scene index.
            }
        });
}
Vec2 slide_move(Vec2 origin, Vec2 displacement, float radius, std::span<const Object> objects) {
    return sweep_move(origin, displacement, radius, 0, objects).position;
}
Vec2 slide_move(Vec2 origin, Vec2 displacement, float radius, std::span<const Object> objects,
                const SolidIndex &index) {
    return sweep_move(origin, displacement, radius, 0, objects, index).position;
}

// ---- Prefabs ----
namespace {
PrefabOverride override_from_json(const Json &v) {
    PrefabOverride o;
    o.child = v.at("child");
    if (v.contains("position"))
        o.position = Vec2{v.at("position").at(0), v.at("position").at(1)};
    if (v.contains("size"))
        o.size = Vec2{v.at("size").at(0), v.at("size").at(1)};
    if (v.contains("target"))
        o.target = v.at("target").get<std::string>();
    if (v.contains("layer"))
        o.layer = v.at("layer").get<int>();
    return o;
}
Json override_json(const PrefabOverride &o) {
    Json j = {{"child", o.child}};
    // Omitted at default like every other optional here: an override that wrote every field
    // would freeze the placement at the prefab's values on the day it was placed.
    if (o.position)
        j["position"] = {o.position->x, o.position->y};
    if (o.size)
        j["size"] = {o.size->x, o.size->y};
    if (o.target)
        j["target"] = *o.target;
    if (o.layer)
        j["layer"] = *o.layer;
    return j;
}
PrefabInstance instance_from_json(const Json &v) {
    PrefabInstance instance;
    instance.id = v.at("id");
    instance.prefab = v.at("prefab");
    instance.position = {v.at("position").at(0), v.at("position").at(1)};
    for (const auto &entry : v.value("overrides", Json::array()))
        instance.overrides.push_back(override_from_json(entry));
    return instance;
}
Json instance_json(const PrefabInstance &instance) {
    Json j = {{"id", instance.id},
              {"prefab", instance.prefab},
              {"position", {instance.position.x, instance.position.y}}};
    if (!instance.overrides.empty()) {
        j["overrides"] = Json::array();
        for (const auto &entry : instance.overrides)
            j["overrides"].push_back(override_json(entry));
    }
    return j;
}
void validate_instances(const std::vector<PrefabInstance> &instances, const char *where) {
    std::set<std::string> ids;
    for (const auto &instance : instances) {
        if (instance.id.empty() || instance.id.size() > limits::max_identifier ||
            !ids.insert(instance.id).second)
            throw std::runtime_error(std::string("Invalid prefab instance id in ") + where);
        // The prefab name becomes a filename, so it goes through the same portability gate as
        // every other authored name rather than straight to the loader.
        if (instance.prefab.empty() || instance.prefab.size() > limits::max_identifier ||
            instance.prefab.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos ||
            !portable_filename(instance.prefab + ".prefab.json"))
            throw std::runtime_error("Prefab instance needs a simple portable prefab ID: " + instance.id);
        if (!finite(instance.position) || std::abs(instance.position.x) > limits::max_position ||
            std::abs(instance.position.y) > limits::max_position)
            throw std::runtime_error("Prefab instance position out of range: " + instance.id);
        std::set<std::string> children;
        for (const auto &entry : instance.overrides) {
            if (entry.child.empty() || entry.child.size() > limits::max_identifier ||
                !children.insert(entry.child).second)
                throw std::runtime_error("Invalid prefab override in " + instance.id);
            if (entry.size && (!finite(*entry.size) || entry.size->x < limits::min_size ||
                               entry.size->y < limits::min_size || entry.size->x > limits::max_size ||
                               entry.size->y > limits::max_size))
                throw std::runtime_error("Prefab override size out of range in " + instance.id);
            if (entry.position &&
                (!finite(*entry.position) || std::abs(entry.position->x) > limits::max_position ||
                 std::abs(entry.position->y) > limits::max_position))
                throw std::runtime_error("Prefab override position out of range in " + instance.id);
            if (entry.layer && (*entry.layer < 0 || *entry.layer > limits::max_layer))
                throw std::runtime_error("Prefab override layer out of range in " + instance.id);
        }
    }
}
} // namespace

const Prefab *PrefabLibrary::find(const std::string &id) const {
    for (const auto &prefab : prefabs)
        if (prefab.id == id)
            return &prefab;
    return nullptr;
}
void validate_prefab(const Prefab &prefab) {
    if (prefab.id.empty() || prefab.id.size() > limits::max_identifier)
        throw std::runtime_error("Invalid prefab id");
    if (prefab.objects.empty() && prefab.instances.empty())
        throw std::runtime_error("Prefab contains nothing: " + prefab.id);
    if (prefab.objects.size() + prefab.instances.size() > limits::max_objects)
        throw std::runtime_error("Prefab capacity exceeded: " + prefab.id);
    validate_instances(prefab.instances, prefab.id.c_str());
    // Each object is checked the way validate_scene checks one, minus the scene-level rules a
    // prefab has no business satisfying: a prefab is a fragment and needs no spawn.
    std::set<std::string> ids;
    const auto kinds = object_kinds();
    for (const auto &o : prefab.objects) {
        if (o.id.empty() || o.id.size() > limits::max_identifier || !ids.insert(o.id).second ||
            std::find(kinds.begin(), kinds.end(), o.kind) == kinds.end() || !finite(o.position) ||
            !finite(o.size) || o.size.x < limits::min_size || o.size.y < limits::min_size ||
            o.size.x > limits::max_size || o.size.y > limits::max_size ||
            std::abs(o.position.x) > limits::max_position || std::abs(o.position.y) > limits::max_position ||
            o.layer < 0 || o.layer > limits::max_layer)
            throw std::runtime_error("Invalid object in prefab " + prefab.id + ": " + o.id);
        // The same door rule a room obeys. A prefab is a fragment of a room, and expansion turns
        // its children into that room's objects - so a door here with nowhere to lead would reach
        // the simulation as an object validate_scene would never have accepted.
        if (o.kind == "door" && !valid_door_target(o.target))
            throw std::runtime_error("Door in prefab " + prefab.id +
                                     " requires a simple stable scene ID, optionally /entrance: " + o.id);
        // A lever and an entrance may live in a prefab (R14): expansion names each
        // "<placement>.<child>", a door can name such an entrance, and a lever's gate inside the
        // same prefab is renamed with it.
        if (o.kind == "entrance" && !entrance_key(o.id))
            throw std::runtime_error("Entrance ID in prefab " + prefab.id +
                                     " must be a key a door can name: " + o.id);
        // And the same capability rule, for the same reason: expansion makes this a room's object.
        if (!o.capabilities.complete() && !kind_becomes_actor(o.kind))
            throw std::runtime_error("Capabilities on a " + o.kind + " in prefab " + prefab.id +
                                     " would do nothing: " + o.id);
        check_behaviour(o, " in prefab " + prefab.id);
    }
}
Prefab prefab_from_json(const Json &j) {
    if (j.at("version") != 1)
        throw std::runtime_error("Unsupported prefab version");
    Prefab prefab;
    prefab.id = j.at("id");
    for (const auto &v : j.at("objects"))
        prefab.objects.push_back(object_from_json(v));
    for (const auto &v : j.value("prefabs", Json::array()))
        prefab.instances.push_back(instance_from_json(v));
    validate_prefab(prefab);
    return prefab;
}
Json prefab_json(const Prefab &prefab) {
    Json j = {{"version", 1}, {"id", prefab.id}, {"objects", Json::array()}};
    for (const auto &o : prefab.objects)
        j["objects"].push_back(object_json(o));
    if (!prefab.instances.empty()) {
        j["prefabs"] = Json::array();
        for (const auto &instance : prefab.instances)
            j["prefabs"].push_back(instance_json(instance));
    }
    return j;
}
PrefabLibrary load_prefabs(const fs::path &project) {
    PrefabLibrary library;
    std::error_code ec;
    if (!fs::is_directory(project, ec))
        return library; // a project with no prefabs is a perfectly good project
    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(project, ec)) {
        const auto name = entry.path().filename().string();
        if (name.size() > 12 && name.compare(name.size() - 12, 12, ".prefab.json") == 0)
            files.push_back(entry.path());
    }
    // Sorted, so the library does not depend on directory order - which differs by filesystem.
    std::sort(files.begin(), files.end());
    for (const auto &path : files) {
        const auto name = path.filename().string();
        const auto stem = name.substr(0, name.size() - 12);
        try {
            auto prefab = prefab_from_json(read_json(path));
            if (prefab.id != stem)
                throw std::runtime_error("declares id '" + prefab.id + "'");
            if (library.find(prefab.id))
                throw std::runtime_error("duplicate prefab id");
            library.prefabs.push_back(std::move(prefab));
        } catch (const std::exception &e) {
            // Recorded, never thrown: a room placing a broken prefab must still open.
            library.errors.push_back(stem + ".prefab.json: " + e.what());
        }
    }
    return library;
}
namespace {
void expand_into(const std::vector<PrefabInstance> &instances, const PrefabLibrary &library, Vec2 origin,
                 const std::string &prefix, std::vector<std::string> &walked, std::vector<Object> &out,
                 std::vector<std::string> &problems, std::vector<std::pair<std::string, std::string>> *owners,
                 const std::string &root) {
    for (const auto &instance : instances) {
        const auto *prefab = library.find(instance.prefab);
        if (!prefab) {
            problems.push_back("Prefab '" + instance.prefab + "' placed by '" + instance.id +
                               "' is missing or failed to load");
            continue;
        }
        // A prefab that reaches itself would expand for ever. Detected on the path rather than
        // globally, because the same prefab placed twice side by side is perfectly legal.
        if (std::find(walked.begin(), walked.end(), instance.prefab) != walked.end()) {
            problems.push_back("Prefab '" + instance.prefab + "' contains itself through '" + instance.id +
                               "' and would never finish expanding");
            continue;
        }
        const auto here = origin + instance.position;
        const auto name = prefix.empty() ? instance.id : prefix + "." + instance.id;
        std::set<std::string> overridden;
        for (const auto &entry : instance.overrides) {
            overridden.insert(entry.child);
            const bool present = std::any_of(prefab->objects.begin(), prefab->objects.end(),
                                             [&](const Object &o) { return o.id == entry.child; });
            if (!present)
                problems.push_back("Override in '" + instance.id + "' names '" + entry.child +
                                   "', which prefab '" + instance.prefab + "' does not contain");
        }
        for (const auto &child : prefab->objects) {
            Object expanded = child;
            // Namespaced, so two placements of one prefab cannot collide and a reference into
            // a placement survives the prefab being renamed.
            expanded.id = name + "." + child.id;
            for (const auto &entry : instance.overrides) {
                if (entry.child != child.id)
                    continue;
                if (entry.position)
                    expanded.position = *entry.position;
                if (entry.size)
                    expanded.size = *entry.size;
                if (entry.target)
                    expanded.target = *entry.target;
                if (entry.layer)
                    expanded.layer = *entry.layer;
            }
            // A lever's gate inside the same prefab is renamed with it, so the pair still works
            // wherever and however often the prefab is placed (R14). A lever naming something the
            // prefab does not contain names the room's own object, and is left as written.
            if (expanded.kind == "lever" && !expanded.target.empty() &&
                std::any_of(prefab->objects.begin(), prefab->objects.end(),
                            [&](const Object &o) { return o.id == expanded.target; }))
                expanded.target = name + "." + expanded.target;
            expanded.position += here;
            // The *top-level* placement, not the nesting level that produced the object: a
            // creator selects and moves the thing they placed in the room, and a grandchild
            // has no separate existence there.
            if (owners)
                owners->push_back({expanded.id, root.empty() ? instance.id : root});
            out.push_back(std::move(expanded));
        }
        walked.push_back(instance.prefab);
        expand_into(prefab->instances, library, here, name, walked, out, problems, owners,
                    root.empty() ? instance.id : root);
        walked.pop_back();
    }
}
} // namespace
std::vector<Object> expand_prefabs(const Scene &scene, const PrefabLibrary &library,
                                   std::vector<std::string> &problems,
                                   std::vector<std::pair<std::string, std::string>> *owners) {
    std::vector<Object> out = scene.objects;
    std::vector<std::string> walked;
    expand_into(scene.prefabs, library, {}, "", walked, out, problems, owners, "");
    // And the walls and platforms its tile terrain makes, as the room is played.
    auto terrain = tile_objects(scene);
    out.insert(out.end(), std::make_move_iterator(terrain.begin()), std::make_move_iterator(terrain.end()));
    return out;
}

} // namespace med
