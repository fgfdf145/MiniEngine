[CmdletBinding()]
param(
    [string]$VcpkgRoot,
    [string]$Triplet,
    [switch]$SkipInstall,
    [switch]$SkipSdkCheck
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Info([string]$Message)
{
    Write-Host "[bootstrap] $Message" -ForegroundColor Cyan
}

function Write-Step([string]$Message)
{
    Write-Host "==> $Message" -ForegroundColor Green
}

function Fail([string]$Message)
{
    throw $Message
}

function Test-CommandAvailable([string]$Name)
{
    return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Require-Command([string]$Name, [string]$Hint)
{
    if (-not (Test-CommandAvailable $Name))
    {
        Fail("Required command '$Name' was not found. $Hint")
    }
}

function Get-RepoRoot()
{
    return [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
}

function Get-PlatformName()
{
    if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows))
    {
        return "windows"
    }
    if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Linux))
    {
        return "linux"
    }
    if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::OSX))
    {
        return "macos"
    }

    Fail("Unsupported operating system for dependency bootstrap.")
    return ""
}

function Get-ArchitectureName()
{
    $architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
    switch ($architecture)
    {
        "x64" { return "x64" }
        "x86" { return "x86" }
        "arm64" { return "arm64" }
        default { Fail("Unsupported architecture '$architecture'.") }
    }

    return ""
}

function Get-DefaultTriplet([string]$PlatformName, [string]$ArchitectureName)
{
    switch ("$PlatformName/$ArchitectureName")
    {
        "windows/x64" { return "x64-windows" }
        "windows/x86" { return "x86-windows" }
        "windows/arm64" { return "arm64-windows" }
        "linux/x64" { return "x64-linux" }
        "linux/arm64" { return "arm64-linux" }
        "macos/x64" { return "x64-osx" }
        "macos/arm64" { return "arm64-osx" }
        default { Fail("No default vcpkg triplet mapping for '$PlatformName/$ArchitectureName'.") }
    }

    return ""
}

function Resolve-AbsolutePath([string]$PathValue)
{
    return [System.IO.Path]::GetFullPath($PathValue)
}

function Get-DefaultVcpkgRoot([string]$RepoRoot)
{
    if ($env:VCPKG_ROOT)
    {
        return (Resolve-AbsolutePath $env:VCPKG_ROOT)
    }

    return (Join-Path $RepoRoot ".deps\vcpkg")
}

function Invoke-NativeCommand([string]$FilePath, [string[]]$Arguments)
{
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0)
    {
        Fail("Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')")
    }
}

function Ensure-VcpkgRepository([string]$ResolvedVcpkgRoot)
{
    if (Test-Path (Join-Path $ResolvedVcpkgRoot ".git"))
    {
        Write-Info("Using existing vcpkg checkout at '$ResolvedVcpkgRoot'.")
        return
    }

    if (Test-Path $ResolvedVcpkgRoot)
    {
        $existingItems = Get-ChildItem -Force -Path $ResolvedVcpkgRoot -ErrorAction SilentlyContinue
        if ($existingItems.Count -gt 0)
        {
            Fail("Target vcpkg directory exists but is not a git checkout: '$ResolvedVcpkgRoot'.")
        }
    }
    else
    {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ResolvedVcpkgRoot) | Out-Null
    }

    Write-Step("Cloning vcpkg into '$ResolvedVcpkgRoot'")
    Invoke-NativeCommand "git" @(
        "clone",
        "--depth", "1",
        "https://github.com/microsoft/vcpkg.git",
        $ResolvedVcpkgRoot
    )
}

function Bootstrap-Vcpkg([string]$ResolvedVcpkgRoot, [string]$PlatformName)
{
    if ($PlatformName -eq "windows")
    {
        $bootstrapScript = Join-Path $ResolvedVcpkgRoot "bootstrap-vcpkg.bat"
        if (-not (Test-Path $bootstrapScript))
        {
            Fail("vcpkg bootstrap script was not found: '$bootstrapScript'.")
        }

        Write-Step("Bootstrapping vcpkg for Windows")
        Invoke-NativeCommand $bootstrapScript @("-disableMetrics")
        return
    }

    $bootstrapScript = Join-Path $ResolvedVcpkgRoot "bootstrap-vcpkg.sh"
    if (-not (Test-Path $bootstrapScript))
    {
        Fail("vcpkg bootstrap script was not found: '$bootstrapScript'.")
    }

    Write-Step("Bootstrapping vcpkg for $PlatformName")
    Invoke-NativeCommand $bootstrapScript @("-disableMetrics")
}

