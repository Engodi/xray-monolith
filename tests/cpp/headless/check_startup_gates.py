#!/usr/bin/env python3
"""Host-side gate: headless startup never initialises the renderer (F0-04).

The engine is built only on Windows (MSVC/VS2022, F0-01), so the startup order
of `xrEngine`/`xrRender`/`xrGame` cannot be executed on a Linux host. What *can*
be checked here is the structural invariant that makes the headless profile
render-free, because a regression in any of these places silently re-enables
D3D/shader compilation under Wine:

1. `CEngineAPI` must not announce a renderer DLL in a headless process. The DX11
   renderer is statically linked (`STATIC_RENDERER_R4`), so the old
   `Loading DLL: xrRender_R4.dll` line merely *looked* like a dynamic load; the
   attach itself is still required (it wires `::Render`/`RenderFactory` for the
   CPU-only render core), so the check is about the emitted log, not the call.
2. `CRenderDevice::Create` must take the `Headless::Active()` branch (which calls
   `CreateHeadless`) before it logs `Starting RENDER device...`, and the
   headless branch must never touch `HW.CreateDevice`.
3. The renderer's own entry points (`dxRenderDeviceRender::Create`/`OnDeviceCreate`,
   `CRender::create`, `CRender::level_Load`) must keep their headless early
   returns *before* the first GPU/shader/swap-chain call.
4. `full_memory_stats` (executed by `Console->Execute("stat_memory")` during
   startup, before the level is loaded) must not call `lua_gc` on a script
   engine that a headless/dedicated process never created.
5. Headless is *not* a dedicated server (F0-05): the AI space (game/level graph,
   cover manager, patrol paths) and the Lua script engine are simulation, not
   presentation, so every site that decides "does this process own an AI space /
   script engine?" must use `ai_simulation_enabled()` and never
   `g_dedicated_server` directly. In the same files the presentation-only
   managers (audio, debug drawing, statistics) must keep the
   `g_dedicated_server` gate, otherwise a headless process would touch a device
   it does not own.
6. The coop Lua bridge must go through `ai().get_script_engine()` and bail out
   when the process has no script engine at all (dedicated server), instead of
   dereferencing the AI space.

Usage: tests/cpp/headless/check_startup_gates.py [--root <engine-root>]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ENGINE_API = Path("src/xrEngine/EngineAPI.cpp")
DEVICE_CREATE = Path("src/xrEngine/Device_create.cpp")
RENDER_DEVICE = Path("src/Layers/xrRender/dxRenderDeviceRender.cpp")
RENDER_R4 = Path("src/Layers/xrRenderPC_R4/r4.cpp")
RENDER_R4_LOADER = Path("src/Layers/xrRenderPC_R4/r4_loader.cpp")
CONSOLE_COMMANDS = Path("src/xrGame/console_commands.cpp")
AI_SPACE = Path("src/xrGame/ai_space.h")
AI_SPACE_INLINE = Path("src/xrGame/ai_space_inline.h")

# F0-05: simulation (AI space + script engine) versus presentation split.
AI_SIMULATION = Path("src/xrEngine/ai_simulation.h")
AI_SPACE_IMPL = Path("src/xrGame/ai_space.cpp")
LEVEL_LOAD = Path("src/xrGame/Level_load.cpp")
LEVEL = Path("src/xrGame/Level.cpp")
LEVEL_NETWORK = Path("src/xrGame/Level_network.cpp")
GAME_OBJECT = Path("src/xrGame/GameObject.cpp")
ACTOR = Path("src/xrGame/Actor.cpp")
HIT_MEMORY = Path("src/xrGame/hit_memory_manager.cpp")
VISUAL_MEMORY = Path("src/xrGame/visual_memory_manager.cpp")
SOUND_MEMORY = Path("src/xrGame/sound_memory_manager.cpp")
LEVEL_NETWORK_SPAWN = Path("src/xrGame/Level_network_spawn.cpp")
OBJECT_FACTORY_REGISTER = Path("src/xrServerEntities/object_factory_register.cpp")
OBJECT_FACTORY_SCRIPT = Path("src/xrServerEntities/object_factory_script.cpp")
COOP_BRIDGE = Path("src/xrGame/coop/coop_bridge_script.cpp")
COOP_RUNTIME = Path("src/xrGame/coop/coop_core_runtime.cpp")
HEADLESS_HEADER = Path("src/xrCore/headless.h")
HEADLESS_SOURCE = Path("src/xrCore/headless.cpp")

HEADLESS = "Headless::Active()"
AI_PREDICATE = "ai_simulation_enabled()"
DEDICATED = "g_dedicated_server"


def read(path: Path) -> str:
    if not path.is_file():
        raise SystemExit(f"missing {path}")
    return path.read_text(encoding="utf-8", errors="replace")


def strip_comments(text: str) -> str:
    """Drop // and /* */ comments but keep string/char literals intact."""
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            i += 2
            while i < n and text[i] != "\n":
                i += 1
        elif ch == "/" and nxt == "*":
            i += 2
            while i < n - 1 and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
        elif ch in "\"'":
            quote = ch
            out.append(ch)
            i += 1
            while i < n:
                if text[i] == "\\":
                    out.append(text[i])
                    i += 1
                    if i < n:
                        out.append(text[i])
                        i += 1
                    continue
                out.append(text[i])
                if text[i] == quote:
                    i += 1
                    break
                i += 1
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def body_of(text: str, signature: str) -> str | None:
    """Body of the first definition whose signature contains `signature`."""
    pattern = re.compile(r"[^\n;{}]*" + re.escape(signature) + r"\s*\([^;{}]*?\)\s*(?:const\s*)?\{")
    match = pattern.search(text)
    if not match:
        return None
    start = text.index("{", match.start())
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:index]
    return None


