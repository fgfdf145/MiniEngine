[CmdletBinding()]
param(
    [string]$ClangFormat = $env:CLANG_FORMAT
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "MiniEngineFormatCommand.ps1")

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$arguments = @("-DMINIENGINE_FORMAT_MODE=CHECK")
if (-not [string]::IsNullOrWhiteSpace($ClangFormat))
{
    $resolvedClangFormat = Resolve-MiniEngineFormatCommand $ClangFormat
    $arguments += "-DMINIENGINE_CLANG_FORMAT=$resolvedClangFormat"
}
$arguments += @("-P", (Join-Path $repoRoot "cmake/MiniEngineFormat.cmake"))

& cmake @arguments
if ($LASTEXITCODE -ne 0)
{
    throw "Format check failed with exit code $LASTEXITCODE"
}
