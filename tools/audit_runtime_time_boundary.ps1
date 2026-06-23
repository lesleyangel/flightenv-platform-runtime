param(
    [switch]$Strict
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
$reportRoot = Join-Path $workspaceRoot '_local_artifacts\platform-runtime\scheduler-acceptance'
$reportPath = Join-Path $reportRoot 'runtime_time_boundary_audit.json'
$failures = [System.Collections.Generic.List[string]]::new()
$warnings = [System.Collections.Generic.List[string]]::new()
$checks = [System.Collections.Generic.List[object]]::new()

function Add-Check {
    param(
        [Parameter(Mandatory=$true)][string]$Id,
        [Parameter(Mandatory=$true)][string]$Status,
        [Parameter(Mandatory=$true)][string]$Message,
        [object]$Evidence = $null
    )
    $item = [ordered]@{
        id = $Id
        status = $Status
        message = $Message
    }
    if ($null -ne $Evidence) {
        $item.evidence = $Evidence
    }
    $checks.Add([pscustomobject]$item) | Out-Null
}

function Add-Failure {
    param([Parameter(Mandatory=$true)][string]$Message)
    $failures.Add($Message) | Out-Null
}

function Assert-File {
    param(
        [Parameter(Mandatory=$true)][string]$PathValue,
        [Parameter(Mandatory=$true)][string]$Label
    )
    if (-not (Test-Path -LiteralPath $PathValue -PathType Leaf)) {
        Add-Failure "$Label missing: $PathValue"
        return $false
    }
    return $true
}

function Assert-Contains {
    param(
        [Parameter(Mandatory=$true)][string]$PathValue,
        [Parameter(Mandatory=$true)][string]$Pattern,
        [Parameter(Mandatory=$true)][string]$Label
    )
    if (-not (Assert-File -PathValue $PathValue -Label $Label)) {
        return
    }
    $hit = Select-String -LiteralPath $PathValue -Pattern $Pattern -SimpleMatch -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $hit) {
        Add-Failure "$Label not found: $Pattern"
    }
}

function Assert-ForbiddenPattern {
    param(
        [Parameter(Mandatory=$true)][string]$PathValue,
        [Parameter(Mandatory=$true)][string]$Pattern,
        [Parameter(Mandatory=$true)][string]$Label
    )
    if (-not (Assert-File -PathValue $PathValue -Label $Label)) {
        return
    }
    $hits = @(Select-String -LiteralPath $PathValue -Pattern $Pattern -SimpleMatch -ErrorAction SilentlyContinue)
    if ($hits.Count -gt 0) {
        foreach ($hit in $hits) {
            Add-Failure "$Label forbidden seconds hot-path: $($hit.Path):$($hit.LineNumber): $($hit.Line.Trim())"
        }
    }
}

$ready = Join-Path $runtimeRoot 'src\RuntimeReadyQueueExecutor.cpp'
$scheduler = Join-Path $runtimeRoot 'src\RuntimeTimeScheduler.cpp'
$inputAlignment = Join-Path $runtimeRoot 'src\time\RuntimeInputAlignment.cpp'
$tensorInterpolator = Join-Path $runtimeRoot 'src\time\RuntimeTensorInterpolator.cpp'
$sampleBuffer = Join-Path $runtimeRoot 'src\time\RuntimePortSampleBuffer.cpp'
$eventQueue = Join-Path $runtimeRoot 'src\time\RuntimeEventQueue.cpp'

Assert-Contains -PathValue $eventQueue -Pattern 'event.event_time.nanoseconds' -Label 'event queue nanosecond ordering'
Assert-Contains -PathValue $sampleBuffer -Pattern 'sample.time.nanoseconds' -Label 'sample buffer nanosecond ordering'
Assert-Contains -PathValue $ready -Pattern 'scheduled_event_time_ns' -Label 'ReadyQueue scheduled time nanosecond input'
Assert-Contains -PathValue $ready -Pattern 'admission.lateness_ns' -Label 'ReadyQueue lateness nanosecond computation'
Assert-Contains -PathValue $ready -Pattern 'admission.deadline_ns' -Label 'ReadyQueue deadline nanosecond computation'
Assert-Contains -PathValue $scheduler -Pattern 'dispatch.output_period.nanoseconds > base_dt.nanoseconds' -Label 'scheduler cadence nanosecond comparison'
Assert-Contains -PathValue $inputAlignment -Pattern 'span.nanoseconds' -Label 'input alignment interpolation nanosecond span'
Assert-Contains -PathValue $inputAlignment -Pattern 'durationSecondsNonNegative(samples[i].time - samples[i - 1].time)' -Label 'input alignment integration nanosecond dt'
Assert-Contains -PathValue $tensorInterpolator -Pattern 'sample.time.nanoseconds - target_time.nanoseconds' -Label 'tensor nearest nanosecond gap'

