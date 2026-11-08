// A sleeping nested stage must contribute wall time, not its parent's time.
#include "dect2/common/stage_prof.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace dect2::prof;
static void check(bool ok, const char* message) {
    if (!ok) { fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    static Stage outer("test.outer"), inner("test.inner");
    stageProfEnable(false);
    { Scope disabled(outer); }
    check(outer.calls == 0 && outer.wallNs == 0, "disabled profiler recorded work");
    stageProfEnable(true);
    const auto start = wallNs();
    {
        Scope parent(outer);
        {
            Scope child(inner);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            child.stop();
            child.stop(); // explicit stop and destruction must not count twice
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    const auto elapsed = wallNs() - start;
    const auto measured = outer.wallNs.load() + inner.wallNs.load();
    check(outer.calls == 1 && inner.calls == 1, "nested stage call counts differ");
    check(inner.wallNs >= 35000000 && outer.wallNs >= 35000000, "wait was not measured");
    check(measured <= elapsed, "nested wall time was double-counted");
    check(outer.ns + inner.ns < measured, "sleep was counted as CPU time");
    bool found = false;
    for (const auto& item : stageProfSnapshot(true)) {
        if (item.first == "test.inner") {
            found = item.second.second == 1 && item.second.first >= .035;
        }
    }
    check(found, "wall snapshot omitted sleeping stage");
    stageProfReset();
    check(outer.calls == 0 && inner.calls == 0 && outer.ns == 0 && inner.ns == 0 &&
          outer.wallNs == 0 && inner.wallNs == 0, "reset retained profile counters");
    puts("stage profile: disabled, nested wait, CPU/wall separation, stop and reset passed");
}