def fail(errors: list[str], path: Path, message: str) -> None:
    errors.append(f"{path}: {message}")


def require_body(text: str, path: Path, signature: str, errors: list[str]) -> str | None:
    body = body_of(text, signature)
    if body is None:
        fail(errors, path, f"definition of '{signature}' not found")
    return body


def check_engine_api(root: Path, errors: list[str]) -> None:
    text = strip_comments(read(root / ENGINE_API))

    helper = body_of(text, "log_renderer_attach")
    if helper is None:
        fail(
            errors,
            ENGINE_API,
            "renderer attach has no headless-aware log helper "
            "('log_renderer_attach'); a headless process must not print "
            "'Loading DLL: xrRender_R*.dll'",
        )
    else:
        if HEADLESS not in helper:
            fail(errors, ENGINE_API, "log_renderer_attach has no Headless::Active() branch")
        if re.search(r'Log\("Loading DLL:",\s*renderer_name\)', helper) is None:
            fail(errors, ENGINE_API, "log_renderer_attach must Log(\"Loading DLL:\") on the render path")

    if re.search(r'Log\("Loading DLL:",\s*r[1-4]_name\)', text):
        fail(
            errors,
            ENGINE_API,
            "renderer DLL is announced unconditionally; use log_renderer_attach() "
            "so the headless profile stays silent",
        )

    attach_sites = len(re.findall(r"DllMainXrRenderR[1-4]\s*\(\s*NULL\s*,\s*DLL_PROCESS_ATTACH", text))
    if attach_sites == 0:
        fail(errors, ENGINE_API, "no renderer attach call found (expected DllMainXrRenderR4)")

    init = require_body(text, ENGINE_API, "CEngineAPI::CreateRendererList", errors)
    if init is not None:
        headless = init.find(HEADLESS)
        probe = init.find('Log("Loading DLL:"')
        if headless < 0:
            fail(errors, ENGINE_API, "CreateRendererList has no Headless::Active() early return")
        elif probe >= 0 and probe < headless:
            fail(errors, ENGINE_API, "CreateRendererList probes renderers before the headless gate")
        if HEADLESS in init and "return" not in init[headless:]:
            fail(errors, ENGINE_API, "headless branch of CreateRendererList must return")


