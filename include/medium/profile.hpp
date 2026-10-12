#pragma once
#include "medium/core.hpp"
#include <chrono>
namespace med {
// Where a frame's time goes, per phase, with percentiles over a rolling window.
//
// `main` already reported whole-frame p50/p95/p99. That tells you a frame was slow and nothing
// about why - and "the frame took 22 ms" is the one measurement that never suggests what to do
// next. This attributes it.
//
// Everything here is allocation-free and bounded. A profiler that allocated would change what it
// was measuring, and one that grew without limit would eventually become the thing worth
// profiling.

enum class Phase : uint8_t {
    Input,      // device polling and command resolution
    Simulation, // actor ticks: intent, rules, resolution
    Combat,     // hit detection and resolution, projectiles
    Navigation, // route queries
    Assets,     // staging and activation
    Network,    // packet handling and reconciliation
    Draw,       // building the sprite queue
    Submit,     // handing work to the GPU
    Present     // swap and wait
};
inline constexpr size_t phase_count = 9;
std::span<const std::string_view> phase_names();
std::string_view phase_name(Phase phase);

// Frames kept for percentiles. Four seconds at 60 Hz: long enough that a percentile means
// something, short enough that a stall stops being reported once it has actually stopped.
inline constexpr size_t profile_window = 240;

struct PhaseStats {
    double p50 = 0, p95 = 0, p99 = 0, worst = 0, mean = 0;
    size_t frames = 0;
    // Calls in the most recent frame, so "slow" can be told from "called far too often".
    size_t calls = 0;
};

class Profiler {
  public:
    using Clock = std::chrono::steady_clock;
    // How deep scopes may nest before an inner one is timed on its own, without pausing its parent.
    static constexpr size_t max_depth = 16;

  private:
    // Open scopes, innermost last. A scope opened inside another *pauses* it, so every phase's
    // time is its own: the simulation's phase excludes the combat and navigation inside it, and a
    // phase total never counts a millisecond twice. Fixed capacity, so timing allocates nothing.
    struct Open {
        uint64_t token = 0;
        Phase phase{};
        Clock::time_point since{};
    };
    std::array<Open, max_depth> open_{};
    size_t depth_ = 0;
    uint64_t next_token_ = 1;
    void charge(Phase phase, Clock::duration elapsed);
    std::array<std::array<double, profile_window>, phase_count> history_{};
    std::array<double, phase_count> frame_{};
    std::array<size_t, phase_count> calls_{};
    std::array<size_t, phase_count> last_calls_{};
    size_t frames_ = 0, next_ = 0;
    bool enabled_ = true;

  public:
    // RAII. Deliberately not nestable within the same phase: phases are meant to partition a
    // frame, and a phase that contained itself would count its own time twice and quietly report
    // a frame busier than it was.
    class Scope {
        Profiler *owner_;
        uint64_t token_;

      public:
        Scope(Profiler &owner, Phase phase);
        ~Scope();
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
        // Movable, so a scope can be held in an optional and closed early - a phase that ends
        // partway through a block is common and should not need the block restructured around it.
        Scope(Scope &&other) noexcept;
        Scope &operator=(Scope &&) = delete;
    };
    Scope scope(Phase phase) { return Scope(*this, phase); }
    // The mechanism under `Scope`, with the time given rather than read, so the nesting rule can be
    // tested exactly. `open` pauses whatever is open; `close` charges the phase and resumes it. A
    // token closed out of order is still charged correctly. Token 0 means "not tracked": either
    // the profiler was disabled when it opened, or the nesting was already `max_depth` deep, in
    // which case it is timed on its own without pausing anything.
    uint64_t open(Phase phase, Clock::time_point now);
    void close(uint64_t token, Clock::time_point now);
    size_t depth() const { return depth_; }

    void enable(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }
    // Adds time to a phase directly, for a caller that already measured it.
    void add(Phase phase, double milliseconds);
    // Closes the frame: its per-phase totals enter the window and the accumulators reset.
    void end_frame();
    size_t frames() const { return frames_; }
    PhaseStats stats(Phase phase) const;
    // Every phase's p95 added together. Not the same as the frame's own p95 - a frame whose
    // phases each spike at different times has a lower p95 than the sum - so it reads as a budget
    // ceiling rather than as a measurement of any real frame.
    double worst_case_total() const;
    void reset();
};

// What a frame is allowed to cost, and whether it did.
//
// Budgets are stated per phase rather than only for the whole frame, because a frame that has gone
// over is a question ("which part?") and a phase that has gone over is an answer.
struct FrameBudget {
    double frame_ms = 16.67; // 60 Hz
    // Zero means **unbudgeted**, and one phase is unbudgeted on purpose. `Present` is where the
    // frame blocks on the display, so its duration is the refresh interval minus everything else
    // - it measures idleness, not cost. Counting it would make a frame that finished its work in
    // half a millisecond and then waited read as sixteen milliseconds over budget, which is the
    // opposite of what the number is for.
    std::array<double, phase_count> phase_ms{};
    // Throws on a budget that cannot be met by construction - budgeted phases summing past the
    // frame.
    void validate() const;
};
FrameBudget default_budget();
struct BudgetReport {
    bool within = true;
    // Phases whose p95 exceeded their budget, worst first.
    std::vector<std::pair<Phase, double>> over;
    // The p95 of the **budgeted** phases added together: what the engine costs. Unbudgeted
    // phases - the vsync wait - are excluded, so this is work rather than wall clock.
    double p95_total = 0;
    // And what the wait costs, reported separately because a large one is *good*: it is the
    // headroom between finishing the frame and the display asking for the next.
    double p95_unbudgeted = 0;
};
BudgetReport against(const Profiler &profiler, const FrameBudget &budget);
} // namespace med
