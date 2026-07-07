param(
    [string]$Python = 'auto',

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',

    [string]$RunIdPrefix = 'gate_h_checkpoint_replay',

    [string]$ExternalObservationStream = '',

    [int]$OnlineFrames = 2,

    [int]$PredictionEveryFrames = 2,

    [int]$TargetIterations = 3,

    [int]$InitialIterations = 1,

    [switch]$SkipBuild
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
$workspaceRoot = [System.IO.Path]::GetFullPath((Join-Path $runtimeRoot '..'))
$objectRoot = Join-Path $workspaceRoot 'flightenv-object-reentry-vehicle'
$pdkRoot = Join-Path $workspaceRoot 'flightenv-platform-pdk'
Import-Module (Join-Path $pdkRoot 'tools\PdkPython.psm1') -Force
$Python = Resolve-PdkPython -Python $Python -PdkRoot $pdkRoot
$compiledRoot = Join-Path $workspaceRoot '_local_artifacts\platform-pdk\compiled-workflows'
$compiledOnline = Join-Path $compiledRoot 'reentry.online_filtering_external_input.v1'
$compiledFuture = Join-Path $compiledRoot 'reentry.posterior_future_prediction.v1'
$adapterRegistry = Join-Path $objectRoot 'tools\adapter_registries\ballistic_adapters.local.json'
$exe = Join-Path $workspaceRoot "_deps\workspace\$Platform\$Configuration\FlightEnvPlatformRuntimeHost.exe"
$runRoot = Join-Path $workspaceRoot '_local_artifacts\platform-runtime\runtime-host-runs'
$reportRoot = Join-Path $workspaceRoot '_local_artifacts\platform-runtime\scheduler-acceptance'
$reportPath = Join-Path $reportRoot 'checkpoint_replay_resume_audit.json'

if ([string]::IsNullOrWhiteSpace($ExternalObservationStream)) {
    $ExternalObservationStream = Join-Path $objectRoot 'fixtures\sensor_stream_db70_real_db.json'
    if (-not (Test-Path -LiteralPath $ExternalObservationStream -PathType Leaf)) {
        $ExternalObservationStream = Join-Path $objectRoot 'fixtures\sensor_stream_db70.json'
    }
}
$ExternalObservationStream = [System.IO.Path]::GetFullPath($ExternalObservationStream)

if ($TargetIterations -le $InitialIterations) {
    throw 'TargetIterations must be greater than InitialIterations for a resume audit.'
}
if (-not (Test-Path -LiteralPath $ExternalObservationStream -PathType Leaf)) {
    throw "External observation stream not found: $ExternalObservationStream"
}

function Read-JsonFile {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "JSON file not found: $Path"
    }
    return Get-Content -LiteralPath $Path -Encoding UTF8 -Raw | ConvertFrom-Json
}

function ConvertTo-StableJson {
    param([object]$Value)
    return ($Value | ConvertTo-Json -Depth 80 -Compress)
}

function Get-Sha256Text {
    param([string]$Text)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        return (($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join '')
    } finally {
        $sha.Dispose()
    }
}

function Select-OrderedFields {
    param(
        [object[]]$Items,
        [string[]]$Fields
    )
    $result = @()
    foreach ($item in $Items) {
        $row = [ordered]@{}
        foreach ($field in $Fields) {
            $prop = $item.PSObject.Properties[$field]
            if ($null -ne $prop) {
                $row[$field] = $prop.Value
            }
        }
        $result += [pscustomobject]$row
    }
    return @($result)
}

function Get-BranchProjection {
    param(
        [object]$Timeline,
        [string]$BranchId
    )
    $steps = @($Timeline.branch_steps | Where-Object { [string]$_.branch_id -eq $BranchId } |
        Sort-Object @{ Expression = { [int]$_.step_index } }, @{ Expression = { [double]$_.public_time_s } })
    $artifacts = @($Timeline.artifact_refs | Where-Object { [string]$_.branch_id -eq $BranchId } |
        Sort-Object step_index, node_id, port_id, field_name, contract_id)
    $qois = @($Timeline.qoi_refs | Where-Object { [string]$_.branch_id -eq $BranchId } |
        Sort-Object step_index, node_id, port_id, contract_id)
    $checkpoints = @($Timeline.checkpoint_refs | Where-Object { [string]$_.branch_id -eq $BranchId } |
        Sort-Object step_index, checkpoint_index)

    return [ordered]@{
        branch_id = $BranchId
        steps = Select-OrderedFields $steps @(
            'step_index', 'branch_relative_time_s', 'public_time_s', 'trigger_time_s',
            'mainline_frame_index', 'status', 'stop_reason'
        )
        artifacts = Select-OrderedFields $artifacts @(
            'step_index', 'public_time_s', 'node_id', 'port_id', 'contract_id',
            'field_name', 'representation', 'shape', 'node_count'
        )
        qois = Select-OrderedFields $qois @(
            'step_index', 'public_time_s', 'node_id', 'port_id', 'contract_id',
            'qoi_id', 'value_kind', 'scalar_value'
        )
        checkpoints = Select-OrderedFields $checkpoints @(
            'step_index', 'checkpoint_index', 'public_time_s'
        )
    }
}

