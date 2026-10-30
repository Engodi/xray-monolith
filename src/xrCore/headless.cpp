#include "stdafx.h"
#include "headless.h"

#include <stdlib.h>

// ---------------------------------------------------------------------------
// Runtime headless configuration (F0-04). Parsed once, queried everywhere.
// Only POD state is used: Parse() may run before Core is initialised, so no
// xr_vector / xr_string / MSG machinery is allowed here.
// ---------------------------------------------------------------------------

namespace {
const unsigned kTokenMax = 128;
const unsigned kSectionMax = Headless::Debug::kSectionMax;

// Splits a launch string into whitespace separated tokens. Values are read
// straight from the scanner, so "-dbg_spawn <section> <x> <y> <z>" sequences
// are consumed in one pass and section names keep their original case.
class CTokenScanner {
public:
    explicit CTokenScanner(const char* line) : m_cursor(line ? line : "") {}

    bool Next() {
        while (*m_cursor == ' ' || *m_cursor == '\t' || *m_cursor == '\n') {
            ++m_cursor;
        }
        if (!*m_cursor) {
            m_token[0] = 0;
            return false;
        }

        unsigned written = 0;
        while (*m_cursor && *m_cursor != ' ' && *m_cursor != '\t' && *m_cursor != '\n') {
            if (written + 1 < kTokenMax) {
                m_token[written] = *m_cursor;
                ++written;
            }
            ++m_cursor;
        }
        m_token[written] = 0;
        return true;
    }

    const char* token() const { return m_token; }

    bool NextFloat(float& value) {
        if (!Next()) {
            return false;
        }
        value = float(atof(m_token));
        return true;
    }

    bool NextUnsigned(unsigned& value) {
        if (!Next()) {
            return false;
        }
        const long parsed = atol(m_token);
        if (parsed <= 0) {
            return false;
        }
        value = unsigned(parsed);
        return true;
    }

private:
    const char* m_cursor;
    char m_token[kTokenMax];
};

struct SSpawnRequest {
    char section[kSectionMax];
    float x;
    float y;
    float z;
};

struct SState {
    // Defaults describe "no launch profile": they are also what an unparsed
    // module reports before its bootstrap ran (see resolved_state()).
    bool parsed = false;

    Headless::EProfile profile = Headless::eProfileNone;
    unsigned tick_hz = 60;
    unsigned backbuffer_width = 640;
    unsigned backbuffer_height = 480;

    bool debug_allowed = false;    // -coop_debug
    bool debug_requested = false;  // at least one -dbg_* token / env value
    bool alife_disabled = false;
    bool god_mode = false;
    bool has_teleport = false;
    float teleport[3] = {0.0f, 0.0f, 0.0f};
    unsigned spawn_count = 0;
    SSpawnRequest spawns[Headless::Debug::kMaxSpawns] = {};
};

SState& state() {
    static SState s;
    return s;
}

// xrCore is a *static* library: the executable and every DLL that links it
// (notably the DX11 renderer) get their own copy of this translation unit and
// therefore their own `static SState`. A module that never called
// Headless::Parse() used to report "not headless" and silently run the GPU
// path - that is why the renderer's headless guards did not fire. The command
// line is process-wide, so an unparsed module bootstraps itself from it.
const char* process_command_line() {
#if defined(_WIN32)
    return ::GetCommandLineA();
#else
    // Host-side parser checks (tests/cpp/headless) always pass an explicit
    // launch line to Parse(); there is no Windows command line to fall back to.
    return nullptr;
#endif
}

// Resolves the per-module configuration before the first query. The one-shot is
// a function-local static (thread-safe magic static): the renderer DLL may ask
// its first question from the render thread, and the bootstrap must not run
// twice.
void ensure_parsed() {
    struct SBootstrap {
        SBootstrap() {
            if (state().parsed) {
                return;
            }
            const char* line = process_command_line();
            if (line && line[0]) {
                Headless::Parse(line);
            }
        }
    };
    static SBootstrap bootstrap;
    (void)bootstrap;
}

const SState& resolved_state() {
    ensure_parsed();
    return state();
}

bool same_token(const char* left, const char* right) {
    return left && right && 0 == _stricmp(left, right);
}

bool same_flag(const char* value, const char* const* names, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        if (same_token(value, names[i])) {
            return true;
        }
    }
    return false;
}

bool env_value(const char* name, char* out, size_t out_size) {
    const char* value = getenv(name);
    if (!value || !value[0]) {
        return false;
    }
    xr_strcpy(out, out_size, value);
    return true;
}

bool env_flag(const char* name, bool& out) {
    char buffer[32];
    if (!env_value(name, buffer, sizeof(buffer))) {
        return false;
    }
    static const char* const truthy[] = {"1", "true", "on", "yes"};
    static const char* const falsy[] = {"0", "false", "off", "no"};
    if (same_flag(buffer, truthy, unsigned(sizeof(truthy) / sizeof(truthy[0])))) {
        out = true;
        return true;
    }
    if (same_flag(buffer, falsy, unsigned(sizeof(falsy) / sizeof(falsy[0])))) {
        out = false;
        return true;
    }
    return false;
}

