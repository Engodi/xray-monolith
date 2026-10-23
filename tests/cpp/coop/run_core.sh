#!/usr/bin/env bash
# Host-side end-to-end check of the coop core wiring (F0-04/F0-05/F0-07/F0-12/F0-02).
#
# The engine compiles only on Windows (MSVC/VS2022, F0-01), but the adapter
# (`src/xrGame/coop/coop_core_runtime.cpp`) touches three engine surfaces only
# (precompiled header, `ai().script_engine().functor`, luabind). With the stub in
# ./stub it runs on the host against the real coop core, so the whole wiring —
# session tick, environment events, F0-07 artifact, bot channel and the F0-02
# transport — is exercised before the Windows build.
#
# The coop checkout is a sibling of the engine tree in CI (`engine/`, `coop/`).
# Override with COOP_SOURCE_ROOT when the layout differs.
#
# Two runs:
#   1. `test_core_runtime.cpp` — one process on the in-process queues
#      (`XRN_HARNESS_NET=inprocess`): the debug surface and the bot channel.
#   2. `test_cross_process.cpp` — host + client process over real UDP
#      (loopback): join, snapshot, intent, ack, transport status.
#
# Usage: tests/cpp/coop/run_core.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
coop="${COOP_SOURCE_ROOT:-$root/../coop}"

# MSVC-only regressions of the Lua export (a `luabind::error` catch outside the
# exceptions guard, a wrapper shadowed by the Lua C API) compile fine on the
# host and only fail in the Windows build, so they are checked statically.
python3 "$here/check_msvc_compat.py"

if [[ ! -d "$coop/src/core/include/xrn/core" ]]; then
    echo "coop checkout not found at $coop (set COOP_SOURCE_ROOT)" >&2
    echo "skip: run.sh покрывает движковые модули, run_core.sh требует ядро" >&2
    exit 2
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cxx="${CXX:-g++}"
flags=(-std=c++17 -Wall -Wextra -pthread -I"$here/stub" -I"$root/src/xrGame"
       -I"$coop/src/core/include" -I"$coop/src/replication/include"
       -I"$coop/src/cfgdiff/include" -I"$coop/src/transport/include"
       -I"$coop/src/transport/src")

adapter_sources=(
    "$root/src/xrGame/coop/coop_bridge.cpp"
    "$root/src/xrGame/coop/coop_bot_json.cpp"
    "$root/src/xrGame/coop/coop_bot_channel.cpp"
    "$root/src/xrGame/coop/coop_transport.cpp"
    "$root/src/xrGame/coop/coop_core_runtime.cpp"
)

# The coop modules are compiled from their own sources, including
# `transport_link.{cpp}` of the core: the F0-02 C ABI adapter is part of the
# wiring now, so the check covers the composed build.
coop_sources=()
while IFS= read -r source; do
    coop_sources+=("$source")
done < <(find "$coop/src/core/src" "$coop/src/replication/src" "$coop/src/cfgdiff/src" \
              "$coop/src/transport/src" -maxdepth 1 -name '*.cpp' | sort)

"$cxx" "${flags[@]}" "$here/test_core_runtime.cpp" "${adapter_sources[@]}" \
    "${coop_sources[@]}" -o "$work/coop_core_runtime_test"
"$work/coop_core_runtime_test"

"$cxx" "${flags[@]}" "$here/test_cross_process.cpp" "${adapter_sources[@]}" \
    "${coop_sources[@]}" -o "$work/coop_cross_process_test"
"$work/coop_cross_process_test"
