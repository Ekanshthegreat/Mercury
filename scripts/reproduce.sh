#!/usr/bin/env bash
#
# Mercury — fresh-clone reproduction script.
#
# Configures a Release build from scratch, compiles, runs the full test suite,
# and runs the engine smoke check. Exits non-zero on the first failure.
#
# Usage:  ./scripts/reproduce.sh [build-dir]
# Default build dir is ./build-reproduce so it never clobbers an interactive
# build directory.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${1:-${PROJECT_ROOT}/build-reproduce}"

echo "== Mercury reproduce =="
echo "project : ${PROJECT_ROOT}"
echo "build   : ${BUILD_DIR}"

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j

echo "== tests =="
( cd "${BUILD_DIR}" && ctest --output-on-failure )

echo "== engine smoke check =="
"${BUILD_DIR}/mercury_engine_check"

echo "== OK =="