def check_device_create(root: Path, errors: list[str]) -> None:
    text = strip_comments(read(root / DEVICE_CREATE))

    create = require_body(text, DEVICE_CREATE, "PROTECT_API void CRenderDevice::Create", errors)
    if create is not None:
        headless = create.find(HEADLESS)
        render_log = create.find("Starting RENDER device...")
        headless_call = create.find("CreateHeadless()")
        if headless < 0:
            fail(errors, DEVICE_CREATE, "CRenderDevice::Create has no headless gate")
        if headless_call < 0:
            fail(errors, DEVICE_CREATE, "CRenderDevice::Create never calls CreateHeadless()")
        if headless >= 0 and render_log >= 0 and render_log < headless:
            fail(errors, DEVICE_CREATE, "'Starting RENDER device...' is logged before the headless gate")
        if headless_call >= 0 and render_log >= 0 and render_log < headless_call:
            fail(errors, DEVICE_CREATE, "render device creation precedes CreateHeadless()")

    headless = require_body(text, DEVICE_CREATE, "CRenderDevice::CreateHeadless", errors)
    if headless is not None:
        if HEADLESS in headless:
            fail(errors, DEVICE_CREATE, "CreateHeadless must not recurse into Headless::Active()")
        for forbidden in ("HW.CreateDevice", "Starting RENDER device"):
            if forbidden in headless:
                fail(errors, DEVICE_CREATE, f"CreateHeadless contains '{forbidden}'")


def check_renderer_guards(root: Path, errors: list[str]) -> None:
    device = strip_comments(read(root / RENDER_DEVICE))

    create = require_body(device, RENDER_DEVICE, "dxRenderDeviceRender::Create", errors)
    if create is not None:
        gate = create.find(HEADLESS)
        gpu = create.find("HW.CreateDevice")
        if gate < 0:
            fail(errors, RENDER_DEVICE, "dxRenderDeviceRender::Create has no headless gate")
        elif gpu >= 0 and gate > gpu:
            fail(errors, RENDER_DEVICE, "dxRenderDeviceRender::Create calls HW.CreateDevice before the headless gate")

    on_create = require_body(device, RENDER_DEVICE, "dxRenderDeviceRender::OnDeviceCreate", errors)
    if on_create is not None:
        gate = on_create.find(HEADLESS)
        if gate < 0:
            fail(errors, RENDER_DEVICE, "dxRenderDeviceRender::OnDeviceCreate has no headless gate")
        else:
            for forbidden in ("RCache.OnDeviceCreate", "Resources->OnDeviceCreate", "WireShader.create"):
                index = on_create.find(forbidden)
                if index >= 0 and index < gate:
                    fail(errors, RENDER_DEVICE, f"'{forbidden}' runs before the headless gate")

    r4 = strip_comments(read(root / RENDER_R4))
    r4_create = require_body(r4, RENDER_R4, "CRender::create", errors)
    if r4_create is not None:
        gate = r4_create.find(HEADLESS)
        if gate < 0:
            fail(errors, RENDER_R4, "CRender::create has no headless gate")
        else:
            seq = r4_create.find("Device.seqFrame.Add")
            if 0 <= seq < gate:
                fail(errors, RENDER_R4, "CRender::create registers a frame callback in headless")

    loader = strip_comments(read(root / RENDER_R4_LOADER))
    level_load = require_body(loader, RENDER_R4_LOADER, "CRender::level_Load", errors)
    if level_load is not None:
        gate = level_load.find(HEADLESS)
        if gate < 0:
            fail(errors, RENDER_R4_LOADER, "CRender::level_Load has no headless gate")
        else:
            load_begin = level_load.find("LoadBegin")
            shaders = level_load.find("fsL_SHADERS")
            for name, index in (("LoadBegin", load_begin), ("fsL_SHADERS", shaders)):
                if 0 <= index < gate:
                    fail(errors, RENDER_R4_LOADER, f"level_Load touches {name} before the headless gate")