function Get-VcpkgExecutable([string]$ResolvedVcpkgRoot, [string]$PlatformName)
{
    if ($PlatformName -eq "windows")
    {
        return (Join-Path $ResolvedVcpkgRoot "vcpkg.exe")
    }

    return (Join-Path $ResolvedVcpkgRoot "vcpkg")
}

function Find-FirstExistingPath([string[]]$Candidates)
{
    foreach ($candidate in $Candidates)
    {
        if ([string]::IsNullOrWhiteSpace($candidate))
        {
            continue
        }

        if (Test-Path $candidate)
        {
            return (Resolve-AbsolutePath $candidate)
        }
    }

    return $null
}

function Find-LatestDirectory([string]$RootPath, [string]$HeaderRelativePath)
{
    if (-not (Test-Path $RootPath))
    {
        return $null
    }

    $directories = Get-ChildItem -Path $RootPath -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
    foreach ($directory in $directories)
    {
        $headerPath = Join-Path $directory.FullName $HeaderRelativePath
        if (Test-Path $headerPath)
        {
            return $directory.FullName
        }
    }

    return $null
}

function Get-VulkanSdkRootHint()
{
    $fromEnvironment = Find-FirstExistingPath @($env:VULKAN_SDK)
    if ($null -ne $fromEnvironment)
    {
        return $fromEnvironment
    }

    return (Find-LatestDirectory "C:\VulkanSDK" "Include\vulkan\vulkan.h")
}

$repoRoot = Get-RepoRoot
$platformName = Get-PlatformName
$architectureName = Get-ArchitectureName
$resolvedVcpkgRoot =
    if ($VcpkgRoot)
    {
        Resolve-AbsolutePath $VcpkgRoot
    }
    else
    {
        Get-DefaultVcpkgRoot $repoRoot
    }
$selectedTriplet =
    if ($Triplet)
    {
        $Triplet
    }
    else
    {
        Get-DefaultTriplet $platformName $architectureName
    }

Require-Command "git" "Install Git and retry."
Require-Command "cmake" "Install CMake 3.25+ and retry."

Write-Info("Repository root: $repoRoot")
Write-Info("Host platform: $platformName ($architectureName)")
Write-Info("vcpkg root: $resolvedVcpkgRoot")
Write-Info("Selected triplet: $selectedTriplet")

Ensure-VcpkgRepository $resolvedVcpkgRoot
Bootstrap-Vcpkg $resolvedVcpkgRoot $platformName

$vcpkgExecutable = Get-VcpkgExecutable $resolvedVcpkgRoot $platformName
if (-not (Test-Path $vcpkgExecutable))
{
    Fail("The vcpkg executable was not produced at '$vcpkgExecutable'.")
}

if (-not $SkipInstall)
{
    Write-Step("Installing manifest dependencies for triplet '$selectedTriplet'")
    Invoke-NativeCommand $vcpkgExecutable @(
        "install",
        "--x-manifest-root=$repoRoot",
        "--triplet=$selectedTriplet"
    )
}
else
{
    Write-Info("Skipping 'vcpkg install' because -SkipInstall was provided.")
}

if (-not $SkipSdkCheck)
{
    if ($platformName -eq "windows")
    {
        $vulkanSdkRoot = Get-VulkanSdkRootHint
        if ($null -ne $vulkanSdkRoot)
        {
            Write-Info("Detected Vulkan SDK root: $vulkanSdkRoot")
        }
        else
        {
            Write-Warning("Vulkan SDK was not detected. Set VULKAN_SDK before configuring the project if glslc is unavailable.")
        }
    }
    else
    {
        Write-Info("Open-source dependencies were bootstrapped for $platformName.")
    }
}

Write-Step("Dependency bootstrap completed")
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Set VCPKG_ROOT to '$resolvedVcpkgRoot' if you want CMake to reuse this checkout."
if ($platformName -eq "windows")
{
    Write-Host "  2. Ensure VULKAN_SDK points to a valid SDK install if glslc is not on PATH."
    Write-Host "  3. Build with automatic full-thread parallelism, for example: .\scripts\build.ps1 x64-debug"
}
else
{
    Write-Host "  2. Configure and build normally on $platformName."
}
