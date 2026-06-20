param(
    [string]$Python = "python",

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',

    [string]$RunIdPrefix = "round2_multirate_audit",

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

function Set-NodeSamplePeriod {
    param(
        [Parameter(Mandatory=$true)]$JsonValue,
        [Parameter(Mandatory=$true)][string]$NodeId,
        [Parameter(Mandatory=$true)][double]$SamplePeriodS
    )
    $matched = $false
    foreach ($node in @($JsonValue.nodes)) {
        if ([string]$node.node_id -ne $NodeId) {
            continue
        }
        if ($null -eq $node.time_policy) {
            $node | Add-Member -NotePropertyName time_policy -NotePropertyValue ([ordered]@{}) -Force
        }
        if ($null -eq $node.time_policy.PSObject.Properties['kind']) {
            $node.time_policy | Add-Member -NotePropertyName kind -NotePropertyValue "sampled" -Force
        } else {
            $node.time_policy.kind = "sampled"
        }
        if ($null -eq $node.time_policy.PSObject.Properties['sample_period_s']) {
            $node.time_policy | Add-Member -NotePropertyName sample_period_s -NotePropertyValue $SamplePeriodS -Force
        } else {
            $node.time_policy.sample_period_s = $SamplePeriodS
        }
        if ($null -ne $node.time_policy.PSObject.Properties['fixed_dt_s']) {
            $node.time_policy.PSObject.Properties.Remove('fixed_dt_s')
        }
        $matched = $true
    }
    if (-not $matched) {
        throw "Node not found in compiled workflow: $NodeId"
    }
}

function Keep-PlanNodes {
    param(
        [Parameter(Mandatory=$true)]$JsonValue,
        [Parameter(Mandatory=$true)][string[]]$NodeIds
    )
    $keep = @{}
    foreach ($nodeId in $NodeIds) {
        $keep[$nodeId] = $true
    }
    if ($null -eq $JsonValue.PSObject.Properties['nodes']) {
        return
    }
    $JsonValue.nodes = @($JsonValue.nodes | Where-Object {
        $null -ne $_.PSObject.Properties['node_id'] -and $keep.ContainsKey([string]$_.node_id)
    })
}

function Assert-Equal {
    param(
        [Parameter(Mandatory=$true)]$Actual,
        [Parameter(Mandatory=$true)]$Expected,
        [Parameter(Mandatory=$true)][string]$Message
    )
    if ($Actual -ne $Expected) {
        throw "$Message expected=$Expected actual=$Actual"
    }
}

$runtimeRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspaceRoot = [System.IO.Path]::GetFullPath((Join-Path $runtimeRoot '..'))
$objectRoot = Join-Path $workspaceRoot 'flightenv-object-reentry-vehicle'
$pdkRoot = Join-Path $workspaceRoot 'flightenv-platform-pdk'
$compiledRoot = Join-Path $workspaceRoot '_local_artifacts\platform-runtime\round2-multirate\compiled-workflows'
$scratchRoot = Join-Path $workspaceRoot '_local_artifacts\platform-runtime\round2-multirate'
$compiledSource = Join-Path $compiledRoot 'reentry.posterior_future_prediction.v1'
$compiledAudit = Join-Path $compiledRoot 'reentry.posterior_future_prediction.round2_multirate.v1'
$runRoot = Join-Path $scratchRoot 'runtime-host-runs'
$chainDir = Join-Path $scratchRoot 'mainline'
$runDir = Join-Path $runRoot 'reentry.posterior_future_prediction.round2_multirate.v1\round2_multirate_branch'
$registryPath = Join-Path $scratchRoot 'json_echo_adapter_registry.json'
$seedPath = Join-Path $scratchRoot 'seed_runtime_outputs.json'
$exe = Join-Path $workspaceRoot "_deps\workspace\$Platform\$Configuration\FlightEnvPlatformRuntimeHost.exe"
$targetNode = 'future_step.state_transition.pressure_coef'
$baselineNode = 'future_step.state_transition.ballistic'
$auditNodes = @(
    'future_step.state_transition.ballistic',
    'future_step.state_transition.pressure_coef',
    'future_step.state_transition.heatflux_coef',
    'future_step.state_transition.structure_coef'
)

if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $runtimeRoot 'tools\build_platform_runtime.ps1') `
        -Configuration $Configuration `
        -Platform $Platform
}
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Runtime host executable not found: $exe"
}

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $objectRoot 'tools\compile_workflows.ps1') `
    -Python $Python `
    -PdkRoot $pdkRoot `
    -Workflow posterior_future_prediction `
    -OutDir $compiledRoot `
    -RunId "$RunIdPrefix.compile.future"

if (Test-Path -LiteralPath $compiledAudit) {
    Remove-Item -LiteralPath $compiledAudit -Recurse -Force
}
Copy-Item -LiteralPath $compiledSource -Destination $compiledAudit -Recurse -Force

$executionPlanPath = Join-Path $compiledAudit 'execution_plan.json'
$timePlanPath = Join-Path $compiledAudit 'time_plan.json'
$schedulerPlanPath = Join-Path $compiledAudit 'scheduler_plan.json'
$uncertaintyPlanPath = Join-Path $compiledAudit 'uncertainty_plan.json'
$stateStorePlanPath = Join-Path $compiledAudit 'state_store_plan.json'
$dataPlanePlanPath = Join-Path $compiledAudit 'data_plane_plan.json'
$executionPlan = Read-JsonFile -PathValue $executionPlanPath
$timePlan = Read-JsonFile -PathValue $timePlanPath
Set-NodeSamplePeriod -JsonValue $executionPlan -NodeId $targetNode -SamplePeriodS 4.0
Set-NodeSamplePeriod -JsonValue $timePlan -NodeId $targetNode -SamplePeriodS 4.0
Keep-PlanNodes -JsonValue $executionPlan -NodeIds $auditNodes
Keep-PlanNodes -JsonValue $timePlan -NodeIds $auditNodes
Write-JsonFile -PathValue $executionPlanPath -Value $executionPlan
Write-JsonFile -PathValue $timePlanPath -Value $timePlan
foreach ($planPath in @($schedulerPlanPath, $uncertaintyPlanPath, $stateStorePlanPath, $dataPlanePlanPath)) {
    if (Test-Path -LiteralPath $planPath -PathType Leaf) {
        $plan = Read-JsonFile -PathValue $planPath
        Keep-PlanNodes -JsonValue $plan -NodeIds $auditNodes
        Write-JsonFile -PathValue $planPath -Value $plan
    }
}

$adapterEntries = @()
$adapterIds = @($executionPlan.nodes | ForEach-Object { [string]$_.adapter_id } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)
foreach ($adapterId in $adapterIds) {
    $adapterEntries += [ordered]@{
        adapter_id = $adapterId
        execution_kind = "json_file_smoke"
        protocol = "json_file.v1"
        command = @("{python}", "{pdk_root}\tools\sample_adapters\json_echo_adapter.py")
        working_directory = "{workspace_root}"
        timeout_ms = 30000
        capability_status = "smoke_test_only"
    }
}
$registry = [ordered]@{
    schema_version = "flightenv.platform.adapter_registry.v1"
    generated_at_utc = "round2_multirate_audit"
    adapters = $adapterEntries
}
Write-JsonFile -PathValue $registryPath -Value $registry
Write-JsonFile -PathValue $seedPath -Value ([ordered]@{
    schema_version = "flightenv.platform.runtime_outputs.v1"
    run_id = "$RunIdPrefix.seed"
    workflow_id = "seed"
    object_id = "reentry_vehicle"
    outputs = [ordered]@{}
})

foreach ($pathToClean in @($runDir, $chainDir)) {
    if (Test-Path -LiteralPath $pathToClean) {
        Remove-Item -LiteralPath $pathToClean -Recurse -Force
    }
}
New-Item -ItemType Directory -Force -Path $scratchRoot | Out-Null

& $exe `
    --workspace-root $workspaceRoot `
    --pdk-root $pdkRoot `
    --object-package-root $objectRoot `
    --compiled-online $compiledAudit `
    --compiled-future $compiledAudit `
    --adapter-registry $registryPath `
    --run-root $runRoot `
    --chain-dir $chainDir `
    --run-id-prefix $RunIdPrefix `
    --branch-worker `
    --branch-id audit.round2_multirate `
    --branch-run-id round2_multirate_branch `
    --branch-run-dir $runDir `
    --seed-runtime-outputs $seedPath `
    --trigger-frame-index 0 `
    --trigger-time-s 0 `
    --future-max-iterations 5 `
    --branch-chunk-iterations 5 `
    --execution-backend native_adapter_sessions `
    --python $Python `
    --require-adapter-registry

