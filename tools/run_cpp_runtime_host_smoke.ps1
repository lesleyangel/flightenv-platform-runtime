param(
    [string]$Python = "python",

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',

    [string]$RunIdPrefix = "cpp_runtime_host_b2_b4_smoke",

    [int]$OnlineFrames = 4,

    [int]$PredictionEveryFrames = 4,

    [int]$FutureMaxIterations = 1,

    [int]$BranchChunkIterations = 1,

    [int]$BranchWaitTimeoutSeconds = 120,

    [ValidateSet('native_adapter_sessions')]
    [string]$ExecutionBackend = 'native_adapter_sessions',

    [ValidateSet('auto', 'prefer', 'require', 'off')]
    [string]$ZeroCopyMode = 'auto',

    [ValidateSet('shadow_artifact', 'memory_only')]
    [string]$TypedBufferPersistence = 'shadow_artifact',

    [string]$ExternalObservationStream = "",

    [string]$AdapterRegistry = "",

    [switch]$KeepPreviousArtifacts,

    [switch]$PreflightAdapters,

    [switch]$RequireTypedZeroCopy,

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
$objectRoot = Join-Path $workspaceRoot 'flightenv-object-reentry-vehicle'
$pdkRoot = Join-Path $workspaceRoot 'flightenv-platform-pdk'
$compiledRoot = Join-Path $workspaceRoot '_local_artifacts\platform-pdk\compiled-workflows'
$compiledOnline = Join-Path $compiledRoot 'reentry.online_filtering_external_input.v1'
$compiledFuture = Join-Path $compiledRoot 'reentry.posterior_future_prediction.v1'
$exe = Join-Path $workspaceRoot "_deps\workspace\$Platform\$Configuration\FlightEnvPlatformRuntimeHost.exe"
if ([string]::IsNullOrWhiteSpace($AdapterRegistry)) {
    $AdapterRegistry = Join-Path $objectRoot 'tools\adapter_registries\ballistic_adapters.local.json'
}

if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $runtimeRoot 'tools\build_platform_runtime.ps1') `
        -Configuration $Configuration `
        -Platform $Platform
}

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $objectRoot 'tools\compile_workflows.ps1') `
    -Python $Python `
    -PdkRoot $pdkRoot `
    -Workflow online_filtering_external_input `
    -OutDir $compiledRoot `
    -RunId "$RunIdPrefix.compile.online"

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $objectRoot 'tools\compile_workflows.ps1') `
    -Python $Python `
    -PdkRoot $pdkRoot `
    -Workflow posterior_future_prediction `
    -OutDir $compiledRoot `
    -RunId "$RunIdPrefix.compile.future"

if ([string]::IsNullOrWhiteSpace($ExternalObservationStream)) {
    $ExternalObservationStream = Join-Path $workspaceRoot '_local_artifacts\platform-pdk\runtime-host-runs\reentry.online_filtering_external_input.v1\ui_external_stream_smoke_20260614.online\sensor_stream.json'
}
if (-not (Test-Path -LiteralPath $ExternalObservationStream -PathType Leaf)) {
    throw "External observation stream not found. Pass -ExternalObservationStream: $ExternalObservationStream"
}
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "C++ Runtime Host exe not found: $exe"
}

$chainDir = Join-Path $workspaceRoot "_local_artifacts\platform-runtime\mainline-runs\$RunIdPrefix"
$runRoot = Join-Path $workspaceRoot "_local_artifacts\platform-runtime\runtime-host-runs"

if (-not $KeepPreviousArtifacts) {
    foreach ($pathToClean in @($chainDir)) {
        if (Test-Path -LiteralPath $pathToClean) {
            Remove-Item -LiteralPath $pathToClean -Recurse -Force
        }
    }
    foreach ($workflowId in @('reentry.online_filtering_external_input.v1', 'reentry.posterior_future_prediction.v1')) {
        $workflowRunRoot = Join-Path $runRoot $workflowId
        if (-not (Test-Path -LiteralPath $workflowRunRoot -PathType Container)) {
            continue
        }
        Get-ChildItem -LiteralPath $workflowRunRoot -Directory |
            Where-Object { $_.Name -like "$RunIdPrefix*" } |
            Remove-Item -Recurse -Force
    }
}

