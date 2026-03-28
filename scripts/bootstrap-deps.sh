#!/usr/bin/env bash
set -euo pipefail

log() {
  printf '[bootstrap] %s\n' "$1"
}

step() {
  printf '==> %s\n' "$1"
}

die() {
  printf 'error: %s\n' "$1" >&2
  exit 1
}

require_command() {
  local name="$1"
  local hint="$2"
  if ! command -v "$name" >/dev/null 2>&1; then
    die "Required command '$name' was not found. $hint"
  fi
}

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"

for arg in "$@"; do
  case "$arg" in
    -h|--help)
      cat <<'EOF'
Usage: ./scripts/bootstrap-deps.sh [options]

Options:
  --vcpkg-root <path>  Override the vcpkg checkout path.
  --triplet <name>     Override the detected vcpkg triplet.
  --skip-install       Only clone/bootstrap vcpkg, do not run `vcpkg install`.
  -h, --help           Show this help message.
EOF
      exit 0
      ;;
  esac
done

platform_name=""
case "$(uname -s)" in
  Linux)
    platform_name="linux"
    ;;
  Darwin)
    platform_name="macos"
    ;;
  MINGW*|MSYS*|CYGWIN*)
    die "Use scripts/bootstrap-deps.ps1 on Windows."
    ;;
  *)
    die "Unsupported operating system for dependency bootstrap."
    ;;
esac

architecture_name=""
case "$(uname -m)" in
  x86_64|amd64)
    architecture_name="x64"
    ;;
  arm64|aarch64)
    architecture_name="arm64"
    ;;
  *)
    die "Unsupported architecture '$(uname -m)'."
    ;;
esac

default_triplet() {
  case "$platform_name/$architecture_name" in
    linux/x64)
      printf 'x64-linux'
      ;;
    linux/arm64)
      printf 'arm64-linux'
      ;;
    macos/x64)
      printf 'x64-osx'
      ;;
    macos/arm64)
      printf 'arm64-osx'
      ;;
    *)
      die "No default vcpkg triplet mapping for '$platform_name/$architecture_name'."
      ;;
  esac
}

vcpkg_root="${VCPKG_ROOT:-$repo_root/.deps/vcpkg}"
triplet=""
skip_install=0

while (($# > 0)); do
  case "$1" in
    --vcpkg-root)
      (($# >= 2)) || die "--vcpkg-root requires a value."
      vcpkg_root="$2"
      shift 2
      ;;
    --triplet)
      (($# >= 2)) || die "--triplet requires a value."
      triplet="$2"
      shift 2
      ;;
    --skip-install)
      skip_install=1
      shift
      ;;
    *)
      die "Unknown argument: $1"
      ;;
  esac
done

if [[ -z "$triplet" ]]; then
  triplet="$(default_triplet)"
fi

require_command git "Install Git and retry."
require_command cmake "Install CMake 3.25+ and retry."

if [[ "$vcpkg_root" != /* ]]; then
  vcpkg_root="$repo_root/$vcpkg_root"
fi

log "Repository root: $repo_root"
log "Host platform: $platform_name ($architecture_name)"
log "vcpkg root: $vcpkg_root"
log "Selected triplet: $triplet"

if [[ -d "$vcpkg_root/.git" ]]; then
  log "Using existing vcpkg checkout at '$vcpkg_root'."
else
  if [[ -e "$vcpkg_root" ]] && [[ -n "$(ls -A "$vcpkg_root" 2>/dev/null || true)" ]]; then
    die "Target vcpkg directory exists but is not a git checkout: '$vcpkg_root'."
  fi

  mkdir -p -- "$(dirname -- "$vcpkg_root")"
  step "Cloning vcpkg into '$vcpkg_root'"
  git clone --depth 1 https://github.com/microsoft/vcpkg.git "$vcpkg_root"
fi

if [[ ! -x "$vcpkg_root/bootstrap-vcpkg.sh" ]]; then
  die "vcpkg bootstrap script was not found: '$vcpkg_root/bootstrap-vcpkg.sh'."
fi

step "Bootstrapping vcpkg for $platform_name"
"$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics

if [[ ! -x "$vcpkg_root/vcpkg" ]]; then
  die "The vcpkg executable was not produced at '$vcpkg_root/vcpkg'."
fi

if [[ "$skip_install" -eq 0 ]]; then
  step "Installing manifest dependencies for triplet '$triplet'"
  "$vcpkg_root/vcpkg" install \
    "--x-manifest-root=$repo_root" \
    "--triplet=$triplet"
else
  log "Skipping 'vcpkg install' because --skip-install was provided."
fi

step "Dependency bootstrap completed"
printf '\n'
printf 'Next steps:\n'
printf '  1. Export VCPKG_ROOT="%s" if you want CMake to reuse this checkout.\n' "$vcpkg_root"
printf '  2. Open-source dependencies are ready on %s, but the current MiniEngine FBX configure path still needs Windows-oriented Autodesk FBX SDK integration before a full build can succeed.\n' "$platform_name"
