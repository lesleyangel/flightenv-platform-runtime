param(
    [string]$Python = "python",

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',

    [string]$RunIdPrefix = "typed_abi_perf",

    [int]$OnlineFrames = 3,

    [int]$PredictionEveryFrames = 1,

    [int]$FutureMaxIterations = 2,

    [int]$BranchChunkIterations = 1,

    [int]$BranchWaitTimeoutSeconds = 180,

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

$runtimeRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspaceRoot = [System.IO.Path]::GetFullPath((Join-Path $runtimeRoot '..'))
$smokeScript = Join-Path $runtimeRoot 'tools\run_cpp_runtime_host_smoke.ps1'
$reportRoot = Join-Path $workspaceRoot '_local_artifacts\platform-runtime\perf-reports'
New-Item -ItemType Directory -Force -Path $reportRoot | Out-Null

function Invoke-SmokeRun {
    param(
        [string]$RunId,
        [bool]$DisableTypedAbi
    )

    $oldDisable = $env:FLIGHTENV_DISABLE_TYPED_ADAPTER_ABI_V2
    $oldBridge = $env:FLIGHTENV_ALLOW_INLINE_JSON_TYPED_PAYLOAD_BRIDGE
    if ($DisableTypedAbi) {
        $env:FLIGHTENV_DISABLE_TYPED_ADAPTER_ABI_V2 = '1'
        $env:FLIGHTENV_ALLOW_INLINE_JSON_TYPED_PAYLOAD_BRIDGE = '1'
    } else {
        Remove-Item Env:\FLIGHTENV_DISABLE_TYPED_ADAPTER_ABI_V2 -ErrorAction SilentlyContinue
        Remove-Item Env:\FLIGHTENV_ALLOW_INLINE_JSON_TYPED_PAYLOAD_BRIDGE -ErrorAction SilentlyContinue
    }
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $args = @(
            '-Python', $Python,
            '-Configuration', $Configuration,
            '-Platform', $Platform,
            '-RunIdPrefix', $RunId,
            '-OnlineFrames', "$OnlineFrames",
            '-PredictionEveryFrames', "$PredictionEveryFrames",
            '-FutureMaxIterations', "$FutureMaxIterations",
            '-BranchChunkIterations', "$BranchChunkIterations",
            '-BranchWaitTimeoutSeconds', "$BranchWaitTimeoutSeconds",
            '-ExecutionBackend', 'native_adapter_sessions'
        )
        $args += '-SkipBuild'
        $smokeOutput = & powershell -NoProfile -ExecutionPolicy Bypass -File $smokeScript @args 2>&1
        $exitCode = $LASTEXITCODE
        foreach ($line in $smokeOutput) {
            Write-Host $line
        }
        if ($exitCode -ne 0) {
            throw "smoke run failed: $RunId exit=$exitCode"
        }
    } finally {
        $stopwatch.Stop()
        if ($null -eq $oldDisable) {
            Remove-Item Env:\FLIGHTENV_DISABLE_TYPED_ADAPTER_ABI_V2 -ErrorAction SilentlyContinue
        } else {
            $env:FLIGHTENV_DISABLE_TYPED_ADAPTER_ABI_V2 = $oldDisable
        }
        if ($null -eq $oldBridge) {
            Remove-Item Env:\FLIGHTENV_ALLOW_INLINE_JSON_TYPED_PAYLOAD_BRIDGE -ErrorAction SilentlyContinue
        } else {
            $env:FLIGHTENV_ALLOW_INLINE_JSON_TYPED_PAYLOAD_BRIDGE = $oldBridge
        }
    }
    return $stopwatch.Elapsed.TotalSeconds
}

function Read-JsonFile {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "JSON file not found: $Path"
    }
    return Get-Content -LiteralPath $Path -Encoding UTF8 -Raw | ConvertFrom-Json
}

function Count-Array {
    param($Value)
    if ($null -eq $Value) {
        return 0
    }
    return @($Value).Count
}

