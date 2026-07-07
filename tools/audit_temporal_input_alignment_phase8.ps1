param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',

    [string]$WorkspaceRoot = '',

    [switch]$VerboseMSBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
try {
    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
    [Console]::InputEncoding = [System.Text.UTF8Encoding]::new($false)
    $OutputEncoding = [Console]::OutputEncoding
} catch {
    $OutputEncoding = [System.Text.UTF8Encoding]::new($false)
}

$runtimeRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspaceHome = [System.IO.Path]::GetFullPath((Join-Path $runtimeRoot '..'))
$projectPath = Join-Path $runtimeRoot 'tests\TemporalInputAlignmentAudit\TemporalInputAlignmentAudit.vcxproj'
$workspaceModule = Join-Path $workspaceHome 'tools\FlightEnvWorkspaceConfig.psm1'
$reportRoot = Join-Path $workspaceHome '_local_artifacts\platform-runtime\temporal-multirate-phase8'
$reportPath = Join-Path $reportRoot 'phase8_temporal_input_alignment_audit.json'

function Resolve-MSBuild {
    if ($env:MSBUILD_EXE -and (Test-Path -LiteralPath $env:MSBUILD_EXE -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $env:MSBUILD_EXE).Path
    }
    $vswhereCandidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe"
    )
    foreach ($vswhere in $vswhereCandidates) {
        if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
            continue
        }
        $candidate = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return $candidate
        }
    }
    $pathCandidate = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if ($pathCandidate -and (Test-Path -LiteralPath $pathCandidate.Source -PathType Leaf)) {
        return $pathCandidate.Source
    }
    throw 'MSBuild not found. Install Visual Studio Build Tools, add MSBuild.exe to PATH, or set MSBUILD_EXE.'
}

function Normalize-ProcessPathEnvironment {
    $processEnvironment = [Environment]::GetEnvironmentVariables('Process')
    $pathValue = $processEnvironment['Path']
    if ([string]::IsNullOrEmpty($pathValue)) {
        $pathValue = $processEnvironment['PATH']
    }
    if (-not [string]::IsNullOrEmpty($pathValue)) {
        [Environment]::SetEnvironmentVariable('PATH', $null, 'Process')
        [Environment]::SetEnvironmentVariable('Path', $pathValue, 'Process')
    }
}

if (-not (Test-Path -LiteralPath $projectPath -PathType Leaf)) {
    throw "TemporalInputAlignment audit project not found: $projectPath"
}

Normalize-ProcessPathEnvironment

if (Test-Path -LiteralPath $workspaceModule -PathType Leaf) {
    Import-Module $workspaceModule -Force
    $config = Get-FlightEnvWorkspaceConfig `
        -Root $workspaceHome `
        -Configuration $Configuration `
        -Platform $Platform `
        -WorkspaceRoot $WorkspaceRoot
    $Configuration = $config.Configuration
    $Platform = $config.Platform
    $WorkspaceRoot = $config.WorkspaceRoot
    Set-FlightEnvWorkspaceEnvironment -Config $config
} else {
    if ([string]::IsNullOrWhiteSpace($WorkspaceRoot)) {
        $WorkspaceRoot = Join-Path $workspaceHome '_deps\workspace'
    }
    $WorkspaceRoot = [System.IO.Path]::GetFullPath($WorkspaceRoot)
    $env:FLIGHTENV_DEPS_WORKSPACE_ROOT = $WorkspaceRoot
}

$msbuild = Resolve-MSBuild
$verbosity = if ($VerboseMSBuild) { 'normal' } else { 'minimal' }

Write-Host 'FlightEnv temporal input alignment Phase8 audit'
Write-Host "  runtime root  = $runtimeRoot"
Write-Host "  project       = $projectPath"
Write-Host "  configuration = $Configuration"
Write-Host "  platform      = $Platform"
Write-Host "  workspace     = $WorkspaceRoot"
Write-Host "  MSBuild       = $msbuild"

& $msbuild $projectPath `
    /m `
    "/p:Configuration=$Configuration" `
    "/p:Platform=$Platform" `
    "/p:FlightEnvRepoRoot=$runtimeRoot\" `
    "/p:FlightEnvWorkspaceHome=$workspaceHome\" `
    "/p:FlightEnvDepsWorkspaceRoot=$WorkspaceRoot" `
    "/v:$verbosity"

if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed for TemporalInputAlignmentAudit with exit code $LASTEXITCODE"
}

$exe = Join-Path $WorkspaceRoot "$Platform\$Configuration\TemporalInputAlignmentAudit.exe"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "TemporalInputAlignmentAudit executable not found after build: $exe"
}

New-Item -ItemType Directory -Force -Path $reportRoot | Out-Null
& $exe $reportPath
if ($LASTEXITCODE -ne 0) {
    throw "TemporalInputAlignmentAudit failed with exit code $LASTEXITCODE. Report: $reportPath"
}

$report = Get-Content -LiteralPath $reportPath -Encoding UTF8 -Raw | ConvertFrom-Json
if ($report.status -ne 'passed') {
    throw "TemporalInputAlignmentAudit report did not pass: $reportPath"
}
if ([int]$report.summary.failure_count -ne 0) {
    throw "TemporalInputAlignmentAudit reported failures: $($report.summary.failure_count)"
}

Write-Host "Temporal input alignment Phase8 audit passed. Report: $reportPath"