bool env_floats(const char* name, float* out, unsigned count) {
    char buffer[kTokenMax];
    if (!env_value(name, buffer, sizeof(buffer))) {
        return false;
    }

    CTokenScanner scan(buffer);
    for (unsigned i = 0; i < count; ++i) {
        if (!scan.NextFloat(out[i])) {
            return false;
        }
    }
    return true;
}

bool env_unsigned(const char* name, unsigned& out) {
    char buffer[32];
    if (!env_value(name, buffer, sizeof(buffer))) {
        return false;
    }
    CTokenScanner scan(buffer);
    return scan.NextUnsigned(out);
}

// "section x y z": one string value followed by three floats.
bool read_spawn(CTokenScanner& scan, SSpawnRequest& spawn) {
    if (!scan.Next()) {
        return false;
    }
    xr_strcpy(spawn.section, kSectionMax, scan.token());
    return scan.NextFloat(spawn.x) && scan.NextFloat(spawn.y) && scan.NextFloat(spawn.z);
}

void push_spawn(const SSpawnRequest& spawn) {
    SState& s = state();
    if (s.spawn_count >= Headless::Debug::kMaxSpawns) {
        return;  // silently capped: the config is logged by LogConfig()
    }
    s.spawns[s.spawn_count] = spawn;
    ++s.spawn_count;
}

void parse_debug_environment() {
    SState& s = state();

    bool enabled = false;
    if (env_flag("XRAY_DBG_ALIFE", enabled)) {
        s.alife_disabled = !enabled;
        s.debug_requested = true;
    }
    if (env_flag("XRAY_DBG_GOD", enabled)) {
        s.god_mode = enabled;
        s.debug_requested = true;
    }

    float position[3];
    if (env_floats("XRAY_DBG_TELEPORT", position, 3)) {
        s.has_teleport = true;
        s.teleport[0] = position[0];
        s.teleport[1] = position[1];
        s.teleport[2] = position[2];
        s.debug_requested = true;
    }

    char buffer[kTokenMax];
    if (env_value("XRAY_DBG_SPAWN", buffer, sizeof(buffer))) {
        CTokenScanner scan(buffer);
        SSpawnRequest spawn = {};
        if (read_spawn(scan, spawn)) {
            push_spawn(spawn);
            s.debug_requested = true;
        }
    }
}

void clamp_tick_rate(unsigned& tick_hz) {
    if (tick_hz < 1) {
        tick_hz = 1;
    }
    if (tick_hz > 500) {
        tick_hz = 500;
    }
}
}  // namespace