function Collect-RuntimeOutputMetrics {
    param([string]$RunId)

    $runRoot = Join-Path $workspaceRoot '_local_artifacts\platform-runtime\runtime-host-runs'
    $files = @(Get-ChildItem -LiteralPath $runRoot -Recurse -Filter runtime_outputs.json |
        Where-Object { $_.FullName -like "*$RunId*" })

    $typedExecuteCount = 0
    $typedStateInputUsedCount = 0
    $typedInputDecodedCount = 0
    $typedInputConsumedCount = 0
    $typedInputConsumerNodeCount = 0
    $typedOutputCount = 0
    $adapterDurationMs = 0.0
    $adapterDurationNodeCount = 0
    $nativeDtoBufferCount = 0

    foreach ($file in $files) {
        $doc = Read-JsonFile -Path $file.FullName
        if ($null -eq $doc.outputs) {
            continue
        }
        foreach ($prop in $doc.outputs.PSObject.Properties) {
            $node = $prop.Value
            if ($null -ne $node.PSObject.Properties['duration_ms']) {
                $adapterDurationMs += [double]$node.duration_ms
                $adapterDurationNodeCount += 1
            }
            if ($null -ne $node.PSObject.Properties['adapter_typed_abi_v2'] -and [bool]$node.adapter_typed_abi_v2) {
                $typedExecuteCount += 1
            }
            if ($null -ne $node.PSObject.Properties['adapter_typed_state_input_used'] -and [bool]$node.adapter_typed_state_input_used) {
                $typedStateInputUsedCount += 1
            }
            if ($null -ne $node.PSObject.Properties['adapter_typed_input_decoded_count']) {
                $typedInputDecodedCount += [int]$node.adapter_typed_input_decoded_count
            }
            if ($null -ne $node.PSObject.Properties['adapter_typed_input_consumed_count']) {
                $consumed = [int]$node.adapter_typed_input_consumed_count
                $typedInputConsumedCount += $consumed
                if ($consumed -gt 0) {
                    $typedInputConsumerNodeCount += 1
                }
            }
            if ($null -ne $node.PSObject.Properties['adapter_typed_output_count']) {
                $typedOutputCount += [int]$node.adapter_typed_output_count
            }
            if ($null -ne $node.outputs) {
                foreach ($outProp in $node.outputs.PSObject.Properties) {
                    $payload = $outProp.Value
                    if ($null -ne $payload.PSObject.Properties['typed_buffer_ref']) {
                        $ref = $payload.typed_buffer_ref
                        if ($null -ne $ref.PSObject.Properties['format'] -and
                            [string]$ref.format -ne 'json_typed_payload.v1') {
                            $nativeDtoBufferCount += 1
                        }
                    }
                }
            }
        }
    }

    return [ordered]@{
        runtime_outputs_file_count = $files.Count
        adapter_duration_ms_sum = [Math]::Round($adapterDurationMs, 3)
        adapter_duration_node_count = $adapterDurationNodeCount
        typed_execute_count = $typedExecuteCount
        typed_state_input_used_count = $typedStateInputUsedCount
        typed_input_decoded_count = $typedInputDecodedCount
        typed_input_consumed_count = $typedInputConsumedCount
        typed_input_consumer_node_count = $typedInputConsumerNodeCount
        typed_output_count = $typedOutputCount
        native_dto_buffer_count = $nativeDtoBufferCount
    }
}

function Collect-RunMetrics {
    param(
        [string]$RunId,
        [double]$WallSeconds,
        [bool]$TypedAbiDisabled
    )

    $chainDir = Join-Path $workspaceRoot "_local_artifacts\platform-runtime\mainline-runs\$RunId"
    $summary = Read-JsonFile -Path (Join-Path $chainDir 'mainline_summary.json')
    $timeline = Read-JsonFile -Path (Join-Path $chainDir 'run_timeline_index.json')
    $runtimeMetrics = Collect-RuntimeOutputMetrics -RunId $RunId

    $onlineFrames = @(if ($null -ne $timeline.online_frames) { $timeline.online_frames } else { @() })
    $futureSteps = @(if ($null -ne $timeline.branch_steps) {
        $timeline.branch_steps | Where-Object { [string]$_.branch_id -like 'predict.*' }
    } else { @() })
    $futureArtifacts = @(if ($null -ne $timeline.artifact_refs) {
        $timeline.artifact_refs | Where-Object { [string]$_.branch_id -like 'predict.*' }
    } else { @() })
    $futureQois = @(if ($null -ne $timeline.qoi_refs) {
        $timeline.qoi_refs | Where-Object { [string]$_.branch_id -like 'predict.*' }
    } else { @() })
    $realtimeArtifacts = @(if ($null -ne $timeline.artifact_refs) {
        $timeline.artifact_refs | Where-Object { [string]$_.branch_id -eq 'main.realtime_prediction' }
    } else { @() })

    return [ordered]@{
        run_id = $RunId
        typed_adapter_abi_disabled = $TypedAbiDisabled
        wall_seconds = [Math]::Round($WallSeconds, 3)
        status = [string]$summary.status
        online_effective_frames = [int]$summary.online.effective_frames
        prediction_requested_branch_count = [int]$summary.prediction.requested_branch_count
        prediction_completed_branch_count = [int]$summary.prediction.completed_branch_count
        prediction_failed_branch_count = [int]$summary.prediction.failed_branch_count
        future_step_count = $futureSteps.Count
        realtime_field_artifact_count = $realtimeArtifacts.Count
        future_field_artifact_count = $futureArtifacts.Count
        future_qoi_count = $futureQois.Count
        runtime_outputs = $runtimeMetrics
    }
}

