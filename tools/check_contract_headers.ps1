#Requires -Version 5.1
# SPDX-License-Identifier: MIT
# NOTE: Keep this script ASCII-only (Windows PowerShell 5.1 ANSI parsing).
#
# tools/check_contract_headers.ps1 -- verify SHA256 parity of cross-repo C/C++
# contract headers against sibling checkouts when present.
[CmdletBinding()]
param(
    [string]$RepoRoot = ''
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
}
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
$localInclude = Join-Path $RepoRoot 'include'

$groups = @(
    @{
        Name = 'GakuLoader'
        CandidateIncludes = @('..\..\GakuLoader\main\include', '..\..\GakuLoader\dev\include')
        Files = @(
            'GakuPlugin.h',
            'GakuPluginMetadata.h',
            'GakuPaths.h',
            'GakuPaths.hpp',
            'GakuWebPanel.h'
        )
    },
    @{
        Name = 'GakuMotion'
        CandidateIncludes = @('..\..\GakuMotion\dev\include', '..\..\GakuMotion\main\include')
        Files = @(
            'GakuMotionClock.h',
            'GakuMotionStageState.h'
        )
    },
    @{
        Name = 'ScreenshotManager'
        CandidateIncludes = @('..\..\ScreenshotManager\main\include', '..\..\ScreenshotManager\dev\include')
        Files = @(
            'GakuScreenshot.h'
        )
    },
    @{
        Name = 'GakuAudio'
        CandidateIncludes = @('..\..\GakuAudio\main\include', '..\..\GakuAudio\dev\include')
        Files = @(
            'GakuAudio.h'
        )
    }
)

$failures = 0
$checked = 0
$skippedGroups = 0

function Get-NormalizedHeaderHash([string]$Path) {
    $text = [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8) -replace "`r`n", "`n"
    $bytes = [Text.Encoding]::UTF8.GetBytes($text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '')
    } finally {
        $sha.Dispose()
    }
}

foreach ($group in $groups) {
    $siblingInclude = ''
    foreach ($candidate in $group.CandidateIncludes) {
        $resolved = [IO.Path]::GetFullPath((Join-Path $RepoRoot $candidate))
        if (Test-Path -LiteralPath $resolved -PathType Container) {
            $siblingInclude = $resolved
            break
        }
    }
    if (-not $siblingInclude) {
        $fallbackDisplay = [IO.Path]::GetFullPath((Join-Path $RepoRoot $group.CandidateIncludes[0]))
        Write-Host ("  SKIP: {0} checkout not found at {1}" -f $group.Name, $fallbackDisplay) -ForegroundColor Yellow
        $skippedGroups++
        foreach ($file in $group.Files) {
            $localFile = Join-Path $localInclude $file
            if (-not (Test-Path -LiteralPath $localFile -PathType Leaf)) {
                Write-Host ("  MISSING (VideoExporter): include\{0}" -f $file) -ForegroundColor Red
                $failures++
            }
        }
        continue
    }

    foreach ($file in $group.Files) {
        $localFile = Join-Path $localInclude $file
        $siblingFile = Join-Path $siblingInclude $file
        if (-not (Test-Path -LiteralPath $localFile -PathType Leaf)) {
            Write-Host ("  MISSING (VideoExporter): include\{0}" -f $file) -ForegroundColor Red
            $failures++
            continue
        }
        if (-not (Test-Path -LiteralPath $siblingFile -PathType Leaf)) {
            Write-Host ("  MISSING ({0}): {1}" -f $group.Name, $siblingFile) -ForegroundColor Red
            $failures++
            continue
        }

        $localHash = Get-NormalizedHeaderHash $localFile
        $siblingHash = Get-NormalizedHeaderHash $siblingFile
        $checked++
        if ($localHash -ne $siblingHash) {
            Write-Host ("  DRIFT: {0} ({1})" -f $file, $group.Name) -ForegroundColor Red
            Write-Host ("    VideoExporter : {0}" -f $localHash)
            Write-Host ("    {0,-13} : {1}" -f $group.Name, $siblingHash)
            $failures++
            continue
        }
        Write-Host ("  OK: {0} ({1}: {2})" -f $file, $group.Name, $localHash.Substring(0, 16)) -ForegroundColor Green
    }
}

if ($failures -ne 0) {
    Write-Host ("Contract header parity check FAILED ({0} issue(s))." -f $failures) -ForegroundColor Red
    exit 1
}

Write-Host ("Contract header parity passed ({0} file(s) compared, {1} absent sibling group(s) skipped)." -f $checked, $skippedGroups)
exit 0
