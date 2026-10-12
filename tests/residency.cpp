#include "medium/residency.hpp"
#include <iostream>
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
        auto position = [](const std::vector<std::string> &order, const std::string &id) {
            return std::find(order.begin(), order.end(), id) - order.begin();
        };

        // ---- The graph ----
        {
            AssetGraph graph;
            graph.add({"sheet", {"wanderer.png"}, {}});
            graph.add({"actor", {"wanderer.anim.json"}, {"sheet"}});
            graph.add({"room", {"courtyard.scene.json"}, {"actor"}});
            graph.validate();
            ++checks;
            check(graph.size() == 3 && graph.find("actor") && !graph.find("absent"),
                  "It holds what it was given and reports what it does not have");

            AssetGraph missing;
            missing.add({"actor", {"a.json"}, {"sheet"}});
            rejects([&] { missing.validate(); },
                    "A dependency that is not in the graph is refused, not ignored");
            AssetGraph twice;
            twice.add({"actor", {"a.json"}, {}});
            twice.add({"actor", {"b.json"}, {}});
            rejects([&] { twice.validate(); }, "And so are two assets sharing an id");
            AssetGraph circular;
            circular.add({"a", {"a.json"}, {"b"}});
            circular.add({"b", {"b.json"}, {"a"}});
            bool named = false;
            try {
                circular.validate();
            } catch (const std::runtime_error &e) {
                named = std::string(e.what()).find("a -> b") != std::string::npos;
            }
            check(named, "A cycle is reported by its members: \"there is a cycle\" is not something "
                         "anyone can act on");
        }

        // ---- Staleness, and the order it comes back in ----
        // The property that matters. Changing a texture must restage the animation built on it and
        // the room built on that, and must hand them back dependency-first - rebuilding a dependent
        // against a version about to be replaced is two loads to reach the wrong answer once.
        {
            AssetGraph graph;
            graph.add({"sheet", {"wanderer.png"}, {}});
            graph.add({"actor", {"wanderer.anim.json"}, {"sheet"}});
            graph.add({"room", {"courtyard.scene.json"}, {"actor"}});
            graph.add({"unrelated", {"weapons.json"}, {}});

            const fs::path touched[]{"wanderer.png"};
            const auto stale = graph.stale_from(touched);
            check(stale.size() == 3, "A texture change reaches everything built on it");
            check(position(stale, "sheet") < position(stale, "actor") &&
                      position(stale, "actor") < position(stale, "room"),
                  "In dependency order: the thing that changed first, then what stands on it");
            check(std::find(stale.begin(), stale.end(), "unrelated") == stale.end(),
                  "And nothing that does not depend on it - this is what stops one saved file "
                  "reloading the whole project");

            const fs::path leaf[]{"courtyard.scene.json"};
            check(graph.stale_from(leaf) == std::vector<std::string>{"room"},
                  "Changing a leaf restages only the leaf");
            const fs::path nothing[]{"unknown.json"};
            check(graph.stale_from(nothing).empty(), "A file nothing is built from changes nothing");
            const fs::path both[]{"wanderer.png", "weapons.json"};
            check(graph.stale_from(both).size() == 4, "Two changes reach the union of what they touch");

            // A diamond: two assets on one dependency, one thing on both.
            AssetGraph diamond;
            diamond.add({"base", {"base.png"}, {}});
            diamond.add({"left", {"left.json"}, {"base"}});
            diamond.add({"right", {"right.json"}, {"base"}});
            diamond.add({"top", {"top.json"}, {"left", "right"}});
            const fs::path root_changed[]{"base.png"};
            const auto order = diamond.stale_from(root_changed);
            check(order.size() == 4, "Everything downstream of a shared dependency is stale");
            check(position(order, "base") < position(order, "left") &&
                      position(order, "base") < position(order, "right") && position(order, "top") == 3,
                  "And the thing standing on both comes last, after both of them");
        }

        // ---- Residency, and what a failure must not do ----
        {
            ResidencyTable table;
            check(table.state("actor") == Residency::Absent && table.resident("actor") == 0,
                  "Nothing is resident before anything is loaded");
            rejects([&] { table.activate("actor"); },
                    "Activating what was never staged is refused: it would present later as a "
                    "missing asset with no explanation");

            const auto first = table.stage("actor");
            check(table.state("actor") == Residency::Staged && table.staged("actor") == first,
                  "A prepared candidate waits rather than taking effect");
            check(table.resident("actor") == 0, "And nothing is in use yet");
            table.activate("actor");
            check(table.state("actor") == Residency::Resident && table.resident("actor") == first &&
                      table.staged("actor") == 0,
                  "Activation is a separate, chosen moment");

            // The whole reason staging and activation are separate.
            const auto second = table.stage("actor");
            check(second != first, "A second candidate gets its own revision");
            table.fail("actor", "half-written file");
            check(table.state("actor") == Residency::Resident && table.resident("actor") == first,
                  "A candidate that will not load leaves the working one exactly where it was - an "
                  "authoring session a few seconds stale beats one with a blank character in it");
            check(table.failure("actor") == "half-written file",
                  "And the reason is kept, because the creator has to be told what to fix");
            check(table.staged("actor") == 0, "The rejected candidate is gone");

            const auto third = table.stage("actor");
            table.activate("actor");
            check(table.resident("actor") == third && table.failure("actor").empty(),
                  "A later candidate that does load clears the complaint");

            // An asset that has never been resident has nothing to retain.
            ResidencyTable fresh;
            fresh.stage("sheet");
            fresh.fail("sheet", "missing");
            check(fresh.state("sheet") == Residency::Failed,
                  "A first load that fails is failed, not silently absent");
        }

        // ---- The activation boundary ----
        {
            ResidencyTable table;
            table.stage("sheet");
            table.stage("actor");
            table.stage("room");
            check(table.pending() == std::vector<std::string>{"sheet", "actor", "room"},
                  "Pending work comes back in the order it was staged, which is the dependency "
                  "order the graph produced - not alphabetical, which would put the room first");
            const auto activated = table.activate_pending();
            check(activated.size() == 3 && table.pending().empty(),
                  "One boundary activates everything waiting");
            for (auto id : {"sheet", "actor", "room"})
                check(table.state(id) == Residency::Resident, "And all of it is in use afterwards");

            table.stage("actor");
            check(table.pending() == std::vector<std::string>{"actor"},
                  "A later change stages only what changed");
            table.forget("actor");
            check(table.state("actor") == Residency::Absent, "An asset can be dropped entirely");
        }

        // ---- Who may talk to the GPU ----
        {
            UploadOwner owner;
            check(owner.owned(), "The thread that constructed it owns uploads");
            owner.check("activation");
            ++checks;
            std::string elsewhere;
            std::thread other([&] {
                try {
                    owner.check("activation");
                } catch (const std::exception &e) {
                    elsewhere = e.what();
                }
            });
            other.join();
            check(!elsewhere.empty(),
                  "And any other thread is refused, rather than discovering the rule by crashing "
                  "somewhere inside a driver");
            check(elsewhere.find("activation") != std::string::npos,
                  "Naming what it was trying to do, because the stack will not say");
        }

        std::cout << "PASS " << checks << " asset residency checks\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL after " << checks << " residency checks: " << e.what() << '\n';
        return 1;
    }
}
