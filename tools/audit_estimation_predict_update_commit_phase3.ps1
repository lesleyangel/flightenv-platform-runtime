param(
    [string]$Python = "auto",

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',

    [string]$WorkspaceRoot = '',

    [string]$ObservationStream = '',

    [int]$FrameCount = 20,

    [int]$BatchSize = 128,

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
$projectPath = Join-Path $runtimeRoot 'tests\EstimationServiceAudit\EstimationServiceAudit.vcxproj'
$workspaceModule = Join-Path $workspaceHome 'tools\FlightEnvWorkspaceConfig.psm1'
$phaseRoot = Join-Path $workspaceHome '_local_artifacts\platform-runtime\phase3-estimation-commit'
$compiledRoot = Join-Path $phaseRoot 'compiled-workflows'
$compiledOnline = Join-Path $compiledRoot 'reentry.online_filtering_external_input.v1'
$estimationPlan = Join-Path $compiledOnline 'estimation_plan.json'
$runA = Join-Path $phaseRoot 'run_a'
$runB = Join-Path $phaseRoot 'run_b'
$reportPath = Join-Path $phaseRoot 'phase3_estimation_commit_audit.json'

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

function Read-JsonFile {
    param([Parameter(Mandatory=$true)][string]$PathValue)
    if (-not (Test-Path -LiteralPath $PathValue -PathType Leaf)) {
        throw "JSON file not found: $PathValue"
    }
    return Get-Content -LiteralPath $PathValue -Raw | ConvertFrom-Json
}

function As-Array {
    param($Value)
    if ($null -eq $Value) {
        return @()
    }
    if ($Value -is [System.Array]) {
        return @($Value)
    }
    return @($Value)
}

function Get-Prop {
    param(
        [Parameter(Mandatory=$true)]$ObjectValue,
        [Parameter(Mandatory=$true)][string]$Name
    )
    if ($null -eq $ObjectValue -or $null -eq $ObjectValue.PSObject.Properties[$Name]) {
        return $null
    }
    return $ObjectValue.PSObject.Properties[$Name].Value
}

function Normalize-Phase3Projection {
    param([Parameter(Mandatory=$true)][string]$RunDir)
    $evidence = Read-JsonFile -PathValue (Join-Path $RunDir 'estimation_evidence.json')
    $checkpoint = Read-JsonFile -PathValue (Join-Path $RunDir 'state_checkpoint.json')
    $runtime = Read-JsonFile -PathValue (Join-Path $RunDir 'runtime_evidence.json')
    $filterCommit = Get-Prop -ObjectValue $evidence -Name 'filter_commit'
    $branches = Get-Prop -ObjectValue $evidence -Name 'branches'
    $summary = Get-Prop -ObjectValue $evidence -Name 'summary'
    return [ordered]@{
        method = [string](Get-Prop -ObjectValue $evidence -Name 'method')
        execution_mode = [string](Get-Prop -ObjectValue $evidence -Name 'execution_mode')
        frame_count = [int](Get-Prop -ObjectValue $summary -Name 'frame_count')
        committed_frame_count = [int](Get-Prop -ObjectValue $summary -Name 'committed_frame_count')
        latest_checkpoint = [string](Get-Prop -ObjectValue $summary -Name 'latest_checkpoint')
        latest_commit_id = [string](Get-Prop -ObjectValue $summary -Name 'latest_commit_id')
        frames = @(As-Array (Get-Prop -ObjectValue $evidence -Name 'frames') | ForEach-Object {
            [ordered]@{
                frame_index = [int](Get-Prop -ObjectValue $_ -Name 'frame_index')
                sample_time_s = [double](Get-Prop -ObjectValue $_ -Name 'sample_time_s')
                posterior_checkpoint = [string](Get-Prop -ObjectValue $_ -Name 'posterior_checkpoint')
                posterior_commit_id = [string](Get-Prop -ObjectValue $_ -Name 'posterior_commit_id')
                posterior_committed = [bool](Get-Prop -ObjectValue $_ -Name 'posterior_committed')
                state_mean = @(As-Array (Get-Prop -ObjectValue $_ -Name 'state_mean'))
                covariance_diag = @(As-Array (Get-Prop -ObjectValue $_ -Name 'covariance_diag'))
            }
        })
        commit_log = @(As-Array (Get-Prop -ObjectValue $filterCommit -Name 'commit_log') | ForEach-Object {
            [ordered]@{
                frame_index = [int](Get-Prop -ObjectValue $_ -Name 'frame_index')
                sample_time_s = [double](Get-Prop -ObjectValue $_ -Name 'sample_time_s')
                posterior_checkpoint = [string](Get-Prop -ObjectValue $_ -Name 'posterior_checkpoint')
                posterior_commit_id = [string](Get-Prop -ObjectValue $_ -Name 'posterior_commit_id')
                posterior_committed = [bool](Get-Prop -ObjectValue $_ -Name 'posterior_committed')
            }
        })
        branch_events = @(As-Array (Get-Prop -ObjectValue $branches -Name 'events') | ForEach-Object {
            [ordered]@{
                event_kind = [string](Get-Prop -ObjectValue $_ -Name 'event_kind')
                trigger_frame_index = [int](Get-Prop -ObjectValue $_ -Name 'trigger_frame_index')
                checkpoint_id = [string](Get-Prop -ObjectValue $_ -Name 'checkpoint_id')
                posterior_commit_id = [string](Get-Prop -ObjectValue $_ -Name 'posterior_commit_id')
                seed_checkpoint_committed = [bool](Get-Prop -ObjectValue $_ -Name 'seed_checkpoint_committed')
                source_state_visibility = [string](Get-Prop -ObjectValue $_ -Name 'source_state_visibility')
            }
        })
        checkpoint_entries = @(As-Array (Get-Prop -ObjectValue $checkpoint -Name 'checkpoints') | ForEach-Object {
            [ordered]@{
                checkpoint_id = [string](Get-Prop -ObjectValue $_ -Name 'checkpoint_id')
                commit_id = [string](Get-Prop -ObjectValue $_ -Name 'commit_id')
                committed = [bool](Get-Prop -ObjectValue $_ -Name 'committed')
                frame_index = [int](Get-Prop -ObjectValue $_ -Name 'frame_index')
                sample_time_s = [double](Get-Prop -ObjectValue $_ -Name 'sample_time_s')
                state_mean = @(As-Array (Get-Prop -ObjectValue $_ -Name 'state_mean'))
                covariance_diag = @(As-Array (Get-Prop -ObjectValue $_ -Name 'covariance_diag'))
            }
        })
        runtime_commit_count = [int](Get-Prop -ObjectValue (Get-Prop -ObjectValue $runtime -Name 'filter_commit') -Name 'committed_frame_count')
    }
}

Normalize-ProcessPathEnvironment

if (-not (Test-Path -LiteralPath $projectPath -PathType Leaf)) {
    throw "EstimationServiceAudit project not found: $projectPath"
}
if (-not (Test-Path -LiteralPath $objectRoot -PathType Container)) {
    throw "Object package not found: $objectRoot"
}
if (-not (Test-Path -LiteralPath $pdkRoot -PathType Container)) {
    throw "PDK repo not found: $pdkRoot"
}

if ([string]::IsNullOrWhiteSpace($ObservationStream)) {
    $candidate = Join-Path $objectRoot 'fixtures\sensor_stream_db70_real_db.json'
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        $candidate = Join-Path $objectRoot 'fixtures\sensor_stream_db70.json'
    }
    $ObservationStream = $candidate
}
if (-not (Test-Path -LiteralPath $ObservationStream -PathType Leaf)) {
    throw "Observation stream not found: $ObservationStream"
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

Write-Host 'FlightEnv estimation predict/update/commit Phase3 audit'
Write-Host "  runtime root  = $runtimeRoot"
Write-Host "  project       = $projectPath"
Write-Host "  configuration = $Configuration"
Write-Host "  platform      = $Platform"
Write-Host "  workspace     = $WorkspaceRoot"
Write-Host "  observation   = $ObservationStream"
Write-Host "  frames        = $FrameCount"
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
    throw "MSBuild failed for EstimationServiceAudit with exit code $LASTEXITCODE"
}

