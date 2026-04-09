#!/usr/bin/env bash
set -euo pipefail

log() {
  printf '[generate-xcode] %s\n' "$1"
}

die() {
  printf 'error: %s\n' "$1" >&2
  exit 1
}

command -v cmake >/dev/null 2>&1 || die "cmake was not found in PATH."

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"

preset="macos-xcode"
open_project=0

while (($# > 0)); do
  case "$1" in
    --preset)
      (($# >= 2)) || die "--preset requires a value."
      preset="$2"
      shift 2
      ;;
    --open)
      open_project=1
      shift
      ;;
    -h|--help)
      cat <<'EOF'
Usage: ./scripts/generate-xcode.sh [options]

Options:
  --preset <name>   CMake configure preset to use. Defaults to macos-xcode.
  --open            Open the generated .xcodeproj after configure completes.
  -h, --help        Show this help message.
EOF
      exit 0
      ;;
    *)
      die "Unknown argument: $1"
      ;;
  esac
done

build_dir="$repo_root/out/build/$preset"

log "Preset: $preset"
log "Build directory: $build_dir"

cmake --preset "$preset"

shopt -s nullglob
xcode_projects=("$build_dir"/*.xcodeproj)
shopt -u nullglob

((${#xcode_projects[@]} > 0)) || die "No .xcodeproj was generated in $build_dir."

project_path="${xcode_projects[0]}"
log "Generated Xcode project: $project_path"

if [[ "$open_project" -eq 1 ]]; then
  log "Opening project in Xcode."
  open "$project_path"
fi
