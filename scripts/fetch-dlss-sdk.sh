#!/usr/bin/env bash
# Downloads the parts of NVIDIA's DLSS SDK (https://github.com/NVIDIA/DLSS) the engine builds against
# into .deps/dlss: the NGX headers, the NGX static library and the DLSS runtime DLLs / shared objects
# (super resolution, and ray reconstruction, DLSS-D).
# The SDK is not committed and not in vcpkg; without it the engine builds as before, with its own TAA
# only (see cmake/MiniEngineDlss.cmake). Re-running with the same version does nothing.
#
# Usage: ./scripts/fetch-dlss-sdk.sh [destination]   (default: <repo>/.deps/dlss)
set -euo pipefail

version="v310.9.1"
# Bumped when the set of files fetched changes, so an older fetch is replaced.
layout="3"
base_url="https://raw.githubusercontent.com/NVIDIA/DLSS/${version}"

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
destination="${1:-$repo_root/.deps/dlss}"

log()
{
    printf '[dlss] %s\n' "$1"
}

if [[ -f "$destination/VERSION" && "$(cat "$destination/VERSION")" == "$version.$layout" ]]; then
    log "DLSS SDK $version already in $destination"
    exit 0
fi

# Every header: nvsdk_ngx_helpers.h pulls in the D3D and CUDA helpers, which guard themselves.
headers=(
    nvsdk_ngx.h
    nvsdk_ngx_defs.h
    nvsdk_ngx_defs_dlssd.h
    nvsdk_ngx_defs_dlssg.h
    nvsdk_ngx_defs_vk.h
    nvsdk_ngx_helpers.h
    nvsdk_ngx_helpers_cuda.h
    nvsdk_ngx_helpers_d3d.h
    nvsdk_ngx_helpers_dlssd.h
    nvsdk_ngx_helpers_dlssd_cuda.h
    nvsdk_ngx_helpers_dlssd_d3d.h
    nvsdk_ngx_helpers_dlssd_vk.h
    nvsdk_ngx_helpers_dlssg.h
    nvsdk_ngx_helpers_dlssg_d3d.h
    nvsdk_ngx_helpers_dlssg_vk.h
    nvsdk_ngx_helpers_vk.h
    nvsdk_ngx_loader.h
    nvsdk_ngx_params.h
    nvsdk_ngx_params_dlssd.h
    nvsdk_ngx_params_dlssg.h
    nvsdk_ngx_standalone_common.h
    nvsdk_ngx_standalone_cuda.h
    nvsdk_ngx_vk.h
)

files=(LICENSE.txt)
for header in "${headers[@]}"; do
    files+=("include/$header")
done

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        # The /MD (_d) library, release and debug-CRT builds, and the release runtime (the dev one
        # draws a watermark).
        files+=(
            lib/Windows_x86_64/x64/nvsdk_ngx_d.lib
            lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib
            lib/Windows_x86_64/rel/nvngx_dlss.dll
            lib/Windows_x86_64/rel/nvngx_dlssd.dll
        )
        ;;
    Linux)
        files+=(
            lib/Linux_x86_64/libnvsdk_ngx.a
            "lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.${version#v}"
            "lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.${version#v}"
        )
        ;;
    *)
        log "DLSS has no runtime for $(uname -s); nothing to fetch."
        exit 0
        ;;
esac

staging="$destination.partial"
rm -rf "$staging"
for file in "${files[@]}"; do
    log "Fetching $file"
    mkdir -p "$staging/$(dirname "$file")"
    curl --fail --location --silent --show-error --retry 3 --output "$staging/$file" "$base_url/$file"
done
printf '%s' "$version.$layout" > "$staging/VERSION"

rm -rf "$destination"
mv "$staging" "$destination"
log "DLSS SDK $version installed in $destination"
