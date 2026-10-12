#include "medium/profile.hpp"
namespace med {
std::span<const std::string_view> phase_names() {
    static constexpr std::string_view names[]{"input",   "simulation", "combat", "navigation", "assets",
                                              "network", "draw",       "submit", "present"};
    static_assert(std::size(names) == phase_count, "Every Phase needs a name; it is how it is reported");
    return names;
}
std::string_view phase_name(Phase phase) { return phase_names()[static_cast<size_t>(phase)]; }

Profiler::Scope::Scope(Profiler &owner, Phase phase)
    : owner_(&owner), token_(owner.open(phase, Clock::now())) {}
Profiler::Scope::Scope(Scope &&other) noexcept : owner_(other.owner_), token_(other.token_) {
    other.owner_ = nullptr; // the moved-from scope must not also record the time
}
Profiler::Scope::~Scope() {
    if (owner_)
        owner_->close(token_, Clock::now());
}

void Profiler::charge(Phase phase, Clock::duration elapsed) {
    const auto ms = std::chrono::duration<double, std::milli>(elapsed).count();
    if (std::isfinite(ms) && ms > 0)
        frame_[static_cast<size_t>(phase)] += ms;
}

uint64_t Profiler::open(Phase phase, Clock::time_point now) {
    if (!enabled_)
        return 0;
    if (depth_ == max_depth) {
        // Too deep to track. Timed on its own below, as the flat profiler always did; its parent
        // keeps running, so that stretch is counted twice - bounded, and visible as a depth.
        return 0;
    }
    if (depth_ > 0) {
        auto &parent = open_[depth_ - 1];
        charge(parent.phase, now - parent.since);
        parent.since = now;
    }
    open_[depth_++] = {next_token_, phase, now};
    return next_token_++;
}

void Profiler::close(uint64_t token, Clock::time_point now) {
    if (token == 0)
        return;
    size_t at = depth_;
    while (at > 0 && open_[at - 1].token != token)
        --at;
    if (at == 0)
        return; // already closed, or opened before a reset
    const auto closing = open_[at - 1];
    // Only the innermost scope is running. One closed out of order has been paused since its
    // child opened, and was charged up to that moment then.
    const bool running = at == depth_;
    if (running)
        charge(closing.phase, now - closing.since);
    ++calls_[static_cast<size_t>(closing.phase)];
    for (size_t i = at; i < depth_; ++i) // out of order: close the gap
        open_[i - 1] = open_[i];
    --depth_;
    // The scope now innermost resumes from here, so the time just charged is not charged twice.
    if (running && depth_ > 0)
        open_[depth_ - 1].since = now;
}

void Profiler::add(Phase phase, double milliseconds) {
    if (!enabled_ || !std::isfinite(milliseconds) || milliseconds < 0)
        return;
    const auto index = static_cast<size_t>(phase);
    frame_[index] += milliseconds;
    ++calls_[index];
}

void Profiler::end_frame() {
    if (!enabled_)
        return;
    for (size_t i = 0; i < phase_count; ++i) {
        history_[i][next_] = frame_[i];
        last_calls_[i] = calls_[i];
        frame_[i] = 0;
        calls_[i] = 0;
    }
    next_ = (next_ + 1) % profile_window;
    ++frames_;
}

PhaseStats Profiler::stats(Phase phase) const {
    PhaseStats out;
    const auto index = static_cast<size_t>(phase);
    out.calls = last_calls_[index];
    const size_t held = std::min(frames_, profile_window);
    out.frames = held;
    if (held == 0)
        return out;
    // Copied into a fixed buffer and sorted in place: percentiles need order, and allocating to
    // get it would make reporting the expensive part of the frame.
    std::array<double, profile_window> sorted{};
    for (size_t i = 0; i < held; ++i)
        sorted[i] = history_[index][i];
    std::sort(sorted.begin(), sorted.begin() + static_cast<long>(held));
    auto at = [&](double fraction) {
        const auto position = static_cast<size_t>(fraction * static_cast<double>(held - 1) + 0.5);
        return sorted[std::min(position, held - 1)];
    };
    out.p50 = at(0.50);
    out.p95 = at(0.95);
    out.p99 = at(0.99);
    out.worst = sorted[held - 1];
    double total = 0;
    for (size_t i = 0; i < held; ++i)
        total += sorted[i];
    out.mean = total / static_cast<double>(held);
    return out;
}

double Profiler::worst_case_total() const {
    double total = 0;
    for (size_t i = 0; i < phase_count; ++i)
        total += stats(static_cast<Phase>(i)).p95;
    return total;
}

void Profiler::reset() {
    depth_ = 0;
    history_ = {};
    frame_ = {};
    calls_ = {};
    last_calls_ = {};
    frames_ = 0;
    next_ = 0;
}

void FrameBudget::validate() const {
    if (!std::isfinite(frame_ms) || frame_ms <= 0)
        throw std::runtime_error("A frame budget needs a positive duration");
    double total = 0;
    for (size_t i = 0; i < phase_count; ++i) {
        if (!std::isfinite(phase_ms[i]) || phase_ms[i] < 0)
            throw std::runtime_error("Phase budget for " + std::string(phase_name(static_cast<Phase>(i))) +
                                     " is not a duration");
        total += phase_ms[i];
    }
    // A budget whose parts cannot fit inside the whole is not a budget, it is a wish. Refused at
    // the point it is written rather than discovered when nothing can meet it.
    if (total > frame_ms)
        throw std::runtime_error("Phase budgets total " + std::to_string(total) +
                                 " ms, which does not fit in a " + std::to_string(frame_ms) + " ms frame");
}

FrameBudget default_budget() {
    // A starting division of a 60 Hz frame, not a measurement. Stated so there is something to
    // fail against; the numbers want revisiting against a representative scene, which the
    // current fixtures are not.
    FrameBudget budget;
    budget.frame_ms = 16.67;
    budget.phase_ms[static_cast<size_t>(Phase::Input)] = 0.5;
    budget.phase_ms[static_cast<size_t>(Phase::Simulation)] = 2.5;
    budget.phase_ms[static_cast<size_t>(Phase::Combat)] = 1.5;
    budget.phase_ms[static_cast<size_t>(Phase::Navigation)] = 1.0;
    budget.phase_ms[static_cast<size_t>(Phase::Assets)] = 1.0;
    budget.phase_ms[static_cast<size_t>(Phase::Network)] = 1.0;
    budget.phase_ms[static_cast<size_t>(Phase::Draw)] = 3.0;
    budget.phase_ms[static_cast<size_t>(Phase::Submit)] = 2.0;
    // Deliberately unbudgeted: this is where the frame waits for the display, and a large value
    // here is headroom rather than cost. Measured at p95 16.2 ms on this host against 0.56 ms of
    // actual work, which is exactly the shape it should have.
    budget.phase_ms[static_cast<size_t>(Phase::Present)] = 0;
    budget.validate();
    return budget;
}

BudgetReport against(const Profiler &profiler, const FrameBudget &budget) {
    budget.validate();
    BudgetReport report;
    for (size_t i = 0; i < phase_count; ++i) {
        const auto phase = static_cast<Phase>(i);
        const auto measured = profiler.stats(phase);
        if (budget.phase_ms[i] <= 0) {
            report.p95_unbudgeted += measured.p95;
            continue; // the vsync wait is idleness, and idleness is not a cost
        }
        report.p95_total += measured.p95;
        if (measured.p95 > budget.phase_ms[i])
            report.over.emplace_back(phase, measured.p95 - budget.phase_ms[i]);
    }
    std::sort(report.over.begin(), report.over.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    report.within = report.over.empty() && report.p95_total <= budget.frame_ms;
    return report;
}
} // namespace med
