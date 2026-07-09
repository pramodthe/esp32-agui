#!/usr/bin/env bash
# Host unit tests for the agui_sdk [device] extensions (no ESP-IDF, no hardware).
# Compiles the minimal vendored-SDK sources the extensions touch + test_extensions.cpp with the
# host g++ and runs them. Intended to be run after re-syncing the vendored SDK (see ../PATCHES.md)
# to catch silent breakage of REASONING_* / interrupt / resume support.
#
#   ./run_host_tests.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_SRC="$(cd "$SCRIPT_DIR/../src" && pwd)"
BUILD_DIR="${TMPDIR:-/tmp}/agui_sdk_hosttest"
mkdir -p "$BUILD_DIR"

# Locate nlohmann/json.hpp: system, the project's managed component, or a pinned download.
find_nlohmann() {
    local candidates=(
        /usr/include
        /usr/local/include
        "$SCRIPT_DIR/../../../managed_components/johboh__nlohmann-json/single_include"
        "$SCRIPT_DIR/../../../managed_components/johboh__nlohmann-json/include"
    )
    for p in "${candidates[@]}"; do
        if [ -f "$p/nlohmann/json.hpp" ]; then echo "$p"; return 0; fi
    done
    local inc="$BUILD_DIR/nlohmann_include"
    if [ ! -f "$inc/nlohmann/json.hpp" ]; then
        mkdir -p "$inc/nlohmann"
        echo "nlohmann/json.hpp not found locally; downloading v3.12.0..." >&2
        curl -fsSL "https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp" \
            -o "$inc/nlohmann/json.hpp"
    fi
    echo "$inc"
}

NLOHMANN_INC="$(find_nlohmann)"
echo "nlohmann include: $NLOHMANN_INC"

# The vendored SDK sources the extensions depend on (all host-portable: no ESP-IDF, no libcurl).
SRCS=(
    "$SDK_SRC/core/error.cpp"
    "$SDK_SRC/core/event.cpp"
    "$SDK_SRC/core/session_types.cpp"
    "$SDK_SRC/core/state.cpp"
    "$SDK_SRC/core/uuid.cpp"
    "$SDK_SRC/core/logger.cpp"
)

CXX="${CXX:-g++}"
echo "compiling with $CXX ..."
"$CXX" -std=c++17 -O0 -g \
    -I"$SDK_SRC" -I"$NLOHMANN_INC" \
    "$SCRIPT_DIR/test_extensions.cpp" "${SRCS[@]}" \
    -o "$BUILD_DIR/test_extensions"

echo "running ..."
"$BUILD_DIR/test_extensions"
