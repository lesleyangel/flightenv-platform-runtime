param(
    [string]$Python = "auto",

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
$objectRoot = Join-Path $workspaceHome 'flightenv-object-reentry-vehicle'
$pdkRoot = Join-Path $workspaceHome 'flightenv-platform-pdk'
Import-Module (Join-Path $pdkRoot 'tools\PdkPython.psm1') -Force
$Python = Resolve-PdkPython -Python $Python -PdkRoot $pdkRoot
$projectPath = Join-Path $runtimeRoot 'tests\EstimationClosedLoopAudit\EstimationClosedLoopAudit.vcxproj'
$workspaceModule = Join-Path $workspaceHome 'tools\FlightEnvWorkspaceConfig.psm1'
$phaseRoot = Join-Path $workspaceHome '_local_artifacts\platform-runtime\temporal-multirate-phase10'
$compiledRoot = Join-Path $phaseRoot 'compiled-workflows'
$compiledOnline = Join-Path $compiledRoot 'reentry.online_filtering_external_input.v1'
$estimationPlan = Join-Path $compiledOnline 'estimation_plan.json'
$auditOut = Join-Path $phaseRoot 'closed_loop_audit'
$reportPath = Join-Path $auditOut 'phase10_estimation_closed_loop_audit.json'

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

Normalize-ProcessPathEnvironment

if (-not (Test-Path -LiteralPath $projectPath -PathType Leaf)) {
    throw "EstimationClosedLoopAudit project not found: $projectPath"
}
if (-not (Test-Path -LiteralPath $objectRoot -PathType Container)) {
    throw "Object package not found: $objectRoot"
}
if (-not (Test-Path -LiteralPath $pdkRoot -PathType Container)) {
    throw "PDK repo not found: $pdkRoot"
}

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

Write-Host 'FlightEnv estimation closed-loop Phase10 audit'
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
    throw "MSBuild failed for EstimationClosedLoopAudit with exit code $LASTEXITCODE"
}

$exe = Join-Path $WorkspaceRoot "$Platform\$Configuration\EstimationClosedLoopAudit.exe"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "EstimationClosedLoopAudit executable not found after build: $exe"
}

New-Item -ItemType Directory -Force -Path $phaseRoot | Out-Null
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $objectRoot 'tools\compile_workflows.ps1') `
    -Python $Python `
    -PdkRoot $pdkRoot `
    -Workflow online_filtering_external_input `
    -OutDir $compiledRoot `
    -RunId 'phase10_estimation_closed_loop.compile.online'

if (-not (Test-Path -LiteralPath $estimationPlan -PathType Leaf)) {
    throw "Compiled estimation_plan.json not found: $estimationPlan"
}

if (Test-Path -LiteralPath $auditOut) {
    $resolved = [System.IO.Path]::GetFullPath($auditOut)
    if (-not $resolved.StartsWith([System.IO.Path]::GetFullPath($phaseRoot), [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean audit directory outside phase root: $resolved"
    }
    Remove-Item -LiteralPath $auditOut -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $auditOut | Out-Null

& $exe `
    --estimation-plan $estimationPlan `
    --out-root $auditOut

if ($LASTEXITCODE -ne 0) {
    throw "EstimationClosedLoopAudit failed with exit code $LASTEXITCODE"
}

if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
    throw "Phase10 report not found: $reportPath"
}

Write-Host '[OK] Phase10 estimation closed-loop audit passed.'
Write-Host "  report = $reportPath"
