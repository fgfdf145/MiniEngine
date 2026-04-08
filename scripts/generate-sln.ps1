[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Preset = "vs2026-x64",
    [switch]$Open
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Info([string]$Message)
{
    Write-Host "[generate-sln] $Message" -ForegroundColor Cyan
}

function Fail([string]$Message)
{
    throw $Message
}

function Invoke-NativeCommand([string]$FilePath, [string[]]$Arguments)
{
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0)
    {
        Fail("Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')")
    }
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildDir = Join-Path $repoRoot "out\build\$Preset"

Write-Info("Preset: $Preset")
Write-Info("Build directory: $buildDir")

Invoke-NativeCommand "cmake" @("--preset", $Preset)

$solutionFiles = @(Get-ChildItem -Path $buildDir -File | Where-Object { $_.Extension -in @(".sln", ".slnx") })
if ($solutionFiles.Count -eq 0)
{
    Fail("No Visual Studio solution file (.sln or .slnx) was generated in $buildDir")
}

$solutionPath = $solutionFiles[0].FullName
Write-Info("Generated solution: $solutionPath")

if ($Open)
{
    Write-Info("Opening solution in Visual Studio.")
    Start-Process $solutionPath | Out-Null
}
