#include "stdafx.h"
#include "xr_headless.h"

#include "device.h"
#include "IGame_Level.h"
#include "XR_IOConsole.h"
#include "../xrCore/headless.h"

// ---------------------------------------------------------------------------
// Runtime glue for headless profiles (F0-04).
//
// Kept deliberately small: the profile/flags live in xrCore, the subsystem
// gates sit at their own seams. This file only owns what is specific to a
// running headless process:
//   * a frame hook that proves the loop ticks (log heartbeat, F0-07 will build
//     metrics on top of it);
//   * deferred execution of the debug launch flags (PLAN §8.11) through the
//     already existing debug surface dispatcher (coop.debug.command), so no
//     Luabind/script-layer code is touched here.
// ---------------------------------------------------------------------------

namespace Headless {
namespace {
const unsigned kMaxCommands = 12;
const unsigned kCommandMax = 192;
const unsigned kLineMax = kCommandMax + 16;

enum EDispatchState {
    eDispatchIdle = 0,
    eDispatchPending,
    eDispatchDone,
    eDispatchExpired,
};

struct SRuntime {
    char commands[kMaxCommands][kCommandMax];
    unsigned count;
    EDispatchState dispatch;
    unsigned dispatch_attempts;
    unsigned frames;
};

SRuntime& runtime() {
    static SRuntime s;
    return s;
}

void push_command(const char* lua) {
    SRuntime& rt = runtime();
    if (rt.count >= kMaxCommands) {
        Msg("! [headless] debug command queue overflow, command dropped");
        return;
    }
    xr_strcpy(rt.commands[rt.count], kCommandMax, lua);
    ++rt.count;
}

// Builds the debug-surface commands once, at startup: the Lua surface is
// exposed as coop.debug.command(name, ...) by the debug layer.
void build_debug_commands() {
    SRuntime& rt = runtime();
    rt.count = 0;
    rt.dispatch = eDispatchIdle;

    if (!Debug::AnyRequested()) {
        return;
    }
    if (!Debug::Enabled()) {
        Msg("! [headless] debug flags ignored: -coop_debug is required");
        return;
    }

    char lua[kCommandMax];
    push_command("coop.debug.enable(true)");

    if (Debug::AlifeDisabled()) {
        push_command("coop.debug.command(\"alife\",false)");
    }
    if (Debug::GodMode()) {
        push_command("coop.debug.command(\"god\",true)");
    }
    if (Debug::HasTeleport()) {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        Debug::TeleportTarget(x, y, z);
        xr_sprintf(lua, sizeof(lua), "coop.debug.command(\"teleport\",%g,%g,%g)", x, y, z);
        push_command(lua);
    }

    const unsigned spawns = Debug::SpawnCount();
    for (unsigned i = 0; i < spawns; ++i) {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        Debug::SpawnPosition(i, x, y, z);
        xr_sprintf(lua, sizeof(lua), "coop.debug.command(\"spawn\",\"%s\",%g,%g,%g)",
                   Debug::SpawnSection(i), x, y, z);
        push_command(lua);
    }

    rt.dispatch = eDispatchPending;
    rt.dispatch_attempts = 0;
    Msg("* [headless] queued %u debug command(s), waiting for the level", rt.count);
}

void dispatch_debug_commands() {
    SRuntime& rt = runtime();
    if (eDispatchPending != rt.dispatch) {
        return;
    }

    if (!Console) {
        rt.dispatch = eDispatchExpired;
        return;
    }

    if (!g_pGameLevel || !g_pGameLevel->bReady) {
        const unsigned budget = TickRateHz() * 10;
        if (++rt.dispatch_attempts > budget) {
            Msg("! [headless] debug commands not applied: level is not ready");
            rt.dispatch = eDispatchExpired;
        }
        return;
    }

    for (unsigned i = 0; i < rt.count; ++i) {
        char line[kLineMax];
        xr_sprintf(line, sizeof(line), "run_string %s", rt.commands[i]);
        Msg("* [headless] debug: %s", rt.commands[i]);
        Console->Execute(line);
    }
    rt.dispatch = eDispatchDone;
}

void log_heartbeat() {
    SRuntime& rt = runtime();
    ++rt.frames;

    const unsigned period = TickRateHz() * 5;  // one line per 5 seconds
    if (1 == rt.frames || (period && 0 == (rt.frames % period))) {
        Msg("* [headless] tick frames=%u sim_time=%.2fs profile=%s", rt.frames, Device.fTimeGlobal,
            ProfileName());
    }
}

// Frame hook of a headless process: heartbeat + deferred debug commands.
class CHeadlessTicker : public pureFrame {
public:
    virtual void _BCL OnFrame() {
        log_heartbeat();
        dispatch_debug_commands();
    }
};

CHeadlessTicker s_ticker;
bool s_registered = false;
}  // namespace

void InitializeRuntime() {
    if (!Active() || s_registered) {
        return;
    }

    const bool auto_start =
        Core.Params && (strstr(Core.Params, "-start ") || strstr(Core.Params, "-load "));
    if (!auto_start) {
        Msg("! [headless] no -start/-load in the command line: the main menu is a UI "
            "feature and is not available in a headless process");
    }

    build_debug_commands();

    // Low priority: run after the game/A-Life frame so the heartbeat reports a
    // frame that has actually been simulated.
    Device.seqFrame.Add(&s_ticker, int(REG_PRIORITY_LOW));
    s_registered = true;

    Msg("* [headless] runtime initialized (profile=%s)", ProfileName());
}

void ShutdownRuntime() {
    if (!s_registered) {
        return;
    }

    Device.seqFrame.Remove(&s_ticker);
    s_registered = false;

    Msg("* [headless] runtime stopped, frames=%u", runtime().frames);
}
}  // namespace Headless