$hostArgs = @(
    '--workspace-root', $workspaceRoot,
    '--pdk-root', $pdkRoot,
    '--object-package-root', $objectRoot,
    '--compiled-online', $compiledOnline,
    '--compiled-future', $compiledFuture,
    '--adapter-registry', $AdapterRegistry,
    '--external-observation-stream', $ExternalObservationStream,
    '--run-id-prefix', $RunIdPrefix,
    '--run-root', $runRoot,
    '--chain-dir', $chainDir,
    '--python', $Python,
    '--execution-backend', $ExecutionBackend,
    '--zero-copy-mode', $ZeroCopyMode,
    '--typed-buffer-persistence', $TypedBufferPersistence,
    '--online-frames', "$OnlineFrames",
    '--prediction-every-frames', "$PredictionEveryFrames",
    '--future-max-iterations', "$FutureMaxIterations",
    '--branch-chunk-iterations', "$BranchChunkIterations",
    '--max-concurrent-branches', '2'
)
if ($PreflightAdapters) {
    $hostArgs += '--preflight-adapters'
}

Push-Location $workspaceRoot
try {
    & $exe @hostArgs
} finally {
    Pop-Location
}

if ($LASTEXITCODE -ne 0) {
    throw "C++ Runtime Host smoke failed with exit code $LASTEXITCODE"
}

$summaryPath = Join-Path $chainDir 'mainline_summary.json'
if (-not (Test-Path -LiteralPath $summaryPath -PathType Leaf)) {
    throw "mainline summary not generated: $summaryPath"
}
$timelinePath = Join-Path $chainDir 'run_timeline_index.json'
$deadline = (Get-Date).AddSeconds($BranchWaitTimeoutSeconds)
do {
    $summary = Get-Content -LiteralPath $summaryPath -Encoding UTF8 -Raw | ConvertFrom-Json
    $prediction = $summary.prediction
    $runCount = 0
    if ($null -ne $prediction.PSObject.Properties['run_count']) {
        $runCount = [int]$prediction.run_count
    }
    $futureArtifactCount = 0
    $futureQoiCount = 0
    if (Test-Path -LiteralPath $timelinePath -PathType Leaf) {
        $timelineProbe = Get-Content -LiteralPath $timelinePath -Encoding UTF8 -Raw | ConvertFrom-Json
        $futureArtifactCount = @($timelineProbe.artifact_refs | Where-Object { [string]$_.branch_id -like 'predict.*' }).Count
        $futureQoiCount = @($timelineProbe.qoi_refs | Where-Object { [string]$_.branch_id -like 'predict.*' }).Count
    }
    if ($runCount -gt 0 -and $futureArtifactCount -gt 0 -and $futureQoiCount -gt 0) {
        break
    }
    Start-Sleep -Milliseconds 500
} while ((Get-Date) -lt $deadline)
$summary = Get-Content -LiteralPath $summaryPath -Encoding UTF8 -Raw | ConvertFrom-Json
if ([int]$summary.online.effective_frames -le 0) {
    throw "online effective frames is zero"
}
if ([int]$summary.prediction.run_count -le 0) {
    throw "prediction branch count is zero"
}

$healthLedgerPath = Join-Path $chainDir 'health_ledger.json'
if (-not (Test-Path -LiteralPath $healthLedgerPath -PathType Leaf)) {
    throw "health ledger not generated: $healthLedgerPath"
}
$healthLedger = Get-Content -LiteralPath $healthLedgerPath -Encoding UTF8 -Raw | ConvertFrom-Json
if ($healthLedger.schema_version -ne 'flightenv.platform.health_ledger.v1') {
    throw "bad health ledger schema: $($healthLedger.schema_version)"
}

function Assert-NearlyEqual {
    param(
        [double]$Actual,
        [double]$Expected,
        [string]$Message,
        [double]$Tolerance = 1.0e-6
    )
    if ([double]::IsNaN($Actual) -or [double]::IsNaN($Expected) -or [Math]::Abs($Actual - $Expected) -gt $Tolerance) {
        throw "$Message actual=$Actual expected=$Expected"
    }
}

