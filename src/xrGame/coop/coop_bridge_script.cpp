////////////////////////////////////////////////////////////////////////////
//	Module 		: coop/coop_bridge_script.cpp
//	Created 	: 23.10.2026
//	Description : Lua export of the coop bridge (task F0-05)
//
//	Exports the `coop_bridge` table. The Lua gameplay layer (gamedata/scripts/
//	coop*.script) is the only consumer; the coop core talks to Lua through
//	this surface and never touches engine internals.
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "coop/coop_bridge_script.h"
#include "coop/coop_bridge.h"
#include "ai_space.h"
#include "script_engine.h"

using namespace luabind;

namespace {
coop::ERole parse_role(LPCSTR role) {
    if (0 == xr_strcmp(role, "host"))
        return coop::ERole::host;
    if (0 == xr_strcmp(role, "client"))
        return coop::ERole::client;
    return coop::ERole::offline;
}

bool is_known_role(LPCSTR role) {
    return 0 == xr_strcmp(role, "offline") || 0 == xr_strcmp(role, "host") ||
           0 == xr_strcmp(role, "client");
}

::luabind::object lua_session(lua_State* L) {
    coop::CBridge& b = coop::bridge();

    ::luabind::object session = ::luabind::newtable(L);
    session["role"] = b.role_name();
    session["role_id"] = static_cast<u32>(b.role());
    session["local_player"] = b.local_player();
    session["peer_count"] = b.peer_count();
    session["max_slots"] = coop::CBridge::kMaxSlots;
    session["api_version"] = b.api_version();
    session["loopback"] = b.loopback();
    session["inbound"] = static_cast<u32>(b.inbound_size());
    return session;
}

const char* lua_role() { return coop::bridge().role_name(); }

bool lua_set_role(LPCSTR role, u32 local_player) {
    if (!is_known_role(role)) {
        coop::bridge().set_role(coop::ERole::offline, 0);
        return false;
    }
    return coop::bridge().set_role(parse_role(role), local_player);
}

bool lua_is_host() { return coop::bridge().is_host(); }

bool lua_is_client() { return coop::bridge().is_client(); }

bool lua_is_offline() { return coop::bridge().is_offline(); }

u32 lua_local_player() { return coop::bridge().local_player(); }

u32 lua_peer_count() { return coop::bridge().peer_count(); }

void lua_set_peer_count(u32 count) { coop::bridge().set_peer_count(count); }

u32 lua_emit_event(LPCSTR name, LPCSTR payload) {
    return coop::bridge().emit_event(name ? name : "", payload ? payload : "");
}

u32 lua_sync(LPCSTR channel, LPCSTR payload) {
    return coop::bridge().send_sync(channel ? channel : "", payload ? payload : "");
}

u32 lua_pump() { return static_cast<u32>(coop::bridge().pump()); }

::luabind::object lua_inbound(lua_State* L, u32 max_count) {
    coop::CBridge& b = coop::bridge();
    ::luabind::object result = ::luabind::newtable(L);

    u32 index = 0;
    coop::SMessage message;
    while (index < max_count && b.pop_inbound(message)) {
        ::luabind::object row = ::luabind::newtable(L);
        row["seq"] = message.seq;
        row["channel"] = message.channel.c_str();
        row["payload"] = message.payload.c_str();
        result[++index] = row;
    }
    return result;
}

void lua_enable_dispatch() {
    coop::bridge().set_dispatch([](const coop::SMessage& message) {
        // F0-05: only a dedicated server runs without a script engine (headless
        // always has one). Never dereference the AI space blindly: keep the
        // message for a later pull instead.
        CScriptEngine* engine = ai().get_script_engine();
        if (!engine) {
            coop::bridge().enqueue(message);
            return;
        }

        ::luabind::functor<void> handler;
        if (!engine->functor("coop_sync.on_inbound", handler)) {
            // Scripts are not loaded yet (menu) or the coop layer is absent:
            // keep the message for a later pull instead of dropping it.
            coop::bridge().enqueue(message);
            return;
        }

        // The engine's release build is compiled without C++ exceptions, which
        // `xrCore.h` expresses as `LUABIND_NO_EXCEPTIONS`: `luabind::error` does
        // not exist there and a failing call is reported by the pcall callback
        // (`CScriptEngine::lua_pcall_failed`) instead of being thrown. Same
        // convention as `process_error` in `script_callback_ex.h`.
#ifndef LUABIND_NO_EXCEPTIONS
        try {
            handler(message.channel.c_str(), message.payload.c_str(), message.seq);
        } catch (::luabind::error& error) {
            if (error.state())
                engine->print_output(error.state(), "", LUA_ERRRUN);
        }
#else   // LUABIND_NO_EXCEPTIONS
        handler(message.channel.c_str(), message.payload.c_str(), message.seq);
#endif  // LUABIND_NO_EXCEPTIONS
    });
}

void lua_set_loopback(bool enabled) { coop::bridge().set_loopback(enabled); }

::luabind::object lua_api_manifest(lua_State* L) {
    ::luabind::object result = ::luabind::newtable(L);

    u32 index = 0;
    for (const coop::SApiEntry& entry : coop::CBridge::api_entries()) {
        ::luabind::object row = ::luabind::newtable(L);
        row["name"] = entry.name;
        row["argc"] = entry.argc;
        row["since"] = entry.since;
        ++index;
        result[index] = row;
    }
    return result;
}

::luabind::object lua_api_fingerprint(lua_State* L) {
    char buffer[32];
    xr_sprintf(buffer, "%016llx",
               static_cast<unsigned long long>(coop::CBridge::manifest_fingerprint()));
    return ::luabind::object(L, buffer);
}

u32 lua_api_version() { return coop::CBridge::kApiVersion; }

::luabind::object lua_stats(lua_State* L) {
    const coop::SStats& s = coop::bridge().stats();

    ::luabind::object result = ::luabind::newtable(L);
    result["events_out"] = static_cast<double>(s.events_out);
    result["events_in"] = static_cast<double>(s.events_in);
    result["sync_out"] = static_cast<double>(s.sync_out);
    result["sync_in"] = static_cast<double>(s.sync_in);
    result["sync_rejected"] = static_cast<double>(s.sync_rejected);
    result["inbound_dropped"] = static_cast<double>(s.inbound_dropped);
    result["last_event_seq"] = s.last_event_seq;
    result["last_sync_seq"] = s.last_sync_seq;
    return result;
}

const char* lua_last_error() { return coop::bridge().last_error().c_str(); }

bool lua_set_debug_enabled(bool enabled, bool launcher_allows) {
    return coop::bridge().set_debug_enabled(enabled, launcher_allows);
}

bool lua_debug_enabled() { return coop::bridge().debug_enabled(); }

bool lua_set_alife_enabled(bool enabled) { return coop::bridge().set_alife_enabled(enabled); }

bool lua_alife_enabled() { return coop::bridge().alife_enabled(); }

bool lua_set_god_mode(bool enabled) { return coop::bridge().set_god_mode(enabled); }

bool lua_god_mode() { return coop::bridge().god_mode(); }

void lua_clear() { coop::bridge().reset(); }
}  // namespace