def check_memory_stats(root: Path, errors: list[str]) -> None:
    text = strip_comments(read(root / CONSOLE_COMMANDS))

    stats = require_body(text, CONSOLE_COMMANDS, "full_memory_stats", errors)
    if stats is None:
        return

    guarded = re.compile(r"get_script_engine\s*\(\s*\)")
    for match in re.finditer(r"lua_gc\s*\(", stats):
        window = stats[max(0, match.start() - 400):match.start()]
        if not guarded.search(window):
            fail(
                errors,
                CONSOLE_COMMANDS,
                "full_memory_stats calls lua_gc() without a get_script_engine() guard; "
                "a headless process has no Lua state there (startup page fault)",
            )
    if "ai().script_engine().lua()" in stats:
        fail(
            errors,
            CONSOLE_COMMANDS,
            "full_memory_stats dereferences script_engine() directly; headless has none",
        )


def check_ai_space(root: Path, errors: list[str]) -> None:
    header = strip_comments(read(root / AI_SPACE))
    if not re.search(r"get_script_engine\s*\(\s*\)\s*const", header):
        fail(errors, AI_SPACE, "CAI_Space::get_script_engine() accessor is missing")

    inline = strip_comments(read(root / AI_SPACE_INLINE))
    body = body_of(inline, "CAI_Space::get_script_engine")
    if body is None:
        fail(errors, AI_SPACE_INLINE, "CAI_Space::get_script_engine() implementation is missing")
    elif "m_script_engine" not in body:
        fail(errors, AI_SPACE_INLINE, "get_script_engine() must return m_script_engine")


# ---------------------------------------------------------------------------
# F0-05: headless keeps the AI space / script engine, dedicated does not.
# ---------------------------------------------------------------------------

# Whole bodies that must decide on ai_simulation_enabled() and never look at
# g_dedicated_server: that is the AI/script core of a level.
AI_SIMULATION_BODIES = (
    (AI_SPACE_IMPL, "CAI_Space::init"),
    (AI_SPACE_IMPL, "CAI_Space::unload"),
    (AI_SPACE_IMPL, "CAI_Space::patrol_path_storage_raw"),
    (AI_SPACE_IMPL, "CAI_Space::patrol_path_storage"),
)

# (file, marker): the closest preceding gate of *every* occurrence of `marker`
# must be ai_simulation_enabled(), because a headless process owns all of it.
AI_MARKERS = (
    (LEVEL_LOAD, "patrol_path_storage_raw"),
    (LEVEL_LOAD, "add_script_process"),
    (LEVEL, "xr_new<CMapManager>"),
    (LEVEL, "xr_new<CGameTaskManager>"),
    (LEVEL, "xr_new<CSpaceRestrictionManager>"),
    (LEVEL, "xr_new<CClientSpawnManager>"),
    (LEVEL, "remove_script_process"),
    (LEVEL, "MapManager().Update()"),
    (LEVEL, "eScriptProcessorLevel)->update()"),
    (LEVEL, "script_gc()"),
    (LEVEL_NETWORK, "space_restriction_manager().clear()"),
    (LEVEL_NETWORK, "client_spawn_manager().clear()"),
    (LEVEL_NETWORK, "collect_all_garbage()"),
    (GAME_OBJECT, "ai_location().reinit()"),
    (GAME_OBJECT, "CScriptBinder::reload"),
    (GAME_OBJECT, "CScriptBinder::reinit"),
    (GAME_OBJECT, "CScriptBinder::shedule_Update"),
    (ACTOR, "memory().reinit()"),
    (ACTOR, "memory().reload("),
    # Per-object delayed-spawn callbacks: AI perception resolves spawns through
    # the client spawn manager, which a headless process now owns.
    (HIT_MEMORY, "client_spawn_manager().add("),
    (VISUAL_MEMORY, "client_spawn_manager().add("),
    (SOUND_MEMORY, "client_spawn_manager().add("),
    (LEVEL_NETWORK_SPAWN, "client_spawn_manager().clear(O->ID())"),
    (LEVEL_NETWORK_SPAWN, "client_spawn_manager().callback(O)"),
)

