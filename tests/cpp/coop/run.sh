#!/usr/bin/env bash
# Host-side check of the bot action channel (F0-12 wiring).
#
# The engine itself is built only on Windows (MSVC/VS2022, F0-01). The bot
# channel, however, is deliberately plain C++17 (`src/xrGame/coop/coop_bot_*.cpp`
# includes no engine header), so its protocol — ordering, tokens, acks, the
# observation stream — is verified here before the Windows build runs.
#
# Usage: tests/cpp/coop/run.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
coop_dir="$root/src/xrGame/coop"

for file in coop_bot_json.cpp coop_bot_channel.cpp coop_bot_json.h coop_bot_channel.h; do
    if [[ ! -f "$coop_dir/$file" ]]; then
        echo "missing $coop_dir/$file" >&2
        exit 2
    fi
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cxx="${CXX:-g++}"
"$cxx" -std=c++17 -Wall -Wextra -Werror -I"$root/src/xrGame" \
    "$here/test_bot_channel.cpp" "$coop_dir/coop_bot_json.cpp" \
    "$coop_dir/coop_bot_channel.cpp" -o "$work/coop_bot_channel_test"

"$work/coop_bot_channel_test"