#pragma optimize("s", on)
void coop_bridge_registrator::script_register(lua_State* L) {
    module(L, "coop_bridge")[
        // session
        def("session", &lua_session, raw<1>()), def("role", &lua_role),
        def("set_role", &lua_set_role), def("is_host", &lua_is_host),
        def("is_client", &lua_is_client), def("is_offline", &lua_is_offline),
        def("local_player", &lua_local_player), def("peer_count", &lua_peer_count),
        def("set_peer_count", &lua_set_peer_count),
        // outbound
        def("emit_event", &lua_emit_event), def("sync", &lua_sync),
        // inbound
        def("pump", &lua_pump), def("inbound", &lua_inbound, raw<1>()),
        def("enable_dispatch", &lua_enable_dispatch), def("set_loopback", &lua_set_loopback),
        // exports parity
        def("api_manifest", &lua_api_manifest, raw<1>()),
        def("api_fingerprint", &lua_api_fingerprint, raw<1>()),
        def("api_version", &lua_api_version),
        // diagnostics
        def("stats", &lua_stats, raw<1>()), def("last_error", &lua_last_error),
        def("clear", &lua_clear),
        // debug surface (PLAN 8.11)
        def("set_debug_enabled", &lua_set_debug_enabled), def("debug_enabled", &lua_debug_enabled),
        def("set_alife_enabled", &lua_set_alife_enabled), def("alife_enabled", &lua_alife_enabled),
        def("set_god_mode", &lua_set_god_mode), def("god_mode", &lua_god_mode)];
}