function Invoke-Smoke {
    param(
        [string]$Prefix,
        [int]$FutureIterations,
        [switch]$NoBuild
    )
    $args = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', (Join-Path $runtimeRoot 'tools\run_cpp_runtime_host_smoke.ps1'),
        '-Python', $Python,
        '-Configuration', $Configuration,
        '-Platform', $Platform,
        '-RunIdPrefix', $Prefix,
        '-OnlineFrames', "$OnlineFrames",
        '-PredictionEveryFrames', "$PredictionEveryFrames",
        '-FutureMaxIterations', "$FutureIterations",
        '-BranchChunkIterations', '1',
        '-ExternalObservationStream', $ExternalObservationStream
    )
    if ($NoBuild) {
        $args += '-SkipBuild'
    }
    & powershell @args | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
        throw "Runtime host smoke failed for prefix $Prefix"
    }
    $chainDir = Join-Path $workspaceRoot "_local_artifacts\platform-runtime\mainline-runs\$Prefix"
    $timelinePath = Join-Path $chainDir 'run_timeline_index.json'
    $timeline = Read-JsonFile $timelinePath
    $predictionRun = @($timeline.prediction_runs | Select-Object -First 1)
    if ($predictionRun.Count -eq 0) {
        throw "No prediction run found for prefix $Prefix"
    }
    return [pscustomobject]@{
        prefix = $Prefix
        chain_dir = $chainDir
        timeline_path = $timelinePath
        timeline = $timeline
        prediction_run = $predictionRun[0]
    }
}

New-Item -ItemType Directory -Force -Path $reportRoot | Out-Null

$baselinePrefix = "$RunIdPrefix.baseline"
$resumePrefix = "$RunIdPrefix.resume"
$baseline = Invoke-Smoke -Prefix $baselinePrefix -FutureIterations $TargetIterations -NoBuild:$SkipBuild
$resumeSeed = Invoke-Smoke -Prefix $resumePrefix -FutureIterations $InitialIterations -NoBuild

if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Runtime Host executable not found: $exe"
}

$run = $resumeSeed.prediction_run
$branchId = [string]$run.branch_id
$branchRunId = [string]$run.run_id
$branchRunDir = [string]$run.run_dir
$seedRuntimeOutputs = [string]$run.seed_runtime_outputs
$triggerFrameIndex = [int]$run.trigger_frame_index
$triggerTimeS = [double]$run.trigger_time_s
$triggerEventId = [string]$run.trigger_event_id

$workerArgs = @(
    '--workspace-root', $workspaceRoot,
    '--pdk-root', $pdkRoot,
    '--object-package-root', $objectRoot,
    '--compiled-online', $compiledOnline,
    '--compiled-future', $compiledFuture,
    '--adapter-registry', $adapterRegistry,
    '--run-id-prefix', $resumePrefix,
    '--run-root', $runRoot,
    '--chain-dir', $resumeSeed.chain_dir,
    '--python', $Python,
    '--execution-backend', 'native_adapter_sessions',
    '--zero-copy-mode', 'auto',
    '--typed-buffer-persistence', 'shadow_artifact',
    '--future-max-iterations', "$TargetIterations",
    '--branch-chunk-iterations', '1',
    '--branch-worker',
    '--resume-existing-branch',
    '--branch-id', $branchId,
    '--branch-run-id', $branchRunId,
    '--branch-run-dir', $branchRunDir,
    '--seed-runtime-outputs', $seedRuntimeOutputs,
    '--trigger-frame-index', "$triggerFrameIndex",
    '--trigger-time-s', "$triggerTimeS",
    '--trigger-event-id', $triggerEventId
)

Push-Location $workspaceRoot
try {
    & $exe @workerArgs
} finally {
    Pop-Location
}
if ($LASTEXITCODE -ne 0) {
    throw "Branch resume worker failed with exit code $LASTEXITCODE"
}

$baselineTimeline = $baseline.timeline
$restoredTimeline = Read-JsonFile $resumeSeed.timeline_path
$baselineBranchId = [string]$baseline.prediction_run.branch_id
$restoredBranchId = $branchId
$baselineProjection = Get-BranchProjection -Timeline $baselineTimeline -BranchId $baselineBranchId
$restoredProjection = Get-BranchProjection -Timeline $restoredTimeline -BranchId $restoredBranchId
$baselineHash = Get-Sha256Text (ConvertTo-StableJson $baselineProjection)
$restoredHash = Get-Sha256Text (ConvertTo-StableJson $restoredProjection)

$statePath = Join-Path (Join-Path $resumeSeed.chain_dir 'branch_manager') "$branchId.json"
$state = Read-JsonFile $statePath
$loop = Read-JsonFile (Join-Path $branchRunDir 'runtime_loop_summary.json')
$checkpoint = Read-JsonFile (Join-Path $branchRunDir 'state_checkpoint.json')

$passed =
    ($baselineHash -eq $restoredHash) -and
    ([int]$state.completed_iterations -eq $TargetIterations) -and
    ([int]$loop.summary.iteration_count -eq $TargetIterations) -and
    ([int]$checkpoint.summary.checkpoint_count -gt 0)

$report = [ordered]@{
    schema_version = 'flightenv.platform.checkpoint_replay_resume_audit.v1'
    result = if ($passed) { 'pass' } else { 'fail' }
    baseline_prefix = $baselinePrefix
    resume_prefix = $resumePrefix
    branch_id = $branchId
    target_iterations = $TargetIterations
    initial_iterations = $InitialIterations
    baseline_hash = $baselineHash
    restored_hash = $restoredHash
    baseline_projection = $baselineProjection
    restored_projection = $restoredProjection
    state_path = $statePath
    branch_run_dir = $branchRunDir
    state_completed_iterations = [int]$state.completed_iterations
    loop_iteration_count = [int]$loop.summary.iteration_count
    checkpoint_count = [int]$checkpoint.summary.checkpoint_count
}
$report | ConvertTo-Json -Depth 90 | Set-Content -LiteralPath $reportPath -Encoding UTF8

if (-not $passed) {
    throw "Checkpoint replay/resume audit failed. Report: $reportPath"
}

Write-Host "Checkpoint replay/resume audit passed. Report: $reportPath"
