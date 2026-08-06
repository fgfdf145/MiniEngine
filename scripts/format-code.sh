#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
cmake_args=(-DMINIENGINE_FORMAT_MODE=APPLY)

if [[ -n "${CLANG_FORMAT:-}" ]]
then
    cmake_args+=("-DMINIENGINE_CLANG_FORMAT=$CLANG_FORMAT")
fi

cmake "${cmake_args[@]}" -P "$repo_root/cmake/MiniEngineFormat.cmake"
