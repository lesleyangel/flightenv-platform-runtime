param(
    [string]$Python = "python",

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',

    [string]$RunIdPrefix = "phase7_due_time_scheduler",

    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

try {
    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
    [Console]::InputEncoding = [System.Text.UTF8Encoding]::new($false)
    $OutputEncoding = [Console]::OutputEncoding
} catch {
    $OutputEncoding = [System.Text.UTF8Encoding]::new($false)
}

function Read-JsonFile {
    param([Parameter(Mandatory=$true)][string]$PathValue)
    if (-not (Test-Path -LiteralPath $PathValue -PathType Leaf)) {
        throw "JSON file not found: $PathValue"
    }
    return Get-Content -LiteralPath $PathValue -Encoding UTF8 -Raw | ConvertFrom-Json
}

function Write-JsonFile {
    param(
        [Parameter(Mandatory=$true)][string]$PathValue,
        [Parameter(Mandatory=$true)]$Value
    )
    $parent = Split-Path -Parent $PathValue
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    $Value | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $PathValue -Encoding UTF8
}

function Assert-True {
    param(
        [Parameter(Mandatory=$true)][bool]$Condition,
        [Parameter(Mandatory=$true)][string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

function Invoke-CheckedPowershell {
    param([Parameter(Mandatory=$true)][string[]]$Arguments)
    $output = & powershell @Arguments 2>&1
    foreach ($line in $output) {
        Write-Host $line
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Child PowerShell command failed with exit code $LASTEXITCODE"
    }
}

function Resolve-ObservationStream {
    $candidates = @(
        (Join-Path $ObjectRoot 'fixtures\sensor_stream_db70_real_db.json'),
        (Join-Path $ObjectRoot 'fixtures\sensor_stream_db70.json'),
        (Join-Path $ObjectRoot 'fixtures\sensor_stream.json')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    throw "No object observation stream fixture found under $ObjectRoot\fixtures"
}

function Find-TraceFile {
    param([Parameter(Mandatory=$true)][string]$RunId)
    $futureRunRoot = Join-Path $WorkspaceRoot '_local_artifacts\platform-runtime\runtime-host-runs\reentry.posterior_future_prediction.v1'
    if (-not (Test-Path -LiteralPath $futureRunRoot -PathType Container)) {
        throw "Runtime future run root not found: $futureRunRoot"
    }
    $files = @(Get-ChildItem -LiteralPath $futureRunRoot -Recurse -Filter scheduler_due_time_trace.json |
        Where-Object { $_.FullName -like "*$RunId*" } |
        Sort-Object LastWriteTimeUtc -Descending)
    if ($files.Count -eq 0) {
        throw "No scheduler_due_time_trace.json found for run id: $RunId"
    }
    return $files[0].FullName
}

function Invoke-SmokeAndAudit {
    param([Parameter(Mandatory=$true)][string]$RunId)

    Invoke-CheckedPowershell -Arguments @(
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        (Join-Path $RuntimeRoot 'tools\run_cpp_runtime_host_smoke.ps1'),
        '-Python',
        $Python,
        '-Configuration',
        $Configuration,
        '-Platform',
        $Platform,
        '-RunIdPrefix',
        $RunId,
        '-OnlineFrames',
        '2',
        '-PredictionEveryFrames',
        '2',
        '-FutureMaxIterations',
        '1',
        '-BranchChunkIterations',
        '1',
        '-ExternalObservationStream',
        $ObservationStream,
        '-SkipBuild'
    )

    $tracePath = Find-TraceFile -RunId $RunId
    $runDir = Split-Path -Parent $tracePath
    $runtimeEvidencePath = Join-Path $runDir 'runtime_evidence.json'
    $trace = Read-JsonFile -PathValue $tracePath
    $runtimeEvidence = Read-JsonFile -PathValue $runtimeEvidencePath
    $summary = $trace.summary
    $events = @($trace.events)
    $dispatchEvents = @($events | Where-Object { [string]$_.event_kind -eq 'dispatch' })
    $heldEvents = @($events | Where-Object { [string]$_.event_kind -eq 'held_not_due' })
    $dispatchPeriods = @($dispatchEvents | ForEach-Object { [double]$_.period_s } | Sort-Object -Unique)
    $slowDispatchEvents = @($dispatchEvents | Where-Object { [double]$_.period_s -gt ([double]$summary.quantum_s + 1.0e-9) })
    $heldDispatchCollisions = @()
    $dispatchKeys = @{}
    foreach ($event in $dispatchEvents) {
        $key = "$($event.tick_index)|$($event.node_id)"
        $dispatchKeys[$key] = $true
    }
    foreach ($event in $heldEvents) {
        $key = "$($event.tick_index)|$($event.node_id)"
        if ($dispatchKeys.ContainsKey($key)) {
            $heldDispatchCollisions += $key
        }
    }

    Assert-True ($trace.schema_version -eq 'flightenv.platform.scheduler_due_time_trace.v1') "bad due-time trace schema for $RunId"
    Assert-True ([bool]$summary.deterministic_single_thread) "due-time trace must be deterministic single-thread for $RunId"
    Assert-True ([int]$summary.dispatch_event_count -gt 0) "due-time trace has no dispatch events for $RunId"
    Assert-True ([int]$summary.held_not_due_event_count -gt 0) "due-time trace has no held-not-due events for $RunId"
    Assert-True ([int]$summary.slow_node_held_event_count -gt 0) "due-time trace did not hold any slower node for $RunId"
    Assert-True ([int]$summary.distinct_due_time_count -gt 1) "due-time trace did not expand multiple due times for $RunId"
    Assert-True ([int]$summary.not_due_violation_count -eq 0) "due-time trace dispatched a not-due node for $RunId"
    Assert-True ([int]$summary.dependency_violation_count -eq 0) "due-time trace dependency order violation for $RunId"
    Assert-True ($dispatchPeriods.Count -gt 1) "due-time trace did not preserve multiple dispatch periods for $RunId"
    Assert-True ($slowDispatchEvents.Count -gt 0) "due-time trace did not dispatch any slower-than-quantum node for $RunId"
    Assert-True ($heldDispatchCollisions.Count -eq 0) "node was both held and dispatched on the same tick for $RunId"
    Assert-True ($runtimeEvidence.refs.scheduler_due_time_trace -eq 'scheduler_due_time_trace.json') "runtime evidence missing scheduler_due_time_trace ref for $RunId"
    Assert-True ($runtimeEvidence.summary.scheduler_due_time_trace_digest -eq $summary.trace_digest) "runtime evidence trace digest mismatch for $RunId"
    Assert-True ([int]$runtimeEvidence.summary.scheduler_due_time_trace_not_due_violation_count -eq 0) "runtime evidence reports not-due violation for $RunId"
    Assert-True ([int]$runtimeEvidence.summary.scheduler_due_time_trace_dependency_violation_count -eq 0) "runtime evidence reports dependency violation for $RunId"

    return [pscustomobject][ordered]@{
        run_id = $RunId
        run_dir = $runDir
        trace_path = $tracePath
        runtime_evidence = $runtimeEvidencePath
        trace_digest = [string]$summary.trace_digest
        dispatch_event_count = [int]$summary.dispatch_event_count
        held_not_due_event_count = [int]$summary.held_not_due_event_count
        slow_node_held_event_count = [int]$summary.slow_node_held_event_count
        distinct_due_time_count = [int]$summary.distinct_due_time_count
        dispatch_periods_s = $dispatchPeriods
    }
}

$RuntimeRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$WorkspaceRoot = [System.IO.Path]::GetFullPath((Join-Path $RuntimeRoot '..'))
$ObjectRoot = Join-Path $WorkspaceRoot 'flightenv-object-reentry-vehicle'
$ReportRoot = Join-Path $WorkspaceRoot '_local_artifacts\platform-runtime\temporal-multirate-phase7'
$ReportPath = Join-Path $ReportRoot 'phase7_due_time_scheduler_audit.json'
$ObservationStream = Resolve-ObservationStream

New-Item -ItemType Directory -Force -Path $ReportRoot | Out-Null

if (-not $SkipBuild) {
    Invoke-CheckedPowershell -Arguments @(
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        (Join-Path $RuntimeRoot 'tools\build_platform_runtime.ps1'),
        '-Configuration',
        $Configuration,
        '-Platform',
        $Platform
    )
}

$auditA = Invoke-SmokeAndAudit -RunId "$RunIdPrefix.a"
$auditB = Invoke-SmokeAndAudit -RunId "$RunIdPrefix.b"
Assert-True ([string]$auditA.trace_digest -eq [string]$auditB.trace_digest) "due-time trace digest is not reproducible across identical inputs"

$report = [ordered]@{
    schema_version = 'flightenv.temporal_multirate.phase7_due_time_scheduler_audit.v1'
    status = 'passed'
    generated_at_utc = [DateTime]::UtcNow.ToString('o')
    run_id_prefix = $RunIdPrefix
    configuration = $Configuration
    platform = $Platform
    observation_stream = $ObservationStream
    audits = @($auditA, $auditB)
    acceptance = [ordered]@{
        not_due_nodes_do_not_execute = $true
        slow_nodes_are_held_on_fast_ticks = $true
        repeated_input_trace_digest_is_stable = $true
        runtime_evidence_refs_due_time_trace = $true
    }
}

Write-JsonFile -PathValue $ReportPath -Value $report

Write-Host "Temporal multirate Phase7 due-time scheduler audit passed."
Write-Host "  report = $ReportPath"
Write-Host "  trace_digest = $($auditA.trace_digest)"