# (file, marker): a ternary that allocates AI state, so the predicate sits on
# the same line as the marker instead of in a preceding `if`.
AI_INLINE_MARKERS = (
    (GAME_OBJECT, "m_ai_location ="),
    (ACTOR, "m_memory ="),
)

# Presentation-only state must stay behind g_dedicated_server: a headless
# process owns no audio device and no font/shader state.
PRESENTATION_MARKERS = (
    (LEVEL_LOAD, "m_level_sound_manager->Load"),
    (LEVEL, "xr_new<CLevelSoundManager>"),
    (LEVEL, "CLevelSoundManager::Update"),
    (LEVEL, "xr_new<CDebugRenderer>"),
    (LEVEL, "psDeviceFlags.test(rsStatistic)"),
)

# Lua bridge files: the AI space may have no script engine at all.
COOP_SCRIPT_ENGINE_FILES = (COOP_BRIDGE, COOP_RUNTIME)


def nearest_gate(text: str, marker: str, start: int) -> tuple[str | None, int]:
    """Which simulation gate guards `marker`, and where the marker starts."""
    index = text.find(marker, start)
    if index < 0:
        return None, -1
    prefix = text[:index]
    simulation = prefix.rfind(AI_PREDICATE)
    dedicated = prefix.rfind(DEDICATED)
    if simulation < 0 and dedicated < 0:
        return "none", index
    return ("simulation" if simulation > dedicated else "dedicated"), index


def check_ai_simulation_split(root: Path, errors: list[str]) -> None:
    for path, signature in AI_SIMULATION_BODIES:
        text = strip_comments(read(root / path))
        body = require_body(text, path, signature, errors)
        if body is None:
            continue
        if AI_PREDICATE not in body:
            fail(
                errors,
                path,
                f"{signature} still decides on g_dedicated_server alone; "
                "headless must build the AI space and the script engine",
            )
        if DEDICATED in body:
            fail(
                errors,
                path,
                f"{signature} tests g_dedicated_server directly; use {AI_PREDICATE}",
            )

    for path, marker in AI_MARKERS + PRESENTATION_MARKERS:
        text = strip_comments(read(root / path))
        wanted = "simulation" if (path, marker) in AI_MARKERS else "dedicated"
        expected_flag = AI_PREDICATE if wanted == "simulation" else DEDICATED
        gate, index = nearest_gate(text, marker, 0)
        if gate is None:
            fail(errors, path, f"'{marker}' not found")
            continue
        while index >= 0:
            if gate != wanted:
                fail(
                    errors,
                    path,
                    f"'{marker}' is guarded by the wrong flag (found: {gate}); "
                    f"expected {expected_flag}",
                )
            gate, index = nearest_gate(text, marker, index + len(marker))

    for path, marker in AI_INLINE_MARKERS:
        text = strip_comments(read(root / path))
        found = False
        for line in text.splitlines():
            if marker not in line:
                continue
            found = True
            if AI_PREDICATE not in line:
                fail(
                    errors,
                    path,
                    f"'{marker}' line does not use {AI_PREDICATE}: {line.strip()}",
                )
            if DEDICATED in line:
                fail(errors, path, f"'{marker}' line still tests g_dedicated_server")
        if not found:
            fail(errors, path, f"'{marker}' not found")


