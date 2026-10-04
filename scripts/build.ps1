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

# A build preset names the configure preset it builds; the two names differ for the
# Visual Studio presets (vs2026-x64-debug builds vs2026-x64), so configure and the build
# directory have to follow the configure preset, not the build preset's own name.
function Resolve-ConfigurePreset([string]$BuildPresetName)
{
    $presetsPath = Join-Path $repoRoot "CMakePresets.json"
    $buildPresets = @((Get-Content -Raw $presetsPath | ConvertFrom-Json).buildPresets)
    $name = $BuildPresetName
    while ($name)
    {
        $buildPreset = $buildPresets | Where-Object { $_.name -eq $name } | Select-Object -First 1
        if (-not $buildPreset)
        {
            break
        }
        if ($buildPreset.PSObject.Properties["configurePreset"])
        {
            return $buildPreset.configurePreset
        }
        $name = if ($buildPreset.PSObject.Properties["inherits"]) { @($buildPreset.inherits)[0] } else { $null }
    }
    Fail("No configure preset found for build preset '$BuildPresetName' in $presetsPath.")
}

$configurePreset = Resolve-ConfigurePreset $Preset
$buildDir = Join-Path $repoRoot "out\build\$configurePreset"
$resolvedJobs = if ($Jobs -gt 0)
{
    $Jobs
}
else
{
    [System.Environment]::ProcessorCount
}

Write-Info("Preset: $Preset (configure preset: $configurePreset)")
Write-Info("Build directory: $buildDir")
Write-Info("Parallel jobs: $resolvedJobs")

if (-not (Test-Path $buildDir))
{
    Write-Info("Build directory does not exist yet, running configure first.")
    Invoke-NativeCommand "cmake" @("--preset", $configurePreset)
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