if ($LASTEXITCODE -ne 0) {
    throw "RuntimeHost Round2 multirate audit failed with exit code $LASTEXITCODE"
}

$scheduler = Read-JsonFile -PathValue (Join-Path $runDir '_branch_chunks\chunk_000000\scheduler_timeline.json')
$loop = Read-JsonFile -PathValue (Join-Path $runDir 'runtime_loop_summary.json')
$targetStart = @($scheduler.events | Where-Object {
    $null -ne $_.PSObject.Properties['node_id'] -and [string]$_.node_id -eq $targetNode -and [string]$_.event -eq 'start'
})
$targetHeld = @($scheduler.events | Where-Object {
    $null -ne $_.PSObject.Properties['node_id'] -and [string]$_.node_id -eq $targetNode -and [string]$_.event -eq 'held'
})
$baselineStart = @($scheduler.events | Where-Object {
    $null -ne $_.PSObject.Properties['node_id'] -and [string]$_.node_id -eq $baselineNode -and [string]$_.event -eq 'start'
})

Assert-Equal -Actual $baselineStart.Count -Expected 5 -Message "$baselineNode should execute on every public tick"
Assert-Equal -Actual $targetStart.Count -Expected 3 -Message "$targetNode should execute every 4s over five 2s ticks"
Assert-Equal -Actual $targetHeld.Count -Expected 2 -Message "$targetNode should carry forward on intermediate ticks"
Assert-Equal -Actual ([int]$loop.summary.iteration_count) -Expected 5 -Message "runtime loop iteration count"
Assert-Equal -Actual ([int]$loop.summary.held_output_count) -Expected 2 -Message "runtime loop held output count"