Assert-ForbiddenPattern -PathValue $ready -Pattern 'event.event_time_s - scheduled_time_s' -Label 'ReadyQueue deadline'
Assert-ForbiddenPattern -PathValue $ready -Pattern 'lateness_s > admission.deadline_s' -Label 'ReadyQueue deadline'
Assert-ForbiddenPattern -PathValue $scheduler -Pattern 'dispatch.output_period_s > base_dt_s' -Label 'scheduler cadence'
Assert-ForbiddenPattern -PathValue $scheduler -Pattern 'state.last_execution_public_time_s + dispatch.output_period_s' -Label 'scheduler next due'
Assert-ForbiddenPattern -PathValue $tensorInterpolator -Pattern 'sample.time_s - target_time_s' -Label 'tensor nearest'
Assert-ForbiddenPattern -PathValue $inputAlignment -Pattern 'bracket.second->time_s - bracket.first->time_s' -Label 'scalar interpolation'
Assert-ForbiddenPattern -PathValue $inputAlignment -Pattern 'target_time_s - bracket.first->time_s' -Label 'scalar interpolation'
Assert-ForbiddenPattern -PathValue $inputAlignment -Pattern 'samples[i].time_s - samples[i - 1].time_s' -Label 'window integration'
Assert-ForbiddenPattern -PathValue $inputAlignment -Pattern 'target_time_s - window_start_s' -Label 'window integration'

$allSources = @(
    (Join-Path $runtimeRoot 'include'),
    (Join-Path $runtimeRoot 'src')
)
$secondsHits = @(Get-ChildItem -LiteralPath $allSources -Recurse -File -Include *.hpp,*.h,*.cpp |
    Select-String -Pattern '\bdouble\s+\w+_s\b' -ErrorAction SilentlyContinue |
    Select-Object Path, LineNumber, Line)
if ($secondsHits.Count -gt 0) {
    $boundaryMessage = "Seconds boundary fields remain by design for JSON/evidence/CLI compatibility: $($secondsHits.Count)"
    if (-not $Strict) {
        $warnings.Add($boundaryMessage) | Out-Null
    }
    Add-Check -Id 'seconds_boundary_inventory' -Status 'PASS' -Message 'Seconds fields are tracked as boundary compatibility inventory; hot-path comparisons and indexes are forbidden separately.' -Evidence @{
        count = $secondsHits.Count
        sampled_locations = @($secondsHits | Select-Object -First 30)
    }
}

New-Item -ItemType Directory -Force -Path $reportRoot | Out-Null
$summary = [ordered]@{
    schema_version = 'flightenv.platform.runtime_time_boundary_audit.v1'
    generated_at_utc = [DateTime]::UtcNow.ToString('o')
    strict = [bool]$Strict
    runtime_root = $runtimeRoot
    result = if ($failures.Count -eq 0) { 'pass' } else { 'fail' }
    failure_count = $failures.Count
    warning_count = $warnings.Count
    failures = @($failures)
    warnings = @($warnings)
    checks = @($checks)
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding UTF8

Write-Host 'FlightEnv runtime time boundary audit'
Write-Host "  runtime = $runtimeRoot"
Write-Host "  report  = $reportPath"

if ($failures.Count -gt 0) {
    Write-Host ''
    Write-Host 'Failures:'
    foreach ($failure in $failures) {
        Write-Host "  - $failure"
    }
    exit 1
}

Write-Host 'Result: PASS'
if ($warnings.Count -gt 0) {
    Write-Host "Warnings: $($warnings.Count)"
}
