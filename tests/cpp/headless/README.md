# Host-side check: headless launch flags (F0-04)

The engine is built only on Windows (MSVC/VS2022, F0-01). The **headless launch
configuration** (`src/xrCore/headless.cpp`) is platform independent, so it is
verified here on any host with a C++17 compiler — the parser is compiled against
a tiny `stdafx.h` stub (`stub/`) instead of the Windows engine headers.

```bash
tests/cpp/headless/run.sh
```

## What is covered

- profile selection: `-headless`, `-headless_server`, `-headless_client`,
  "last profile token wins", `XRAY_HEADLESS=server|client|0`;
- fixed loop rate: `-headless_tick <hz>`, `XRAY_HEADLESS_TICK_HZ`, invalid and
  out-of-range values (`0` rejected, `>500` clamped);
- subsystem gates: render/sound/UI/local-actor are closed in every headless
  profile and open in the normal path;
- debug launch flags (PLAN §8.11) staying off by default: `-dbg_god`,
  `-dbg_alife_off/on`, `-dbg_teleport`, repeated `-dbg_spawn` (max 8, section
  case preserved) and their `XRAY_DBG_*` mirrors;
- the production-safe behaviour: without `-coop_debug` every debug getter
  returns the default value even when the flags were passed.

## What is *not* covered

- Anything that needs Windows/D3D: device creation, the headless tick loop,
  render/sound/UI gating in the engine, the deferred debug-command dispatch.
  Those are compiled and executed by the engine CI (F0-01) and the Wine/Proton
  harness (F0-11).