if (-not (Test-Path -LiteralPath $timelinePath -PathType Leaf)) {
    throw "run timeline index not generated: $timelinePath"
}
$timeline = Get-Content -LiteralPath $timelinePath -Encoding UTF8 -Raw | ConvertFrom-Json
$onlineFrameItems = @($timeline.online_frames)
$realtimeSteps = @($timeline.branch_steps | Where-Object { $_.branch_id -eq 'main.realtime_prediction' })
$realtimeArtifacts = @($timeline.artifact_refs | Where-Object { $_.branch_id -eq 'main.realtime_prediction' })
$futureSteps = @($timeline.branch_steps | Where-Object { [string]$_.branch_id -like 'predict.*' })
$futureArtifacts = @($timeline.artifact_refs | Where-Object { [string]$_.branch_id -like 'predict.*' })
$futureQois = @($timeline.qoi_refs | Where-Object { [string]$_.branch_id -like 'predict.*' })

foreach ($frame in $onlineFrameItems) {
    if ($null -eq $frame.PSObject.Properties['public_time_s']) {
        throw "online frame missing public_time_s: frame=$($frame.frame_index)"
    }
    Assert-NearlyEqual ([double]$frame.public_time_s) ([double]$frame.sample_time_s) "online frame public_time_s must equal sample_time_s"
}

if ($onlineFrameItems.Count -gt 1) {
    $distinctRealtimePublicTimes = @($realtimeSteps | ForEach-Object { [double]$_.public_time_s } | Sort-Object -Unique)
    if ($distinctRealtimePublicTimes.Count -le 1) {
        throw "realtime prediction public timeline collapsed to one timestamp"
    }
}

foreach ($step in $realtimeSteps) {
    if ($null -eq $step.PSObject.Properties['public_time_s']) {
        throw "realtime step missing public_time_s: step=$($step.step_index)"
    }
    $frameIndex = [int]$step.mainline_frame_index
    $frame = @($onlineFrameItems | Where-Object { [int]$_.frame_index -eq $frameIndex } | Select-Object -First 1)
    if ($frame.Count -eq 0) {
        throw "realtime step references missing online frame: $frameIndex"
    }
    Assert-NearlyEqual ([double]$step.public_time_s) ([double]$frame[0].sample_time_s) "realtime step public_time_s must align with online sample"
}

foreach ($artifact in $realtimeArtifacts) {
    if ($null -eq $artifact.PSObject.Properties['public_time_s']) {
        throw "realtime artifact missing public_time_s: $($artifact.port_id)"
    }
    $frameIndex = [int]$artifact.mainline_frame_index
    $frame = @($onlineFrameItems | Where-Object { [int]$_.frame_index -eq $frameIndex } | Select-Object -First 1)
    if ($frame.Count -eq 0) {
        throw "realtime artifact references missing online frame: $frameIndex"
    }
    Assert-NearlyEqual ([double]$artifact.public_time_s) ([double]$frame[0].sample_time_s) "realtime artifact public_time_s must align with online sample"
}

foreach ($step in $futureSteps) {
    if ($null -eq $step.PSObject.Properties['public_time_s'] -or
        $null -eq $step.PSObject.Properties['branch_relative_time_s'] -or
        $null -eq $step.PSObject.Properties['trigger_time_s']) {
        throw "future step missing trigger-relative public timing metadata"
    }
    Assert-NearlyEqual ([double]$step.public_time_s) ([double]$step.trigger_time_s + [double]$step.branch_relative_time_s) "future step public_time_s must equal trigger + branch_relative"
    $matchingFields = @($futureArtifacts | Where-Object {
        [string]$_.branch_id -eq [string]$step.branch_id -and
        [Math]::Abs(([double]$_.public_time_s) - ([double]$step.public_time_s)) -le 1.0e-6
    })
    if ($matchingFields.Count -eq 0) {
        throw "future step has no field artifact at matching public_time_s: branch=$($step.branch_id) public=$($step.public_time_s)"
    }
    $matchingQois = @($futureQois | Where-Object {
        [string]$_.branch_id -eq [string]$step.branch_id -and
        [Math]::Abs(([double]$_.public_time_s) - ([double]$step.public_time_s)) -le 1.0e-6
    })
    if ($matchingQois.Count -eq 0) {
        throw "future step has no QoI at matching public_time_s: branch=$($step.branch_id) public=$($step.public_time_s)"
    }
}

