[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Preset = "x64-debug",
    [string]$Target,
    [string]$Config,
    [int]$Jobs = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Info([string]$Message)
{
    Write-Host "[build] $Message" -ForegroundColor Cyan
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
$resolvedJobs = if ($Jobs -gt 0) { $Jobs } else { [System.Environment]::ProcessorCount }

Write-Info("Preset: $Preset")
Write-Info("Build directory: $buildDir")
Write-Info("Parallel jobs: $resolvedJobs")

if (-not (Test-Path $buildDir))
{
    Write-Info("Build directory does not exist yet, running configure first.")
    Invoke-NativeCommand "cmake" @("--preset", $Preset)
}

$buildArguments = @("--build", "--preset", $Preset, "--parallel", $resolvedJobs.ToString())

if (-not [string]::IsNullOrWhiteSpace($Config))
{
    $buildArguments += @("--config", $Config)
}

if (-not [string]::IsNullOrWhiteSpace($Target))
{
    $buildArguments += @("--target", $Target)
}

Invoke-NativeCommand "cmake" $buildArguments
