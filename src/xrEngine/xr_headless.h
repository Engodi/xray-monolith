#ifndef XRENGINE_HEADLESS_RUNTIME_H_INCLUDED
#define XRENGINE_HEADLESS_RUNTIME_H_INCLUDED
#pragma once

// ---------------------------------------------------------------------------
// Engine glue for the headless profiles (F0-04).
//
// The launch configuration itself lives in xrCore/headless.h; this layer wires
// it into the engine runtime:
//   * a frame ticker keeps the headless loop observable (heartbeat in the log);
//   * debug launch flags (PLAN §8.11) are translated into debug-surface
//     commands and executed once the level is ready.
//
// Both calls are no-ops when no headless profile is active, so the normal
// game path pays nothing but a single flag test.
// ---------------------------------------------------------------------------
namespace Headless {
void InitializeRuntime();
void ShutdownRuntime();
}  // namespace Headless

#endif  // XRENGINE_HEADLESS_RUNTIME_H_INCLUDED
