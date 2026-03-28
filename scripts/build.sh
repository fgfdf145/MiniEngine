#!/usr/bin/env bash
set -euo pipefail

log() {
  printf '[build] %s\n' "$1"
}

die() {
  printf 'error: %s\n' "$1" >&2
  exit 1
}

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"

preset="${1:-linux-debug}"
target=""
config=""
jobs="${JOBS:-0}"

shift_count=0
if (($# > 0)); then
  shift_count=1
fi
shift "$shift_count"

while (($# > 0)); do
  case "$1" in
    --target)
      (($# >= 2)) || die "--target requires a value."
      target="$2"
      shift 2
      ;;
    --config)
      (($# >= 2)) || die "--config requires a value."
      config="$2"
      shift 2
      ;;
    --jobs|-j)
      (($# >= 2)) || die "--jobs requires a value."
      jobs="$2"
      shift 2
      ;;
    -h|--help)
      cat <<'EOF'
Usage: ./scripts/build.sh [preset] [options]

Options:
  --target <name>   Build a specific target.
  --config <name>   Forward an explicit config to CMake multi-config generators.
  --jobs, -j <n>    Override the detected logical CPU count.
  -h, --help        Show this help message.
EOF
      exit 0
      ;;
    *)
      die "Unknown argument: $1"
      ;;
  esac
done

if [[ "$jobs" == "0" ]]; then
  if command -v getconf >/dev/null 2>&1; then
    jobs="$(getconf _NPROCESSORS_ONLN)"
  elif command -v nproc >/dev/null 2>&1; then
    jobs="$(nproc)"
  elif command -v sysctl >/dev/null 2>&1; then
    jobs="$(sysctl -n hw.logicalcpu)"
  else
    jobs="1"
  fi
fi

build_dir="$repo_root/out/build/$preset"

log "Preset: $preset"
log "Build directory: $build_dir"
log "Parallel jobs: $jobs"

if [[ ! -d "$build_dir" ]]; then
  log "Build directory does not exist yet, running configure first."
  cmake --preset "$preset"
fi

build_args=(--build --preset "$preset" --parallel "$jobs")

if [[ -n "$config" ]]; then
  build_args+=(--config "$config")
fi

if [[ -n "$target" ]]; then
  build_args+=(--target "$target")
fi

cmake "${build_args[@]}"
