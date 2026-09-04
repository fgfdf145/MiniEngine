#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
cmake_args=()

if [[ -n "${MINIENGINE_DOCS_OUTPUT:-}" ]]
then
    cmake_args+=("-DMINIENGINE_DOCS_OUTPUT=$MINIENGINE_DOCS_OUTPUT")
fi

if [[ -n "${DOXYGEN:-}" ]]
then
    cmake_args+=("-DMINIENGINE_DOXYGEN=$DOXYGEN")
fi

if [[ -n "${DOT:-}" ]]
then
    cmake_args+=("-DMINIENGINE_DOT=$DOT")
fi

cmake "${cmake_args[@]+"${cmake_args[@]}"}" \
    -P "$repo_root/cmake/MiniEngineDocs.cmake"
