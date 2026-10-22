// Host-side check of the headless launch-flag parser (F0-04).
//
// Runs on any host with a C++17 compiler: the parser only needs the tiny
// stdafx stub from ./stub, not the Windows engine. One process parses exactly
// one launch line (Parse() is a one-shot), so the matrix lives in run.sh.
//
// expected values come from argv:
//   [1] launch line   [2] profile           [3] tick hz        [4] debug armed
//   [5] alife off     [6] god mode          [7] has teleport   [8] spawn count
//   [9] teleport x    [10] first spawn section
#include "headless.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

void expect_text(const char* what, const char* expected, const char* actual) {
    if (0 != std::strcmp(expected, actual)) {
        std::printf("FAIL: %s expected '%s', got '%s'\n", what, expected, actual);
        ++failures;
    }
}

bool flag_arg(int argc, char** argv, int index, bool fallback) {
    if (argc <= index) {
        return fallback;
    }
    return 0 == std::strcmp(argv[index], "1");
}
}  // namespace

int main(int argc, char** argv) {
    const char* launch = (argc > 1) ? argv[1] : "";
    Headless::Parse(launch);

    expect(Headless::Parsed(), "parsed flag");
    expect_text("profile", (argc > 2) ? argv[2] : "none", Headless::ProfileName());

    const unsigned expected_tick = (argc > 3) ? unsigned(::atoi(argv[3])) : 60u;
    expect(Headless::TickRateHz() == expected_tick, "tick rate");

    const bool debug_armed = flag_arg(argc, argv, 4, false);
    expect(Headless::Debug::Enabled() == debug_armed, "debug surface gate");

    // Debug values stay production-safe unless -coop_debug armed the surface.
    expect(Headless::Debug::AlifeDisabled() == flag_arg(argc, argv, 5, false), "alife flag");
    expect(Headless::Debug::GodMode() == flag_arg(argc, argv, 6, false), "god flag");
    expect(Headless::Debug::HasTeleport() == flag_arg(argc, argv, 7, false), "teleport flag");

    const unsigned expected_spawns = (argc > 8) ? unsigned(::atoi(argv[8])) : 0u;
    expect(Headless::Debug::SpawnCount() == expected_spawns, "spawn count");

    if (argc > 9 && Headless::Debug::HasTeleport()) {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        Headless::Debug::TeleportTarget(x, y, z);
        expect(x == float(::atof(argv[9])), "teleport x");
    }

    if (argc > 10 && Headless::Debug::SpawnCount() > 0) {
        expect_text("first spawn section", argv[10], Headless::Debug::SpawnSection(0));
    }

    if (Headless::Active()) {
        expect(!Headless::RenderEnabled(), "render gate closed");
        expect(!Headless::SoundEnabled(), "sound gate closed");
        expect(!Headless::UiEnabled(), "ui gate closed");
        expect(!Headless::LocalActorEnabled(), "local actor gate closed");

        unsigned width = 0;
        unsigned height = 0;
        Headless::ConfigureBackbuffer(width, height);
        expect(width == 640 && height == 480, "default headless backbuffer");
    } else {
        expect(Headless::RenderEnabled(), "render gate open without headless");
        expect(Headless::SoundEnabled(), "sound gate open without headless");
        expect(Headless::UiEnabled(), "ui gate open without headless");
    }

    return failures ? 1 : 0;
}
