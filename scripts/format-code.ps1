[CmdletBinding()]
param(
    [string]$ClangFormat = $env:CLANG_FORMAT
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$arguments = @("-DMINIENGINE_FORMAT_MODE=APPLY")
if (-not [string]::IsNullOrWhiteSpace($ClangFormat))
{
    $arguments += "-DMINIENGINE_CLANG_FORMAT=$([System.IO.Path]::GetFullPath($ClangFormat))"
}
$arguments += @("-P", (Join-Path $repoRoot "cmake/MiniEngineFormat.cmake"))

& cmake @arguments
if ($LASTEXITCODE -ne 0)
{
    throw "Formatting failed with exit code $LASTEXITCODE"
}
