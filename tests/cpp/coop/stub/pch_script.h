#ifndef COOP_TEST_PCH_SCRIPT_H
#define COOP_TEST_PCH_SCRIPT_H
// Minimal host stub for the coop engine adapter (F0-04/F0-07/F0-12 wiring).
//
// The engine sources need MSVC/VS2022 (F0-01) and cannot be built on Linux, but
// `src/xrGame/coop/coop_core_runtime.cpp` touches exactly three engine surfaces:
// its precompiled header, `ai().script_engine().functor(...)` and `luabind`.
// This stub provides only those, so the adapter is compiled and executed on the
// host against the real coop core (`tests/cpp/coop/run_core.sh`).
//
// Nothing here imitates engine behaviour: the Lua bootstrap simply reports
// "no script layer", exactly like a process without loaded scripts.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

typedef const char* LPCSTR;
typedef char* LPSTR;
typedef unsigned int u32;
typedef unsigned char u8;

// --- luabind ---------------------------------------------------------------
namespace luabind {
class error {
public:
    const void* state() const { return nullptr; }
};

template <typename Result> class functor {
public:
    functor() = default;
    void operator()() const {}
};
}  // namespace luabind

// --- script engine ---------------------------------------------------------
class CScriptEngine {
public:
    template <typename Result> bool functor(const char*, ::luabind::functor<Result>& out) {
        out = ::luabind::functor<Result>();
        return false;  // the host test has no Lua layer
    }
};

struct CAI {
    CScriptEngine m_engine;
    CScriptEngine& script_engine() { return m_engine; }
};

inline CAI& ai() {
    static CAI space;
    return space;
}

#endif  // COOP_TEST_PCH_SCRIPT_H