$targetEffective = @($targetStart | ForEach-Object { [double]$_.effective_delta_t_s })
if ($targetEffective.Count -ne 3 -or [Math]::Abs($targetEffective[0] - 2.0) -gt 1e-9 -or
    [Math]::Abs($targetEffective[1] - 4.0) -gt 1e-9 -or
    [Math]::Abs($targetEffective[2] - 4.0) -gt 1e-9) {
    throw "$targetNode effective_delta_t_s mismatch: $($targetEffective -join ', ')"
}

$targetEventTimes = @($targetStart | ForEach-Object { [double]$_.runtime_event_time_s })
if ($targetEventTimes.Count -ne 3 -or [Math]::Abs($targetEventTimes[0] - 2.0) -gt 1e-9 -or
    [Math]::Abs($targetEventTimes[1] - 6.0) -gt 1e-9 -or
    [Math]::Abs($targetEventTimes[2] - 10.0) -gt 1e-9) {
    throw "$targetNode node_due event times mismatch: $($targetEventTimes -join ', ')"
}

foreach ($event in $targetStart) {
    Assert-Equal -Actual ([string]$event.runtime_event_kind) -Expected "node_due" -Message "$targetNode start event kind"
}
foreach ($event in $targetHeld) {
    Assert-Equal -Actual ([string]$event.runtime_event_kind) -Expected "public_tick" -Message "$targetNode held event kind"
}

$publicFinish = @($scheduler.events | Where-Object { [string]$_.event -eq 'loop_iteration_finish' })
Assert-Equal -Actual $publicFinish.Count -Expected 5 -Message "public tick finish event count"
foreach ($event in $publicFinish) {
    Assert-Equal -Actual ([string]$event.runtime_event_kind) -Expected "public_tick" -Message "public tick finish event kind"
}

$heldIterationCount = @($loop.iterations | Where-Object { [int]$_.held_output_count -gt 0 }).Count
Assert-Equal -Actual $heldIterationCount -Expected 2 -Message "loop iterations with held outputs"

Write-Host "[OK] Round2 multirate runtime audit passed."
Write-Host "  run_dir = $runDir"
