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

HEADLESS = "Headless::Active()"


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


CHECKS = (
    check_engine_api,
    check_device_create,
    check_renderer_guards,
    check_memory_stats,
    check_ai_space,
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