def check_ai_simulation_header(root: Path, errors: list[str]) -> None:
    text = strip_comments(read(root / AI_SIMULATION))
    if not re.search(r"inline\s+bool\s+ai_simulation_enabled\s*\(\s*\)", text):
        fail(errors, AI_SIMULATION, "inline ai_simulation_enabled() helper is missing")
    if DEDICATED not in text:
        fail(errors, AI_SIMULATION, "helper never reads g_dedicated_server")
    if "Headless::" not in text:
        fail(errors, AI_SIMULATION, "helper ignores the headless profile")

    header = strip_comments(read(root / HEADLESS_HEADER))
    if not re.search(r"\bbool\s+SimulationEnabled\s*\(\s*\)", header):
        fail(errors, HEADLESS_HEADER, "Headless::SimulationEnabled() is not declared")

    source = strip_comments(read(root / HEADLESS_SOURCE))
    body = body_of(source, "SimulationEnabled")
    if body is None:
        fail(errors, HEADLESS_SOURCE, "Headless::SimulationEnabled() is not implemented")
    elif "Active()" not in body:
        fail(errors, HEADLESS_SOURCE, "SimulationEnabled() must follow the headless profile")

    config = body_of(source, "LogConfig")
    if config is None:
        fail(errors, HEADLESS_SOURCE, "Headless::LogConfig() is missing")
    elif "ai=" not in config:
        fail(
            errors,
            HEADLESS_SOURCE,
            "LogConfig() does not report the AI/script state ('ai=on'); "
            "the harness cannot verify the headless simulation from the log",
        )


def check_object_factory(root: Path, errors: list[str]) -> None:
    """A headless process registers script classes from Lua, not from the
    dedicated-only C++ stubs: both paths would register the same clsid."""
    text = strip_comments(read(root / OBJECT_FACTORY_REGISTER))
    skip_when_scripts = re.compile(r"if\s*\(\s*" + re.escape(AI_PREDICATE) + r"\s*\)\s*return\s*;")
    if skip_when_scripts.search(text) is None:
        fail(
            errors,
            OBJECT_FACTORY_REGISTER,
            "the dedicated-only class stubs are not skipped when the process runs "
            f"scripts; guard them with 'if ({AI_PREDICATE}) return;'",
        )
    if re.search(r"if\s*\(\s*!\s*" + re.escape(DEDICATED) + r"\s*\)\s*return\s*;", text):
        fail(
            errors,
            OBJECT_FACTORY_REGISTER,
            "dedicated-only stubs are skipped on g_dedicated_server, so a headless "
            "process registers every script clsid twice",
        )

    script = strip_comments(read(root / OBJECT_FACTORY_SCRIPT))
    body = body_of(script, "CObjectFactory::register_script_classes")
    if body is None:
        fail(errors, OBJECT_FACTORY_SCRIPT, "CObjectFactory::register_script_classes is missing")
    elif AI_PREDICATE not in body or DEDICATED in body:
        fail(
            errors,
            OBJECT_FACTORY_SCRIPT,
            f"register_script_classes() must build the AI space via {AI_PREDICATE}",
        )


def check_coop_script_engine(root: Path, errors: list[str]) -> None:
    for path in COOP_SCRIPT_ENGINE_FILES:
        text = strip_comments(read(root / path))
        if "ai().script_engine()" in text:
            fail(
                errors,
                path,
                "dereferences ai().script_engine() directly; use "
                "ai().get_script_engine() and bail out when it is null "
                "(dedicated server has no script engine)",
            )
        if "get_script_engine()" not in text:
            fail(errors, path, "no ai().get_script_engine() guard found")


CHECKS = (
    check_engine_api,
    check_device_create,
    check_renderer_guards,
    check_memory_stats,
    check_ai_space,
    check_ai_simulation_split,
    check_ai_simulation_header,
    check_object_factory,
    check_coop_script_engine,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="engine repository root")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    errors: list[str] = []
    for check in CHECKS:
        check(root, errors)

    if errors:
        for error in errors:
            print(f"FAIL  {error}", file=sys.stderr)
        print(f"headless startup gates: FAILED ({len(errors)})", file=sys.stderr)
        return 1
    print("headless startup gates: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
