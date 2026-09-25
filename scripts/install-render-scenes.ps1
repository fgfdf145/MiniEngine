# Copies the rendering acceptance scenes (tests/fixtures/render_scenes) into the asset workspace:
# models to assets/models/<name>/, scenes to assets/scenes/test/. Existing files are kept, so a model
# already imported (and its uuid) is never replaced. See tests/fixtures/render_scenes/README.md.
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$sourceDir = Join-Path $repoRoot "tests/fixtures/render_scenes"
$assetsDir = if ($env:MINIENGINE_ASSETS_DIR) { $env:MINIENGINE_ASSETS_DIR } else { Join-Path $repoRoot "assets" }

foreach ($pair in @(@("models", "models"), @("scenes", "scenes/test")))
{
    $from = Join-Path $sourceDir $pair[0]
    $to = Join-Path $assetsDir $pair[1]
    Get-ChildItem -Path $from -Recurse -File | ForEach-Object {
        $target = Join-Path $to $_.FullName.Substring($from.Length + 1)
        if (-not (Test-Path $target))
        {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            Copy-Item $_.FullName $target
        }
    }
}
Write-Host "Render scenes installed under $(Join-Path $assetsDir 'scenes/test')"
