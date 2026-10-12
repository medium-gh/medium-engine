#include "medium/profile.hpp"
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>
using namespace med;

int main() {
    int checks = 0;
    try {
        auto check = [&](bool pass, const char *message) {
            ++checks;
            if (!pass)
                throw std::runtime_error(message);
        };
        auto rejects = [&](auto action, const char *message) {
            bool threw = false;
            try {
                action();
            } catch (const std::runtime_error &) {
                threw = true;
            }
            check(threw, message);
        };

        // ---- Names ----
        {
            check(phase_names().size() == phase_count, "Every phase has a name; it is how it is reported");
            check(phase_name(Phase::Simulation) == "simulation", "And the name is the obvious one");
        }

        // ---- Accumulating and closing a frame ----
        {
            Profiler profiler;
            check(profiler.frames() == 0 && profiler.stats(Phase::Draw).p50 == 0,
                  "Nothing has been measured yet");
            profiler.add(Phase::Draw, 4);
            profiler.add(Phase::Draw, 2);
            check(profiler.stats(Phase::Draw).frames == 0,
                  "A frame in progress is not in the window yet: a percentile over a half-finished "
                  "frame would read low every time it was asked mid-frame");
            profiler.end_frame();
            const auto drawn = profiler.stats(Phase::Draw);
            check(drawn.frames == 1 && drawn.p50 == 6,
                  "Closing the frame files the phase's total, which is the sum of its calls");
            check(drawn.calls == 2, "And how many calls made it up, so slow can be told from too often");
            profiler.add(Phase::Draw, 1);
            profiler.end_frame();
            check(profiler.stats(Phase::Draw).calls == 1, "Call counts are per frame, not cumulative");
            check(profiler.frames() == 2, "Two frames have been closed");
        }

        // ---- Percentiles ----
        {
            Profiler profiler;
            for (int frame = 1; frame <= 100; ++frame) {
                profiler.add(Phase::Simulation, static_cast<double>(frame));
                profiler.end_frame();
            }
            const auto measured = profiler.stats(Phase::Simulation);
            check(measured.frames == 100, "A hundred frames are held");
            check(measured.p50 >= 50 && measured.p50 <= 51, "The median is in the middle");
            check(measured.p95 >= 95 && measured.p95 <= 96, "The 95th is near the top");
            check(measured.p99 >= 99 && measured.p99 <= 100, "And the 99th nearer still");
            check(measured.worst == 100, "The worst is the worst");
            check(measured.mean >= 50 && measured.mean <= 51, "And the mean is the mean");
            check(measured.p50 < measured.p95 && measured.p95 <= measured.p99,
                  "Percentiles are ordered, which is the one thing a percentile must be");
        }

        // ---- The window is bounded, and forgets ----
        // A profiler that never forgot would report a stall long after it stopped, which is the
        // opposite of useful: the reason to watch p95 is to know whether it is happening *now*.
        {
            Profiler profiler;
            for (size_t i = 0; i < profile_window; ++i) {
                profiler.add(Phase::Present, 40); // a very bad stretch
                profiler.end_frame();
            }
            check(profiler.stats(Phase::Present).p50 == 40, "The bad stretch is what is held");
            for (size_t i = 0; i < profile_window; ++i) {
                profiler.add(Phase::Present, 1);
                profiler.end_frame();
            }
            const auto recovered = profiler.stats(Phase::Present);
            check(recovered.frames == profile_window, "The window stays its size rather than growing");
            check(recovered.worst == 1,
                  "And the stall is gone once it has actually stopped, because the window rolled "
                  "past it - a profiler that never forgets reports history, not health");
            check(profiler.frames() == profile_window * 2, "Though the frame count is honest about both");
        }

        // ---- Scopes measure real time ----
        {
            Profiler profiler;
            {
                auto scope = profiler.scope(Phase::Assets);
                std::this_thread::sleep_for(std::chrono::milliseconds(12));
            }
            profiler.end_frame();
            const auto measured = profiler.stats(Phase::Assets);
            check(measured.p50 >= 10, "A scope measures the time actually spent inside it");
            check(measured.p50 < 200, "And not wildly more than it");
            check(profiler.stats(Phase::Draw).p50 == 0, "Other phases are untouched");
        }

        // ---- Turning it off ----
        {
            Profiler profiler;
            profiler.enable(false);
            profiler.add(Phase::Draw, 5);
            profiler.end_frame();
            check(profiler.frames() == 0 && profiler.stats(Phase::Draw).p50 == 0,
                  "A disabled profiler records nothing, so it can be left in a shipping build");
            profiler.enable(true);
            profiler.add(Phase::Draw, 5);
            profiler.end_frame();
            check(profiler.stats(Phase::Draw).p50 == 5, "And resumes when it is turned back on");
            profiler.reset();
            check(profiler.frames() == 0 && profiler.stats(Phase::Draw).p50 == 0, "Reset clears it");
        }

        // ---- Nonsense is ignored rather than recorded ----
        {
            Profiler profiler;
            profiler.add(Phase::Draw, std::numeric_limits<double>::quiet_NaN());
            profiler.add(Phase::Draw, -5);
            profiler.add(Phase::Draw, 3);
            profiler.end_frame();
            check(profiler.stats(Phase::Draw).p50 == 3,
                  "A clock that went backwards contributes nothing rather than poisoning the "
                  "window it lands in");
            check(profiler.stats(Phase::Draw).calls == 1, "And is not counted as a call either");
        }

        // ---- Budgets ----
        {
            auto budget = default_budget();
            budget.validate();
            ++checks;
            check(budget.frame_ms > 16 && budget.frame_ms < 17, "The default frame is 60 Hz");

            FrameBudget impossible;
            impossible.frame_ms = 16.67;
            for (size_t i = 0; i < phase_count; ++i)
                impossible.phase_ms[i] = 5;
            rejects([&] { impossible.validate(); },
                    "A budget whose parts cannot fit inside the whole is refused where it is "
                    "written, not discovered when nothing can meet it");
            FrameBudget negative;
            negative.phase_ms[0] = -1;
            rejects([&] { negative.validate(); }, "And a negative phase budget is not a duration");
            FrameBudget no_frame;
            no_frame.frame_ms = 0;
            rejects([&] { no_frame.validate(); }, "Nor is a frame of no length");

            Profiler profiler;
            for (int frame = 0; frame < 50; ++frame) {
                profiler.add(Phase::Simulation, 1.0);
                profiler.add(Phase::Draw, 1.0);
                profiler.end_frame();
            }
            const auto comfortable = against(profiler, budget);
            check(comfortable.within && comfortable.over.empty(), "A cheap frame is within budget");
            check(comfortable.p95_total > 1.9 && comfortable.p95_total < 2.1,
                  "And the total is the sum of what the phases actually cost");

            Profiler heavy;
            for (int frame = 0; frame < 50; ++frame) {
                heavy.add(Phase::Simulation, 9.0); // budget is 2.5
                heavy.add(Phase::Draw, 6.0);       // budget is 3.0
                heavy.end_frame();
            }
            const auto report = against(heavy, budget);
            check(!report.within, "A frame over budget says so");
            check(report.over.size() == 2, "Naming every phase that went over, not just the frame");
            check(report.over.front().first == Phase::Simulation,
                  "Worst first, because that is where to look - a frame over budget is a question "
                  "and a phase over budget is an answer");
            check(report.over.front().second > 6 && report.over.front().second < 7,
                  "By how much, so the size of the problem is visible");

            // The vsync wait is not a cost. A frame that finished its work in half a millisecond
            // and then waited fifteen for the display is a *fast* frame, and a budget that read it
            // as fifteen milliseconds over would be measuring the display's pace as our failure.
            {
                Profiler waiting;
                for (int frame = 0; frame < 50; ++frame) {
                    waiting.add(Phase::Simulation, 0.4);
                    waiting.add(Phase::Present, 15.6); // blocked on vsync
                    waiting.end_frame();
                }
                const auto idle = against(waiting, budget);
                check(idle.within, "A frame that did its work quickly and then waited is within budget");
                check(idle.p95_total < 1,
                      "The total is what the engine cost, not what the display made it wait");
                check(idle.p95_unbudgeted > 15,
                      "And the wait is reported separately, because a large one is headroom");
            }

            // Every phase inside its own budget, but the frame still over.
            Profiler spread;
            for (int frame = 0; frame < 50; ++frame) {
                for (size_t i = 0; i < phase_count; ++i)
                    spread.add(static_cast<Phase>(i), budget.phase_ms[i] * 0.99);
                spread.add(Phase::Draw, 5.0); // pushes the budgeted total past the frame
                spread.end_frame();
            }
            const auto total = against(spread, budget);
            check(!total.within,
                  "A frame can be over even when most phases are inside their own share, and the "
                  "report has to notice that rather than only checking phases");
        }

        // ---- Nested scopes count their time once ----
        // A scope opened inside another pauses it. Before this, a navigation query inside the
        // simulation phase counted in both, and the client's Assets phase silently contained the
        // Input and Network phases timed inside it - so the phase total counted the same
        // milliseconds twice. Times are given explicitly, so the rule is checked exactly.
        {
            using Clock = Profiler::Clock;
            const auto t0 = Clock::time_point{};
            auto at = [&](double ms) {
                return t0 + std::chrono::duration_cast<Clock::duration>(
                                std::chrono::duration<double, std::milli>(ms));
            };
            auto near = [](double a, double b) { return std::abs(a - b) < 1e-6; };
            Profiler nested;
            const auto outer = nested.open(Phase::Simulation, at(0));
            const auto inner = nested.open(Phase::Combat, at(2));
            const auto deepest = nested.open(Phase::Navigation, at(3));
            check(nested.depth() == 3, "Three scopes are open");
            nested.close(deepest, at(4.5));
            nested.close(inner, at(5));
            nested.close(outer, at(10));
            nested.end_frame();
            check(near(nested.stats(Phase::Simulation).p50, 2 + 5) &&
                      near(nested.stats(Phase::Combat).p50, 1 + 0.5) &&
                      near(nested.stats(Phase::Navigation).p50, 1.5),
                  "Each phase is charged only while it is the innermost open scope");
            check(nested.stats(Phase::Simulation).calls == 1 && nested.stats(Phase::Combat).calls == 1,
                  "And a paused-and-resumed scope is still one call");
            check(near(nested.stats(Phase::Simulation).p50 + nested.stats(Phase::Combat).p50 +
                           nested.stats(Phase::Navigation).p50,
                       10),
                  "So the phases add up to the wall time, never more");

            // Closed out of order: the outer scope first, while the inner one runs.
            Profiler crossed;
            const auto a = crossed.open(Phase::Assets, at(0));
            const auto b = crossed.open(Phase::Input, at(1));
            crossed.close(a, at(3)); // paused since 1; charged nothing more
            crossed.close(b, at(4));
            crossed.end_frame();
            check(near(crossed.stats(Phase::Assets).p50, 1) && near(crossed.stats(Phase::Input).p50, 3) &&
                      crossed.depth() == 0,
                  "A scope closed out of order was paused and is not charged for its child's time");
            crossed.close(a, at(9));
            check(crossed.depth() == 0, "Closing a scope twice does nothing");

            Profiler off;
            off.enable(false);
            check(off.open(Phase::Draw, at(0)) == 0 && off.depth() == 0, "A disabled profiler opens nothing");

            Profiler deep;
            std::vector<uint64_t> tokens;
            for (size_t i = 0; i < Profiler::max_depth + 2; ++i)
                tokens.push_back(deep.open(Phase::Draw, at(static_cast<double>(i))));
            check(deep.depth() == Profiler::max_depth && tokens.back() == 0,
                  "Past the fixed depth a scope is untracked rather than allocating");
            for (auto it = tokens.rbegin(); it != tokens.rend(); ++it)
                deep.close(*it, at(100));
            check(deep.depth() == 0, "And everything tracked still closes");
        }

        std::cout << "PASS " << checks << " profiling checks\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL after " << checks << " profiling checks: " << e.what() << '\n';
        return 1;
    }
}
