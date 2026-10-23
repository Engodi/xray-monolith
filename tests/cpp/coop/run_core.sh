#!/usr/bin/env bash
# Host-side end-to-end check of the coop core wiring (F0-04/F0-05/F0-07/F0-12).
#
# The engine compiles only on Windows (MSVC/VS2022, F0-01), but the adapter
# (`src/xrGame/coop/coop_core_runtime.cpp`) touches three engine surfaces only
# (precompiled header, `ai().script_engine().functor`, luabind). With the stub in
# ./stub it runs on the host against the real coop core, so the whole wiring —
# session tick, environment events, F0-07 artifact, bot channel — is exercised
# before the Windows build.
#
# The coop checkout is a sibling of the engine tree in CI (`engine/`, `coop/`).
# Override with COOP_SOURCE_ROOT when the layout differs.
#
# Usage: tests/cpp/coop/run_core.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
coop="${COOP_SOURCE_ROOT:-$root/../coop}"

if [[ ! -d "$coop/src/core/include/xrn/core" ]]; then
    echo "coop checkout not found at $coop (set COOP_SOURCE_ROOT)" >&2
    echo "skip: run.sh покрывает движковые модули, run_core.sh требует ядро" >&2
    exit 2
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cxx="${CXX:-g++}"
flags=(-std=c++17 -Wall -Wextra -I"$here/stub" -I"$root/src/xrGame"
       -I"$coop/src/core/include" -I"$coop/src/replication/include" -I"$coop/src/cfgdiff/include")

sources=(
    "$here/test_core_runtime.cpp"
    "$root/src/xrGame/coop/coop_bridge.cpp"
    "$root/src/xrGame/coop/coop_bot_json.cpp"
    "$root/src/xrGame/coop/coop_bot_channel.cpp"
    "$root/src/xrGame/coop/coop_core_runtime.cpp"
)

# The core is compiled from its own sources: `transport_link.cpp` is the single
# file bound to the F0-02 C ABI and is therefore not part of the wiring yet.
while IFS= read -r source; do
    case "$source" in
        */transport_link.cpp) continue ;;
    esac
    sources+=("$source")
done < <(find "$coop/src/core/src" "$coop/src/replication/src" "$coop/src/cfgdiff/src" \
              -maxdepth 1 -name '*.cpp' | sort)

"$cxx" "${flags[@]}" "${sources[@]}" -o "$work/coop_core_runtime_test"
"$work/coop_core_runtime_test"
