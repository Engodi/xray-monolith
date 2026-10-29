// Host-side check of the simulation/presentation split (F0-05).
//
// The predicate that decides "does this process own an AI space and a Lua
// script engine?" is plain C++: `xrEngine/ai_simulation.h` composes the legacy
// dedicated-server flag with the headless profile resolved by
// `xrCore/headless.cpp`. Both are independent of Windows, so the truth table is
// verified here before the engine is ever built by CI.
//
// One process parses exactly one launch line (Headless::Parse is one-shot), so
// run.sh compiles this file against the real sources and drives the matrix.
//
// argv: [1] launch line  [2] g_dedicated_server (0|1)  [3] expected (0|1)
#include "xrEngine/ai_simulation.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// The engine defines this in xrEngine/x_ray.cpp; the host test owns it.
bool g_dedicated_server = false;

int main(int argc, char** argv) {
    const char* launch = (argc > 1) ? argv[1] : "";
    const bool dedicated = (argc > 2) && 0 == std::strcmp(argv[2], "1");
    const bool expected = (3 > argc) || 0 != std::atoi(argv[3]);

    Headless::Parse(launch);
    g_dedicated_server = dedicated;

    int failures = 0;

    // The headless profile itself is the source of the simulation capability.
    if (Headless::SimulationEnabled() != Headless::Active()) {
        std::printf("FAIL: SimulationEnabled() must follow the headless profile\n");
        ++failures;
    }

    const bool actual = ai_simulation_enabled();
    std::printf("launch='%s' dedicated=%d profile=%s ai_simulation=%d expected=%d\n", launch,
                dedicated ? 1 : 0, Headless::ProfileName(), actual ? 1 : 0, expected ? 1 : 0);
    if (actual != expected) {
        std::printf("FAIL: ai_simulation_enabled() = %d, expected %d\n", actual ? 1 : 0,
                    expected ? 1 : 0);
        ++failures;
    }

    return failures ? 1 : 0;
}
