#ifndef XRENGINE_AI_SIMULATION_H_INCLUDED
#define XRENGINE_AI_SIMULATION_H_INCLUDED
#pragma once

// ---------------------------------------------------------------------------
// Simulation capability of the running process (F0-05, NFR-ARCH-4/7).
//
// Two runtime modes share the engine core:
//   * a headless profile (-headless_server / -headless_client) is a *full*
//     simulation without presentation: AI space (EF storage, game/level graph,
//     cover manager, patrol paths, moving objects, doors) and the Lua script
//     engine are built exactly like in an interactive game, so gameplay
//     scripts, AI and the coop Lua bridge work;
//   * the legacy dedicated server (DEDICATED_SERVER build or an explicit
//     dedicated launch) is a network stub: no GPU, no audio, no UI, no local
//     actor *and no AI simulation*.
//
// F0-04 keeps the `g_dedicated_server` flag on for a headless process, because
// it is the engine-wide "no presentation" gate (renderer, audio, console,
// statistics, particles). That flag therefore no longer means "dedicated
// server" - it means "presentation is off". Every decision about the AI space
// or the script engine must use ai_simulation_enabled() instead, otherwise a
// headless process ends up with `ai().script_engine() == nullptr` and the Lua
// bridge dereferences it on level load.
// ---------------------------------------------------------------------------

#include "../xrCore/headless.h"

extern ENGINE_API bool g_dedicated_server;

// True when the process owns an AI space and a Lua script engine.
inline bool ai_simulation_enabled() { return !g_dedicated_server || Headless::SimulationEnabled(); }

#endif  // XRENGINE_AI_SIMULATION_H_INCLUDED
