param(
    [string]$Python = "auto",

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',

    [string]$RunIdPrefix = "phase11_simulink_like",

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

function Get-Prop {
    param(
        [object]$Object,
        [string]$Name,
        [object]$Fallback = $null
    )
    if ($null -eq $Object -or $null -eq $Object.PSObject.Properties[$Name]) {
        return $Fallback
    }
    return $Object.$Name
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

function Find-DiagnosticsFile {
    param([Parameter(Mandatory=$true)][string]$RunId)
    $futureRunRoot = Join-Path $WorkspaceRoot '_local_artifacts\platform-runtime\runtime-host-runs\reentry.posterior_future_prediction.v1'
    if (-not (Test-Path -LiteralPath $futureRunRoot -PathType Container)) {
        throw "Runtime future run root not found: $futureRunRoot"
    }
    $files = @(Get-ChildItem -LiteralPath $futureRunRoot -Recurse -Filter scheduler_diagnostics.json |
        Where-Object { $_.FullName -like "*$RunId*" } |
        Sort-Object LastWriteTimeUtc -Descending)
    if ($files.Count -eq 0) {
        throw "No scheduler_diagnostics.json found for run id: $RunId"
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

    $diagnosticsPath = Find-DiagnosticsFile -RunId $RunId
    $runDir = Split-Path -Parent $diagnosticsPath
    $runtimeEvidencePath = Join-Path $runDir 'runtime_evidence.json'
    $diagnostics = Read-JsonFile -PathValue $diagnosticsPath
    $runtimeEvidence = Read-JsonFile -PathValue $runtimeEvidencePath

    $summary = $diagnostics.summary
    $rateGraph = $diagnostics.rate_graph.summary
    $multitasking = $diagnostics.deterministic_multitasking.summary
    $timing = $diagnostics.deadline_overrun_jitter.summary
    $runtimeSummary = $runtimeEvidence.summary

    Assert-True ($diagnostics.schema_version -eq 'flightenv.platform.scheduler_diagnostics.v1') "bad scheduler diagnostics schema for $RunId"
    Assert-True ([int](Get-Prop $summary 'node_count' 0) -gt 0) "scheduler diagnostics has no nodes for $RunId"
    Assert-True ([int](Get-Prop $rateGraph 'node_count' 0) -gt 0) "rate graph has no nodes for $RunId"
    Assert-True ([int](Get-Prop $rateGraph 'edge_count' 0) -gt 0) "rate graph has no edges for $RunId"
    Assert-True ([int](Get-Prop $rateGraph 'transition_edge_count' 0) -gt 0) "rate graph has no transition edges for $RunId"
    Assert-True ([int](Get-Prop $rateGraph 'cross_rate_edge_count' 0) -gt 0) "rate graph has no cross-rate edges for $RunId"
    Assert-True ([int](Get-Prop $rateGraph 'distinct_period_count' 0) -gt 1) "rate graph did not preserve multiple periods for $RunId"
    Assert-True ([bool](Get-Prop $multitasking 'enabled' $false)) "deterministic multitasking plan is disabled for $RunId"
    Assert-True ([int](Get-Prop $multitasking 'worker_count' 0) -gt 1) "deterministic multitasking worker count is not > 1 for $RunId"
    Assert-True ([int](Get-Prop $multitasking 'batch_count' 0) -gt 0) "deterministic multitasking has no batches for $RunId"
    Assert-True ([int](Get-Prop $multitasking 'parallelizable_batch_count' 0) -gt 0) "deterministic multitasking has no parallelizable batches for $RunId"
    Assert-True ([int](Get-Prop $multitasking 'dependency_violation_count' 0) -eq 0) "deterministic multitasking dependency violation for $RunId"
    Assert-True ([int](Get-Prop $multitasking 'resource_conflict_count' 0) -eq 0) "deterministic multitasking resource conflict for $RunId"
    Assert-True ([bool](Get-Prop $multitasking 'deterministic_order' $false)) "deterministic multitasking order is not deterministic for $RunId"
    Assert-True ([int](Get-Prop $timing 'deadline_check_event_count' 0) -gt 0) "deadline timing has no checks for $RunId"
    Assert-True ([int](Get-Prop $timing 'deadline_miss_count' 0) -eq 0) "deadline miss observed for $RunId"
    Assert-True ([int](Get-Prop $timing 'overrun_count' 0) -eq 0) "overrun observed for $RunId"
    Assert-True ([int](Get-Prop $timing 'jitter_violation_count' 0) -eq 0) "jitter violation observed for $RunId"
    Assert-True ([double](Get-Prop $timing 'max_abs_jitter_s' 0.0) -le 1.0e-7) "jitter exceeds tolerance for $RunId"
    Assert-True ($runtimeEvidence.refs.scheduler_diagnostics -eq 'scheduler_diagnostics.json') "runtime evidence missing scheduler_diagnostics ref for $RunId"
    Assert-True ($runtimeSummary.scheduler_diagnostics_digest -eq $summary.diagnostics_digest) "runtime evidence diagnostics digest mismatch for $RunId"
    Assert-True ([int]$runtimeSummary.rate_graph_cross_rate_edge_count -eq [int]$rateGraph.cross_rate_edge_count) "runtime evidence rate graph count mismatch for $RunId"
    Assert-True ([int]$runtimeSummary.deadline_miss_count -eq 0) "runtime evidence deadline miss count is nonzero for $RunId"
    Assert-True ([int]$runtimeSummary.overrun_count -eq 0) "runtime evidence overrun count is nonzero for $RunId"
    Assert-True ([int]$runtimeSummary.jitter_violation_count -eq 0) "runtime evidence jitter violation count is nonzero for $RunId"
    Assert-True ([int](Get-Prop $runtimeSummary 'node_due_dropped_count' 0) -eq 0) "runtime evidence node_due dropped count is nonzero for $RunId"

    return [pscustomobject][ordered]@{
        run_id = $RunId
        run_dir = $runDir
        diagnostics_path = $diagnosticsPath
        runtime_evidence = $runtimeEvidencePath
        diagnostics_digest = [string]$summary.diagnostics_digest
        rate_graph_digest = [string]$rateGraph.graph_digest
        multitasking_digest = [string]$multitasking.multitasking_digest
        timing_digest = [string]$timing.timing_digest
        node_count = [int]$summary.node_count
        edge_count = [int]$summary.rate_graph_edge_count
        cross_rate_edge_count = [int]$summary.cross_rate_edge_count
        batch_count = [int]$summary.multitasking_batch_count
        parallelizable_batch_count = [int]$summary.parallelizable_batch_count
        deadline_check_event_count = [int]$summary.deadline_check_event_count
        deadline_miss_count = [int]$summary.deadline_miss_count
        overrun_count = [int]$summary.overrun_count
        jitter_violation_count = [int]$summary.jitter_violation_count
        input_alignment_blocked_count = [int](Get-Prop $runtimeSummary 'input_alignment_blocked_count' 0)
        ready_queue_rejected_count = [int](Get-Prop $runtimeSummary 'ready_queue_rejected_count' 0)
        node_due_retry_scheduled_count = [int](Get-Prop $runtimeSummary 'node_due_retry_scheduled_count' 0)
        node_due_dropped_count = [int](Get-Prop $runtimeSummary 'node_due_dropped_count' 0)
    }
}

$RuntimeRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$WorkspaceRoot = [System.IO.Path]::GetFullPath((Join-Path $RuntimeRoot '..'))
$ObjectRoot = Join-Path $WorkspaceRoot 'flightenv-object-reentry-vehicle'
$PdkRoot = Join-Path $WorkspaceRoot 'flightenv-platform-pdk'
Import-Module (Join-Path $PdkRoot 'tools\PdkPython.psm1') -Force
$Python = Resolve-PdkPython -Python $Python -PdkRoot $PdkRoot
$ReportRoot = Join-Path $WorkspaceRoot '_local_artifacts\platform-runtime\temporal-multirate-phase11'
$ReportPath = Join-Path $ReportRoot 'phase11_simulink_like_audit.json'
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
Assert-True ([string]$auditA.diagnostics_digest -eq [string]$auditB.diagnostics_digest) "scheduler diagnostics digest is not reproducible"
Assert-True ([string]$auditA.rate_graph_digest -eq [string]$auditB.rate_graph_digest) "rate graph digest is not reproducible"
Assert-True ([string]$auditA.multitasking_digest -eq [string]$auditB.multitasking_digest) "multitasking digest is not reproducible"
Assert-True ([string]$auditA.timing_digest -eq [string]$auditB.timing_digest) "deadline/overrun/jitter digest is not reproducible"

$report = [ordered]@{
    schema_version = 'flightenv.temporal_multirate.phase11_simulink_like_audit.v1'
    status = 'passed'
    generated_at_utc = [DateTime]::UtcNow.ToString('o')
    run_id_prefix = $RunIdPrefix
    configuration = $Configuration
    platform = $Platform
    observation_stream = $ObservationStream
    audits = @($auditA, $auditB)
    acceptance = [ordered]@{
        deterministic_multitasking_plan_present = $true
        rate_graph_present = $true
        deadline_overrun_jitter_evidence_present = $true
        digest_reproducible = $true
        final_simulink_like_scheduler_gate = $true
    }
}

Write-JsonFile -PathValue $ReportPath -Value $report

Write-Host "Temporal multirate Phase11 Simulink-like audit passed."
Write-Host "  report = $ReportPath"
Write-Host "  diagnostics_digest = $($auditA.diagnostics_digest)"
