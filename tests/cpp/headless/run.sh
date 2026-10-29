#!/usr/bin/env bash
# Host-side check of the headless launch-flag parser (F0-04).
#
# The engine itself is built only on Windows (MSVC/VS2022, F0-01); this script
# verifies the platform-independent part locally: the CLI/env parsing of
# xrCore/headless.cpp. It compiles the real source against ./stub/stdafx.h —
# the source is copied to a scratch dir because its quoted "stdafx.h" resolves
# next to the file in the engine tree.
#
# Usage: tests/cpp/headless/run.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
source_file="$root/src/xrCore/headless.cpp"
header_file="$root/src/xrCore/headless.h"

if [[ ! -f "$source_file" ]]; then
    echo "missing $source_file" >&2
    exit 2
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cp "$source_file" "$work/headless.cpp"
cp "$header_file" "$work/headless.h"
cp "$here/stub/stdafx.h" "$work/stdafx.h"

cxx="${CXX:-g++}"
"$cxx" -std=c++17 -Wall -Wextra -Werror -I"$work" \
    "$here/test_headless_flags.cpp" "$work/headless.cpp" -o "$work/headless_test"

fail=0

clear_env() {
    unset XRAY_HEADLESS XRAY_HEADLESS_TICK_HZ XRAY_HEADLESS_WIDTH XRAY_HEADLESS_HEIGHT \
        XRAY_DBG_ALIFE XRAY_DBG_GOD XRAY_DBG_TELEPORT XRAY_DBG_SPAWN
}

check() { # name launch profile tick debug [alife god teleport spawns]
    local name="$1" launch="$2" profile="$3" tick="$4" debug="$5"
    local alife="${6:-0}" god="${7:-0}" teleport="${8:-0}" spawns="${9:-0}"
    if "$work/headless_test" "$launch" "$profile" "$tick" "$debug" \
        "$alife" "$god" "$teleport" "$spawns"; then
        echo "PASS  $name"
    else
        echo "FAIL  $name"
        fail=1
    fi
}

clear_env
check "no flags"                     ""                                     none   60 0
check "-headless"                    "-headless"                            server 60 0
check "-headless_server"             "-headless_server"                     server 60 0
check "-headless_client"             "-headless_client"                     client 60 0
check "client then server"           "-headless_client -headless_server"    server 60 0
check "server then client"           "-headless_server -headless_client"    client 60 0
check "tick 30"                      "-headless -headless_tick 30"          server 30 0
check "tick 0 rejected"              "-headless -headless_tick 0"           server 60 0
check "tick clamped"                 "-headless -headless_tick 9999"        server 500 0
check "debug flags without -coop_debug" \
      "-headless -dbg_god -dbg_alife_off" server 60 0
check "debug surface armed"          "-headless -coop_debug"                server 60 1
check "god flag armed"               "-headless -coop_debug -dbg_god"       server 60 1 0 1
check "alife off armed"              "-headless -coop_debug -dbg_alife_off" server 60 1 1 0
check "alife on explicit"            "-headless -coop_debug -dbg_alife_on"  server 60 1 0 0

clear_env; export XRAY_HEADLESS=client
check "env profile client"           ""                                     client 60 0
clear_env; export XRAY_HEADLESS=0
check "env profile off"              "-headless"                            none   60 0
clear_env; export XRAY_HEADLESS_TICK_HZ=45
check "env tick rate"                "-headless"                            server 45 0
clear_env; export XRAY_DBG_GOD=1
check "env god without -coop_debug"  "-headless"                            server 60 0
clear_env; export XRAY_DBG_GOD=1
check "env god armed"                "-headless -coop_debug"                server 60 1 0 1

clear_env
if "$work/headless_test" "-headless -coop_debug -dbg_teleport 1.5 -2 3" \
    server 60 1 0 0 1 0 1.5; then
    echo "PASS  teleport coordinates"
else
    echo "FAIL  teleport coordinates"
    fail=1
fi

if "$work/headless_test" \
    "-headless -coop_debug -dbg_spawn Stalker_Soldier_1 1 2 3 -dbg_spawn Bandit_2 -4 5 -6" \
    server 60 1 0 0 0 2 Stalker_Soldier_1; then
    echo "PASS  repeated spawns keep section case"
else
    echo "FAIL  repeated spawns keep section case"
    fail=1
fi

spawn_line="-headless -coop_debug"
for index in 1 2 3 4 5 6 7 8 9 10; do
    spawn_line="$spawn_line -dbg_spawn npc_$index 0 0 0"
done
if "$work/headless_test" "$spawn_line" server 60 1 0 0 0 8; then
    echo "PASS  spawn queue capped at 8"
else
    echo "FAIL  spawn queue capped at 8"
    fail=1
fi

clear_env; export XRAY_DBG_SPAWN="stalker_soldier 10 20 30"
if "$work/headless_test" "-headless -coop_debug" server 60 1 0 0 0 1 "" stalker_soldier; then
    echo "PASS  env spawn"
else
    echo "FAIL  env spawn"
    fail=1
fi
clear_env

# ---------------------------------------------------------------------------
# F0-05: headless keeps the simulation (AI space + script engine); only the
# legacy dedicated server runs without it.
# ---------------------------------------------------------------------------
ai_work="$work/ai"
mkdir -p "$ai_work/xrCore" "$ai_work/xrEngine"
cp "$source_file" "$ai_work/xrCore/headless.cpp"
cp "$header_file" "$ai_work/xrCore/headless.h"
cp "$here/stub/stdafx.h" "$ai_work/xrCore/stdafx.h"
cp "$root/src/xrEngine/ai_simulation.h" "$ai_work/xrEngine/ai_simulation.h"

"$cxx" -std=c++17 -Wall -Wextra -Werror -DENGINE_API= -I"$ai_work" \
    "$here/test_ai_simulation.cpp" "$ai_work/xrCore/headless.cpp" -o "$ai_work/ai_simulation_test"

ai_check() { # name launch dedicated expected
    if "$ai_work/ai_simulation_test" "$2" "$3" "$4" >/dev/null; then
        echo "PASS  $1"
    else
        echo "FAIL  $1"
        fail=1
    fi
}

ai_check "normal game keeps simulation"        ""                  0 1
ai_check "dedicated server has no simulation"  ""                  1 0
ai_check "headless server keeps simulation"    "-headless_server"  1 1
ai_check "headless client keeps simulation"    "-headless_client"  1 1
ai_check "headless without dedicated flag"     "-headless_server"  0 1

if [[ "$fail" -ne 0 ]]; then
    echo "headless flag checks: FAILED" >&2
    exit 1
fi
echo "headless flag checks: OK"

# Structural gate for the startup path (renderer must stay cold in headless).
python_bin="${PYTHON:-python3}"
if command -v "$python_bin" >/dev/null 2>&1; then
    "$python_bin" "$here/check_startup_gates.py" --root "$root"
else
    echo "skip  startup gates (no $python_bin)" >&2
fi