if ($RequireTypedZeroCopy) {
    if ($ZeroCopyMode -eq 'off') {
        throw "-RequireTypedZeroCopy cannot be used with -ZeroCopyMode off"
    }
    $runtimeRunRoot = Join-Path $workspaceRoot '_local_artifacts\platform-runtime\runtime-host-runs'
    $runtimeOutputFiles = @(Get-ChildItem -LiteralPath $runtimeRunRoot -Recurse -Filter runtime_outputs.json |
        Where-Object { $_.FullName -like "*$RunIdPrefix*" })
    if ($runtimeOutputFiles.Count -eq 0) {
        throw "typed zero-copy audit found no runtime_outputs.json files for run id prefix: $RunIdPrefix"
    }

    $typedExecuteCount = 0
    $defaultAbiCount = 0
    $maxExtraLogicalRefCount = 0
    $maxLogicalRefCount = 0
    $maxShadowArtifactCount = 0
    $maxMemoryOnlyCount = 0
    foreach ($file in $runtimeOutputFiles) {
        $runtimeOutputs = Get-Content -LiteralPath $file.FullName -Encoding UTF8 -Raw | ConvertFrom-Json
        if ($null -ne $runtimeOutputs.PSObject.Properties['typed_buffer_store']) {
            $store = $runtimeOutputs.typed_buffer_store
            if ($null -ne $store.PSObject.Properties['extra_logical_ref_count']) {
                $extra = [int]$store.extra_logical_ref_count
                if ($extra -gt $maxExtraLogicalRefCount) {
                    $maxExtraLogicalRefCount = $extra
                }
            }
            if ($null -ne $store.PSObject.Properties['max_logical_ref_count']) {
                $maxRef = [int]$store.max_logical_ref_count
                if ($maxRef -gt $maxLogicalRefCount) {
                    $maxLogicalRefCount = $maxRef
                }
            }
            if ($null -ne $store.PSObject.Properties['shadow_artifact_count']) {
                $shadowCount = [int]$store.shadow_artifact_count
                if ($shadowCount -gt $maxShadowArtifactCount) {
                    $maxShadowArtifactCount = $shadowCount
                }
            }
            if ($null -ne $store.PSObject.Properties['memory_only_count']) {
                $memoryOnlyCount = [int]$store.memory_only_count
                if ($memoryOnlyCount -gt $maxMemoryOnlyCount) {
                    $maxMemoryOnlyCount = $memoryOnlyCount
                }
            }
        }
        if ($null -eq $runtimeOutputs.PSObject.Properties['outputs']) {
            continue
        }
        foreach ($prop in $runtimeOutputs.outputs.PSObject.Properties) {
            $nodeOutput = $prop.Value
            if ($null -eq $nodeOutput.PSObject.Properties['runtime_zero_copy_policy']) {
                continue
            }
            $policy = $nodeOutput.runtime_zero_copy_policy
            if ($policy.use_typed_execute -eq $true) {
                $typedExecuteCount += 1
            } elseif ($policy.typed_contract_present -eq $true -or $policy.typed_output_required -eq $true) {
                $defaultAbiCount += 1
            }
        }
    }

    if ($typedExecuteCount -le 0) {
        throw "typed zero-copy audit failed: no typed execute calls were observed"
    }
    if ($defaultAbiCount -gt 0) {
        throw "typed zero-copy audit failed: $defaultAbiCount typed data-plane calls fell back to default ABI"
    }
    if ($maxExtraLogicalRefCount -gt 0) {
        throw "typed zero-copy audit failed: extra logical refs remained after execution, max=$maxExtraLogicalRefCount"
    }
    if ($TypedBufferPersistence -eq 'memory_only' -and $maxShadowArtifactCount -gt 0) {
        throw "typed zero-copy audit failed: memory_only run produced shadow artifacts, max=$maxShadowArtifactCount"
    }
    Write-Host "[OK] typed zero-copy audit passed."
    Write-Host "  runtime_outputs        = $($runtimeOutputFiles.Count)"
    Write-Host "  typed_execute_count    = $typedExecuteCount"
    Write-Host "  default_abi_count      = $defaultAbiCount"
    Write-Host "  max_logical_ref_count  = $maxLogicalRefCount"
    Write-Host "  extra_logical_ref_count= $maxExtraLogicalRefCount"
    Write-Host "  shadow_artifact_count  = $maxShadowArtifactCount"
    Write-Host "  memory_only_count      = $maxMemoryOnlyCount"
}

Write-Host "[OK] C++ Runtime Host smoke passed."
Write-Host "  evidence = $chainDir"
