#!/usr/bin/env bash
# Copies the rendering acceptance scenes (tests/fixtures/render_scenes) into the asset workspace:
# models to assets/models/<name>/, scenes to assets/scenes/test/. Existing files are kept, so a model
# already imported (and its uuid) is never replaced. See tests/fixtures/render_scenes/README.md.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${repo_root}/tests/fixtures/render_scenes"
assets_dir="${MINIENGINE_ASSETS_DIR:-${repo_root}/assets}"

# Copies every file under $1 to the same relative path under $2, skipping files that exist.
copy_missing()
{
    local from="$1"
    local to="$2"
    while IFS= read -r -d '' file; do
        local target="${to}/${file#"${from}"/}"
        if [[ ! -e "${target}" ]]; then
            mkdir -p "$(dirname "${target}")"
            cp "${file}" "${target}"
        fi
    done < <(find "${from}" -type f -print0)
}

copy_missing "${source_dir}/models" "${assets_dir}/models"
copy_missing "${source_dir}/scenes" "${assets_dir}/scenes/test"
echo "Render scenes installed under ${assets_dir}/scenes/test"