namespace Headless {
void Parse(const char* command_line) {
    SState& s = state();
    if (s.parsed) {
        return;
    }
    s.parsed = true;
    s.tick_hz = 60;
    s.backbuffer_width = 640;
    s.backbuffer_height = 480;

    bool server = false;
    bool client = false;

    for (CTokenScanner scan(command_line); scan.Next();) {
        const char* token = scan.token();

        if (same_token(token, "-headless_server")) {
            // last profile token wins, so a wrapper can append an override
            server = true;
            client = false;
        } else if (same_token(token, "-headless_client")) {
            client = true;
            server = false;
        } else if (same_token(token, "-headless")) {
            if (!client) {
                server = true;
            }
        } else if (same_token(token, "-headless_tick")) {
            unsigned hz = 0;
            if (scan.NextUnsigned(hz)) {
                clamp_tick_rate(hz);
                s.tick_hz = hz;
            }
        } else if (same_token(token, "-coop_debug")) {
            s.debug_allowed = true;
        } else if (same_token(token, "-dbg_alife_off")) {
            s.alife_disabled = true;
            s.debug_requested = true;
        } else if (same_token(token, "-dbg_alife_on")) {
            s.alife_disabled = false;
            s.debug_requested = true;
        } else if (same_token(token, "-dbg_god")) {
            s.god_mode = true;
            s.debug_requested = true;
        } else if (same_token(token, "-dbg_teleport")) {
            float position[3];
            if (scan.NextFloat(position[0]) && scan.NextFloat(position[1]) &&
                scan.NextFloat(position[2])) {
                s.has_teleport = true;
                s.teleport[0] = position[0];
                s.teleport[1] = position[1];
                s.teleport[2] = position[2];
                s.debug_requested = true;
            }
        } else if (same_token(token, "-dbg_spawn")) {
            SSpawnRequest spawn = {};
            if (read_spawn(scan, spawn)) {
                push_spawn(spawn);
                s.debug_requested = true;
            }
        }
    }

    // Environment: the harness passes profiles/flags through the environment,
    // explicit values win over the command line.
    char profile[32];
    if (env_value("XRAY_HEADLESS", profile, sizeof(profile))) {
        static const char* const off_values[] = {"0", "false", "off", "no", "none"};
        static const char* const server_values[] = {"1", "true", "on", "yes", "server"};
        if (same_flag(profile, off_values, unsigned(sizeof(off_values) / sizeof(off_values[0])))) {
            server = false;
            client = false;
        } else if (same_flag(profile, server_values,
                             unsigned(sizeof(server_values) / sizeof(server_values[0])))) {
            server = true;
            client = false;
        } else if (same_token(profile, "client")) {
            client = true;
            server = false;
        }
    }

    unsigned tick_hz = 0;
    if (env_unsigned("XRAY_HEADLESS_TICK_HZ", tick_hz)) {
        clamp_tick_rate(tick_hz);
        s.tick_hz = tick_hz;
    }

    unsigned width = 0;
    if (env_unsigned("XRAY_HEADLESS_WIDTH", width) && width >= 16 && width <= 16384) {
        s.backbuffer_width = width;
    }
    unsigned height = 0;
    if (env_unsigned("XRAY_HEADLESS_HEIGHT", height) && height >= 16 && height <= 16384) {
        s.backbuffer_height = height;
    }

    if (client) {
        s.profile = eProfileClient;
    } else if (server) {
        s.profile = eProfileServer;
    } else {
        s.profile = eProfileNone;
    }

    parse_debug_environment();
}

bool Parsed() { return resolved_state().parsed; }

EProfile Profile() { return resolved_state().profile; }

bool Active() { return eProfileNone != resolved_state().profile; }

bool IsServer() { return eProfileServer == resolved_state().profile; }

bool IsClient() { return eProfileClient == resolved_state().profile; }

const char* ProfileName() {
    switch (resolved_state().profile) {
    case eProfileServer:
        return "server";
    case eProfileClient:
        return "client";
    default:
        return "none";
    }
}

bool RenderEnabled() { return !Active(); }

bool SoundEnabled() { return !Active(); }

bool UiEnabled() { return !Active(); }

bool LocalActorEnabled() { return !Active(); }

bool SimulationEnabled() { return Active(); }

unsigned TickRateHz() { return resolved_state().tick_hz; }

void ConfigureBackbuffer(unsigned& width, unsigned& height) {
    if (!Active()) {
        return;
    }
    width = resolved_state().backbuffer_width;
    height = resolved_state().backbuffer_height;
}

void SleepToNextTick(unsigned elapsed_ms) {
    if (!Active()) {
        return;
    }

    unsigned tick_hz = TickRateHz();
    clamp_tick_rate(tick_hz);
    const unsigned budget_ms = 1000 / tick_hz;
    if (budget_ms > elapsed_ms) {
        ::Sleep(budget_ms - elapsed_ms);
    }
}

void LogConfig() {
    if (!Active()) {
        return;
    }

    Msg("* [headless] profile=%s tick=%uHz backbuffer=%ux%u", ProfileName(), TickRateHz(),
        resolved_state().backbuffer_width, resolved_state().backbuffer_height);
    Msg("* [headless] render=%s sound=%s ui=%s local_actor=%s ai=%s", RenderEnabled() ? "on" : "off",
        SoundEnabled() ? "on" : "off", UiEnabled() ? "on" : "off",
        LocalActorEnabled() ? "on" : "off", SimulationEnabled() ? "on" : "off");

    if (!Debug::AnyRequested()) {
        return;
    }
    if (!Debug::Enabled()) {
        Msg("! [headless] debug launch flags ignored: pass -coop_debug to arm the debug surface");
        return;
    }

    Msg("* [headless] debug flags: alife=%s god=%s teleport=%s spawns=%u",
        Debug::AlifeDisabled() ? "off" : "on", Debug::GodMode() ? "on" : "off",
        Debug::HasTeleport() ? "on" : "off", Debug::SpawnCount());
}

namespace Debug {
bool Enabled() { return resolved_state().debug_allowed; }

bool AnyRequested() { return resolved_state().debug_requested; }

bool AlifeDisabled() {
    const SState& s = resolved_state();
    return s.debug_allowed && s.alife_disabled;
}

bool GodMode() {
    const SState& s = resolved_state();
    return s.debug_allowed && s.god_mode;
}

bool HasTeleport() {
    const SState& s = resolved_state();
    return s.debug_allowed && s.has_teleport;
}

void TeleportTarget(float& x, float& y, float& z) {
    const SState& s = resolved_state();
    x = s.teleport[0];
    y = s.teleport[1];
    z = s.teleport[2];
}

unsigned SpawnCount() {
    const SState& s = resolved_state();
    return s.debug_allowed ? s.spawn_count : 0;
}

const char* SpawnSection(unsigned index) {
    const SState& s = resolved_state();
    if (!s.debug_allowed || index >= s.spawn_count) {
        return "";
    }
    return s.spawns[index].section;
}

void SpawnPosition(unsigned index, float& x, float& y, float& z) {
    const SState& s = resolved_state();
    if (index >= s.spawn_count) {
        x = y = z = 0.0f;
        return;
    }
    x = s.spawns[index].x;
    y = s.spawns[index].y;
    z = s.spawns[index].z;
}
}  // namespace Debug
}  // namespace Headless
