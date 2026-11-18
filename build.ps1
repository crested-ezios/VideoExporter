#Requires -Version 5.1
# SPDX-License-Identifier: MIT
# NOTE: Keep this script ASCII-only (Windows PowerShell 5.1 ANSI parsing).
[CmdletBinding()]
param(
    [ValidateSet('Runtime', 'Frontend', 'Tests', 'All')]
    [string]$Target = 'All',
    [string]$OutputRoot = '',
    [string]$WindowsSdkRoot = '',
    [string]$VisualStudioPath = '',
    [ValidateSet('Auto', 'Ci', 'Install', 'Skip')]
    [string]$FrontendDependencies = 'Auto'
)

$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repo 'build'
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null

$vs = $VisualStudioPath
if ([string]::IsNullOrWhiteSpace($vs)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'vswhere.exe not found. Install Visual Studio with Desktop development with C++.'
    }
    $vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if ([string]::IsNullOrWhiteSpace($vs)) {
    throw 'MSVC x64 toolset not found.'
}

$pathValue = (($env:Path -split ';') | Where-Object { $_ } | Select-Object -Unique) -join ';'
foreach ($key in @('PATH', 'Path', 'path')) {
    [Environment]::SetEnvironmentVariable($key, $null, 'Process')
}
$env:Path = $pathValue

if ($Target -eq 'Runtime' -or $Target -eq 'All') {
    $msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
    $toolset = if ([IO.File]::Exists((Join-Path $vs 'MSBuild\Microsoft\VC\v180\Platforms\x64\PlatformToolsets\v145\Toolset.props'))) { 'v145' } else { 'v143' }
    $nativeOutput = Join-Path $OutputRoot 'native'
    $nativeObjects = Join-Path $OutputRoot 'obj\native'
    New-Item -ItemType Directory -Path $nativeOutput, $nativeObjects -Force | Out-Null
    & $msbuild (Join-Path $repo 'VideoExporter.vcxproj') '/p:Configuration=Release' '/p:Platform=x64' "/p:PlatformToolset=$toolset" `
        "/p:OutDir=$nativeOutput/" "/p:IntDir=$nativeObjects/" '/nologo' '/verbosity:minimal'
    if ($LASTEXITCODE -ne 0) {
        throw ('VideoExporter runtime build failed: ' + $LASTEXITCODE)
    }
}

if ($Target -eq 'Frontend' -or $Target -eq 'All') {
    $resolvedDeps = $FrontendDependencies
    if ($resolvedDeps -eq 'Auto') {
        $viteBin = Join-Path $repo 'frontend\node_modules\.bin\vite.cmd'
        $resolvedDeps = if (Test-Path -LiteralPath $viteBin -PathType Leaf) { 'Skip' } else { 'Ci' }
    }
    $uiStageDir = Join-Path $OutputRoot 'native\VideoExporter.UI'
    $global:LASTEXITCODE = 0
    & (Join-Path $repo 'frontend\build.ps1') -Dependencies $resolvedDeps -StageDir $uiStageDir -AllowedStageRoot $OutputRoot
    if ($LASTEXITCODE -ne 0) {
        throw ('VideoExporter frontend build failed: ' + $LASTEXITCODE)
    }
}

if ($Target -eq 'Tests' -or $Target -eq 'All') {
    $global:LASTEXITCODE = 0
    & (Join-Path $repo 'tests\run_tests.ps1') -OutputRoot $OutputRoot -VisualStudioPath $vs -WindowsSdkRoot $WindowsSdkRoot
    if ($LASTEXITCODE -ne 0) {
        throw ('VideoExporter tests failed: ' + $LASTEXITCODE)
    }
}
