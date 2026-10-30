#ifndef XRCORE_HEADLESS_H_INCLUDED
#define XRCORE_HEADLESS_H_INCLUDED
#pragma once

// The engine build gets the real macro from xrCore.h (through stdafx.h); the
// host-side parser check (tests/cpp/headless) includes this header on its own.
#ifndef XRCORE_API
# define XRCORE_API
#endif

// ---------------------------------------------------------------------------
// Headless launch profiles (F0-04; PLAN §8.1, §8.11, NFR-ARCH-7).
//
// A headless process runs the engine without a render window, without a GPU
// device, without a sound device and without a local player actor, while the
// game / network loops keep ticking. Two profiles share one authoritative
// engine core, so the future dedicated server (US-2.8) reuses exactly this
// initialisation:
//   * server - authoritative simulation (A-Life, AI, physics, time, loot);
//   * client - network loop + snapshot apply + bot input, no drawing.
//
// The configuration is parsed once, before any subsystem is initialised, so
// render/sound/input/UI code never re-scans command line strings.
//
// Launch tokens (whole-token, case-insensitive match):
//   -headless_server                     authoritative simulation, no GPU/sound/UI
//   -headless_client                     network + snapshot apply + bot input
//   -headless                            alias of -headless_server
//   -headless_tick <hz>                  fixed headless loop rate (default 60)
//
// Environment (kept for the Wine/Proton harness, F0-11):
//   XRAY_HEADLESS=server|client|0        profile override
//   XRAY_HEADLESS_TICK_HZ=<hz>           fixed loop rate
//   XRAY_HEADLESS_WIDTH / _HEIGHT        fake backbuffer size (no real window)
//
// Debug launch flags (PLAN §8.11). They are off by default and only arm when
// the debug surface is explicitly allowed with -coop_debug:
//   -coop_debug                          allow the debug surface
//   -dbg_alife_off / -dbg_alife_on       A-Life simulation off/on
//   -dbg_god                             actor god-mode on
//   -dbg_teleport <x> <y> <z>            teleport the local actor
//   -dbg_spawn <section> <x> <y> <z>     force-spawn an entity (repeatable, 8 max)
//
// Environment mirrors: XRAY_DBG_ALIFE=0|1, XRAY_DBG_GOD=0|1,
// XRAY_DBG_TELEPORT="x y z", XRAY_DBG_SPAWN="section x y z".
// ---------------------------------------------------------------------------

// Every public getter below resolves the per-module configuration before it
// answers (see resolved_state() in headless.cpp): xrCore is a static library, so
// the renderer DLL owns its own copy of the state and would otherwise report
// "not headless" and silently take the GPU path.
namespace Headless {
enum EProfile {
    eProfileNone = 0,
    eProfileServer,
    eProfileClient,
};

// Reads CLI tokens and environment variables once. Idempotent: the first call
// wins, later calls are ignored. Must be called before engine initialisation.
//
// The executable calls this once at startup, but the state is per module (xrCore
// is a static library, so the renderer DLL owns its own copy): every getter
// bootstraps an unparsed module from the process command line, so a DLL that
// never calls Parse() still sees the headless profile.
XRCORE_API void Parse(const char* command_line);
XRCORE_API bool Parsed();

XRCORE_API EProfile Profile();
XRCORE_API bool Active();
XRCORE_API bool IsServer();
XRCORE_API bool IsClient();
XRCORE_API const char* ProfileName();  // "none" | "server" | "client"

// Subsystem gates: false in every headless profile.
XRCORE_API bool RenderEnabled();
XRCORE_API bool SoundEnabled();
XRCORE_API bool UiEnabled();
XRCORE_API bool LocalActorEnabled();

// Simulation gate: true in every headless profile (F0-05). The AI space and the
// Lua script engine are part of the simulation, not of the presentation layer,
// so a headless process builds them while a dedicated server (`g_dedicated_server`
// without a headless profile) does not - see xrEngine/ai_simulation.h.
XRCORE_API bool SimulationEnabled();

// Fixed simulation step of the headless loop (no vsync, no frame limiter).
XRCORE_API unsigned TickRateHz();

// Fake backbuffer size handed to the render device, which never creates a
// real swap chain in headless mode.
XRCORE_API void ConfigureBackbuffer(unsigned& width, unsigned& height);

// Paces one headless iteration to the fixed tick rate.
XRCORE_API void SleepToNextTick(unsigned elapsed_ms);

// Logs the resolved configuration (call once, after Core initialisation).
XRCORE_API void LogConfig();

// ---------------------------------------------------------------------------
// Debug launch flags (PLAN §8.11). Every getter returns the production-safe
// value unless the debug surface was explicitly requested at launch.
// ---------------------------------------------------------------------------
namespace Debug {
const unsigned kMaxSpawns = 8;
const unsigned kSectionMax = 64;

// True when -coop_debug was passed: the debug surface may be armed.
XRCORE_API bool Enabled();

// True when at least one -dbg_* token / env variable was supplied.
XRCORE_API bool AnyRequested();

XRCORE_API bool AlifeDisabled();
XRCORE_API bool GodMode();

XRCORE_API bool HasTeleport();
XRCORE_API void TeleportTarget(float& x, float& y, float& z);

XRCORE_API unsigned SpawnCount();
XRCORE_API const char* SpawnSection(unsigned index);
XRCORE_API void SpawnPosition(unsigned index, float& x, float& y, float& z);
}  // namespace Debug
}  // namespace Headless

#endif  // XRCORE_HEADLESS_H_INCLUDED