if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $runtimeRoot 'tools\build_platform_runtime.ps1') `
        -Configuration $Configuration `
        -Platform $Platform
}

$baselineRunId = "$RunIdPrefix.baseline_json_abi"
$typedRunId = "$RunIdPrefix.typed_abi_v2"

Write-Host "[perf] baseline JSON ABI run: $baselineRunId"
$baselineSeconds = Invoke-SmokeRun -RunId $baselineRunId -DisableTypedAbi $true
Write-Host "[perf] typed ABI v2 run: $typedRunId"
$typedSeconds = Invoke-SmokeRun -RunId $typedRunId -DisableTypedAbi $false

$baselineMetrics = Collect-RunMetrics -RunId $baselineRunId -WallSeconds $baselineSeconds -TypedAbiDisabled $true
$typedMetrics = Collect-RunMetrics -RunId $typedRunId -WallSeconds $typedSeconds -TypedAbiDisabled $false

$wallDeltaSeconds = [double]$baselineMetrics.wall_seconds - [double]$typedMetrics.wall_seconds
$wallSpeedupPct = if ([double]$baselineMetrics.wall_seconds -gt 0) {
    100.0 * $wallDeltaSeconds / [double]$baselineMetrics.wall_seconds
} else {
    0.0
}
$adapterDeltaMs = [double]$baselineMetrics.runtime_outputs.adapter_duration_ms_sum -
                  [double]$typedMetrics.runtime_outputs.adapter_duration_ms_sum
$adapterSpeedupPct = if ([double]$baselineMetrics.runtime_outputs.adapter_duration_ms_sum -gt 0) {
    100.0 * $adapterDeltaMs / [double]$baselineMetrics.runtime_outputs.adapter_duration_ms_sum
} else {
    0.0
}

$report = [ordered]@{
    schema_version = 'flightenv.platform.typed_abi_perf_comparison.v1'
    generated_at_utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    configuration = [ordered]@{
        online_frames = $OnlineFrames
        prediction_every_frames = $PredictionEveryFrames
        future_max_iterations = $FutureMaxIterations
        branch_chunk_iterations = $BranchChunkIterations
        execution_backend = 'native_adapter_sessions'
    }
    baseline = $baselineMetrics
    typed_abi_v2 = $typedMetrics
    comparison = [ordered]@{
        wall_delta_seconds = [Math]::Round($wallDeltaSeconds, 3)
        wall_speedup_percent = [Math]::Round($wallSpeedupPct, 2)
        adapter_duration_delta_ms_sum = [Math]::Round($adapterDeltaMs, 3)
        adapter_duration_speedup_percent = [Math]::Round($adapterSpeedupPct, 2)
        typed_execute_count_delta = [int]$typedMetrics.runtime_outputs.typed_execute_count -
                                    [int]$baselineMetrics.runtime_outputs.typed_execute_count
        typed_input_consumed_count_delta = [int]$typedMetrics.runtime_outputs.typed_input_consumed_count -
                                          [int]$baselineMetrics.runtime_outputs.typed_input_consumed_count
        native_dto_buffer_count_delta = [int]$typedMetrics.runtime_outputs.native_dto_buffer_count -
                                        [int]$baselineMetrics.runtime_outputs.native_dto_buffer_count
    }
}

$reportPath = Join-Path $reportRoot "$RunIdPrefix.typed_abi_perf_report.json"
$report | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $reportPath -Encoding UTF8

Write-Host "[OK] typed ABI performance comparison complete."
Write-Host "  report   = $reportPath"
Write-Host "  baseline = $($baselineMetrics.wall_seconds)s / adapter $($baselineMetrics.runtime_outputs.adapter_duration_ms_sum)ms"
Write-Host "  typed    = $($typedMetrics.wall_seconds)s / adapter $($typedMetrics.runtime_outputs.adapter_duration_ms_sum)ms"
Write-Host "  wall speedup    = $([Math]::Round($wallSpeedupPct, 2))%"
Write-Host "  adapter speedup = $([Math]::Round($adapterSpeedupPct, 2))%"
