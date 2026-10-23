////////////////////////////////////////////////////////////////////////////
//	Module 		: coop/coop_core_script.cpp
//	Created 	: 23.10.2026
//	Description : Lua export of the coop core runtime (F0-04/F0-07 wiring)
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "coop/coop_core_script.h"
#include "coop/coop_core_runtime.h"

using namespace luabind;

namespace {
// `lua_status` is taken by the Lua C API (lua.h: `int lua_status(lua_State*)`),
// so the wrapper is named after the exported entry point instead: `&lua_status`
// would be an unresolved overload set and `luabind::def` could not deduce `F`.
::luabind::object lua_runtime_status(lua_State* L) {
    const coop::SCoreStatus status = coop::core_runtime().status();

    ::luabind::object result = ::luabind::newtable(L);
    result["active"] = status.active;
    result["host"] = status.host;
    result["degraded"] = status.degraded;
    result["role"] = status.role.c_str();
    result["instance"] = status.instance.c_str();
    result["run_dir"] = status.run_dir.c_str();
    result["log_path"] = status.log_path.c_str();
    result["error"] = status.error.c_str();
    result["steps"] = status.steps;
    result["world_tick"] = status.world_tick;
    result["peers"] = status.peers;
    result["events"] = static_cast<u32>(status.events);
    return result;
}

bool lua_available() { return coop::core_runtime().active(); }

bool lua_start() { return coop::core_runtime().start(); }

::luabind::object lua_command(lua_State* L, LPCSTR name, LPCSTR argument) {
    std::string detail;
    const bool ok = coop::core_runtime().command(name != nullptr ? name : "",
                                                 argument != nullptr ? argument : "", detail);

    ::luabind::object result = ::luabind::newtable(L);
    result["ok"] = ok;
    result["detail"] = detail.c_str();
    return result;
}
}  // namespace

#pragma optimize("s", on)
void coop_core_registrator::script_register(lua_State* L) {
    module(L, "coop_runtime")[def("available", &lua_available), def("start", &lua_start),
                              def("status", &lua_runtime_status, raw<1>()),
                              def("command", &lua_command, raw<1>())];
}