$exe = Join-Path $WorkspaceRoot "$Platform\$Configuration\EstimationServiceAudit.exe"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "EstimationServiceAudit executable not found after build: $exe"
}

New-Item -ItemType Directory -Force -Path $phaseRoot | Out-Null
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $objectRoot 'tools\compile_workflows.ps1') `
    -Python $Python `
    -PdkRoot $pdkRoot `
    -Workflow online_filtering_external_input `
    -OutDir $compiledRoot `
    -RunId 'phase3_estimation_commit.compile.online'

if (-not (Test-Path -LiteralPath $estimationPlan -PathType Leaf)) {
    throw "Compiled estimation_plan.json not found: $estimationPlan"
}

foreach ($runDir in @($runA, $runB)) {
    if (Test-Path -LiteralPath $runDir) {
        $resolved = [System.IO.Path]::GetFullPath($runDir)
        if (-not $resolved.StartsWith([System.IO.Path]::GetFullPath($phaseRoot), [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to clean run directory outside phase root: $resolved"
        }
        Remove-Item -LiteralPath $runDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
}

$expectedBranches = [Math]::Floor($FrameCount / 20)
foreach ($runDir in @($runA, $runB)) {
    & $exe `
        --estimation-plan $estimationPlan `
        --observation-stream $ObservationStream `
        --out-dir $runDir `
        --expect-method particle_filter `
        --expect-frames $FrameCount `
        --require-sample-scheduler true `
        --batch-size $BatchSize `
        --expect-branches $expectedBranches
    if ($LASTEXITCODE -ne 0) {
        throw "EstimationServiceAudit failed for $runDir with exit code $LASTEXITCODE"
    }
}

$projectionA = Normalize-Phase3Projection -RunDir $runA
$projectionB = Normalize-Phase3Projection -RunDir $runB
$jsonA = $projectionA | ConvertTo-Json -Depth 80
$jsonB = $projectionB | ConvertTo-Json -Depth 80
if ($jsonA -ne $jsonB) {
    throw 'Phase3 deterministic replay projection mismatch'
}

$report = [ordered]@{
    schema_version = 'flightenv.platform.phase3_estimation_commit_audit.v1'
    generated_at_utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    runtime_root = $runtimeRoot
    workspace_root = $workspaceHome
    estimation_plan = $estimationPlan
    observation_stream = $ObservationStream
    frame_count = $FrameCount
    expected_branch_count = $expectedBranches
    run_a = $runA
    run_b = $runB
    committed_frame_count = $projectionA.committed_frame_count
    runtime_commit_count = $projectionA.runtime_commit_count
    latest_checkpoint = $projectionA.latest_checkpoint
    latest_commit_id = $projectionA.latest_commit_id
    deterministic_replay_projection = $true
}
($report | ConvertTo-Json -Depth 20) | Set-Content -LiteralPath $reportPath -Encoding UTF8

Write-Host '[OK] Phase3 estimation predict/update/commit audit passed.'
Write-Host "  report = $reportPath"
