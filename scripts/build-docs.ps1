[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [string]$Doxygen = $env:DOXYGEN,
    [string]$Dot = $env:DOT,
    [switch]$Open
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$arguments = @()
if (-not [string]::IsNullOrWhiteSpace($OutputDirectory))
{
    $resolvedOutput = [System.IO.Path]::GetFullPath(
        [System.IO.Path]::Combine($repoRoot, $OutputDirectory))
    $arguments += "-DMINIENGINE_DOCS_OUTPUT=$resolvedOutput"
}
else
{
    $resolvedOutput = Join-Path $repoRoot "out\docs"
}
if (-not [string]::IsNullOrWhiteSpace($Doxygen))
{
    $arguments += "-DMINIENGINE_DOXYGEN=$Doxygen"
}
if (-not [string]::IsNullOrWhiteSpace($Dot))
{
    $arguments += "-DMINIENGINE_DOT=$Dot"
}
$arguments += @("-P", (Join-Path $repoRoot "cmake/MiniEngineDocs.cmake"))

& cmake @arguments
if ($LASTEXITCODE -ne 0)
{
    throw "Documentation build failed with exit code $LASTEXITCODE"
}

if ($Open)
{
    Start-Process (Join-Path $resolvedOutput "html\index.html")
}
