param(
    [string]$Python = "python",

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',

    [string]$RunIdPrefix = "phase2_rate_transition_audit",

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

function Set-JsonProperty {
    param(
        [Parameter(Mandatory=$true)]$ObjectValue,
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)]$Value
    )
    if ($null -eq $ObjectValue.PSObject.Properties[$Name]) {
        $ObjectValue | Add-Member -NotePropertyName $Name -NotePropertyValue $Value -Force
    } else {
        $ObjectValue.PSObject.Properties[$Name].Value = $Value
    }
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

function Assert-True {
    param(
        [Parameter(Mandatory=$true)][bool]$Condition,
        [Parameter(Mandatory=$true)][string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
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
        if ($null -eq $node.PSObject.Properties['time_policy'] -or $null -eq $node.time_policy) {
            Set-JsonProperty -ObjectValue $node -Name time_policy -Value ([ordered]@{})
        }
        Set-JsonProperty -ObjectValue $node.time_policy -Name kind -Value "sampled"
        Set-JsonProperty -ObjectValue $node.time_policy -Name sample_period_s -Value $SamplePeriodS
        Set-JsonProperty -ObjectValue $node -Name delta_t_s -Value $SamplePeriodS
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

function Update-EdgeBindingPlanSummary {
    param([Parameter(Mandatory=$true)]$Plan)
    if ($null -eq $Plan.PSObject.Properties['summary']) {
        Set-JsonProperty -ObjectValue $Plan -Name summary -Value ([ordered]@{})
    }
    $count = @($Plan.bindings).Count
    Set-JsonProperty -ObjectValue $Plan.summary -Name binding_count -Value $count
}

function Update-RateTransitionPlanSummary {
    param([Parameter(Mandatory=$true)]$Plan)
    if ($null -eq $Plan.PSObject.Properties['summary']) {
        Set-JsonProperty -ObjectValue $Plan -Name summary -Value ([ordered]@{})
    }
    $transitions = @($Plan.transitions)
    $same = @($transitions | Where-Object { [string]$_.rate_relation -eq "same_rate" }).Count
    $fast = @($transitions | Where-Object { [string]$_.rate_relation -eq "fast_to_slow" }).Count
    $slow = @($transitions | Where-Object { [string]$_.rate_relation -eq "slow_to_fast" }).Count
    $runtime = @($transitions | Where-Object { [bool]$_.requires_runtime_transition }).Count
    Set-JsonProperty -ObjectValue $Plan.summary -Name transition_count -Value $transitions.Count
    Set-JsonProperty -ObjectValue $Plan.summary -Name same_rate_transition_count -Value $same
    Set-JsonProperty -ObjectValue $Plan.summary -Name cross_rate_transition_count -Value ($fast + $slow)
    Set-JsonProperty -ObjectValue $Plan.summary -Name fast_to_slow_count -Value $fast
    Set-JsonProperty -ObjectValue $Plan.summary -Name slow_to_fast_count -Value $slow
    Set-JsonProperty -ObjectValue $Plan.summary -Name runtime_transition_count -Value $runtime
}

function Get-RateRelation {
    param(
        [Parameter(Mandatory=$true)][double]$SourcePeriodS,
        [Parameter(Mandatory=$true)][double]$TargetPeriodS
    )
    if ([Math]::Abs($SourcePeriodS - $TargetPeriodS) -le 1e-9) {
        return "same_rate"
    }
    if ($SourcePeriodS -lt $TargetPeriodS) {
        return "fast_to_slow"
    }
    return "slow_to_fast"
}

$runtimeRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspaceRoot = [System.IO.Path]::GetFullPath((Join-Path $runtimeRoot '..'))
$objectRoot = Join-Path $workspaceRoot 'flightenv-object-reentry-vehicle'
$pdkRoot = Join-Path $workspaceRoot 'flightenv-platform-pdk'
$scratchRoot = Join-Path $workspaceRoot '_local_artifacts\platform-runtime\p2rt'
$compiledRoot = Join-Path $scratchRoot 'compiled-workflows'
$compiledSource = Join-Path $compiledRoot 'reentry.posterior_future_prediction.v1'
$compiledAudit = Join-Path $compiledRoot 'p2rt'
$runRoot = Join-Path $scratchRoot 'runtime-host-runs'
$chainDir = Join-Path $scratchRoot 'mainline'
$runDir = Join-Path $runRoot 'p2rt\b'
$registryPath = Join-Path $scratchRoot 'json_echo_adapter_registry.json'
$seedPath = Join-Path $scratchRoot 'seed_runtime_outputs.json'
$exe = Join-Path $workspaceRoot "_deps\workspace\$Platform\$Configuration\FlightEnvPlatformRuntimeHost.exe"
$targetNode = 'future_step.state_transition.pressure_coef'
$baselineNode = 'future_step.state_transition.ballistic'
$targetPeriodS = 4.0
$auditNodes = [System.Collections.Generic.HashSet[string]]::new()
[void]$auditNodes.Add($baselineNode)
[void]$auditNodes.Add($targetNode)
[void]$auditNodes.Add('future_step.state_transition.heatflux_coef')
[void]$auditNodes.Add('future_step.state_transition.structure_coef')

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
$edgePlanPath = Join-Path $compiledAudit 'edge_binding_plan.json'
$ratePlanPath = Join-Path $compiledAudit 'rate_transition_plan.json'

$executionPlan = Read-JsonFile -PathValue $executionPlanPath
$timePlan = Read-JsonFile -PathValue $timePlanPath
$edgePlan = Read-JsonFile -PathValue $edgePlanPath
$ratePlan = Read-JsonFile -PathValue $ratePlanPath

$targetTransitions = @($ratePlan.transitions | Where-Object { [string]$_.target_node_id -eq $targetNode })
Assert-True -Condition ($targetTransitions.Count -gt 0) -Message "No rate transition targets $targetNode"
foreach ($transition in $targetTransitions) {
    [void]$auditNodes.Add([string]$transition.source_node_id)
    [void]$auditNodes.Add([string]$transition.target_node_id)
}
$auditNodeArray = @($auditNodes)

Set-NodeSamplePeriod -JsonValue $executionPlan -NodeId $targetNode -SamplePeriodS $targetPeriodS
Set-NodeSamplePeriod -JsonValue $timePlan -NodeId $targetNode -SamplePeriodS $targetPeriodS
Keep-PlanNodes -JsonValue $executionPlan -NodeIds $auditNodeArray
Keep-PlanNodes -JsonValue $timePlan -NodeIds $auditNodeArray
foreach ($planPath in @($schedulerPlanPath, $uncertaintyPlanPath, $stateStorePlanPath, $dataPlanePlanPath)) {
    if (Test-Path -LiteralPath $planPath -PathType Leaf) {
        $plan = Read-JsonFile -PathValue $planPath
        Keep-PlanNodes -JsonValue $plan -NodeIds $auditNodeArray
        Write-JsonFile -PathValue $planPath -Value $plan
    }
}

$edgePlan.bindings = @($edgePlan.bindings | Where-Object {
    $auditNodes.Contains([string]$_.source_node_id) -and $auditNodes.Contains([string]$_.target_node_id)
})
$timeByNode = @{}
foreach ($node in @($timePlan.nodes)) {
    $timeByNode[[string]$node.node_id] = $node
}
$ratePlan.transitions = @($ratePlan.transitions | Where-Object {
    $auditNodes.Contains([string]$_.source_node_id) -and $auditNodes.Contains([string]$_.target_node_id)
})
foreach ($transition in @($ratePlan.transitions)) {
    $sourcePeriod = [double]$timeByNode[[string]$transition.source_node_id].delta_t_s
    $targetPeriod = [double]$timeByNode[[string]$transition.target_node_id].delta_t_s
    Set-JsonProperty -ObjectValue $transition -Name source_period_s -Value $sourcePeriod
    Set-JsonProperty -ObjectValue $transition -Name target_period_s -Value $targetPeriod
    $relation = Get-RateRelation -SourcePeriodS $sourcePeriod -TargetPeriodS $targetPeriod
    Set-JsonProperty -ObjectValue $transition -Name rate_relation -Value $relation
    $strategy = if ($relation -eq "same_rate") {
        "direct"
    } elseif ($relation -eq "fast_to_slow") {
        "exact_or_substep"
    } else {
        "hold_last"
    }
    Set-JsonProperty -ObjectValue $transition -Name strategy -Value $strategy
    $requiresRuntime = ($relation -ne "same_rate") -or ($strategy -ne "direct")
    Set-JsonProperty -ObjectValue $transition -Name requires_runtime_transition -Value $requiresRuntime
    Set-JsonProperty -ObjectValue $transition -Name transition_node_id -Value ($(if ($requiresRuntime) { [string]$transition.transition_id } else { "" }))
    if ($strategy -eq "hold_last") {
        Set-JsonProperty -ObjectValue $transition -Name max_age_s -Value ([Math]::Max($sourcePeriod, $targetPeriod))
    }
}
Update-EdgeBindingPlanSummary -Plan $edgePlan
Update-RateTransitionPlanSummary -Plan $ratePlan
Assert-True -Condition ([int]$ratePlan.summary.cross_rate_transition_count -gt 0) -Message "Phase2 audit did not create a cross-rate transition"

Write-JsonFile -PathValue $executionPlanPath -Value $executionPlan
Write-JsonFile -PathValue $timePlanPath -Value $timePlan
Write-JsonFile -PathValue $edgePlanPath -Value $edgePlan
Write-JsonFile -PathValue $ratePlanPath -Value $ratePlan

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
    generated_at_utc = "phase2_rate_transition_audit"
    adapters = $adapterEntries
}
Write-JsonFile -PathValue $registryPath -Value $registry
Write-JsonFile -PathValue $seedPath -Value ([ordered]@{
    schema_version = "flightenv.platform.runtime_outputs.v1"
    run_id = "$RunIdPrefix.seed"
    workflow_id = "seed"
    object_id = "object"
    outputs = [ordered]@{}
})

foreach ($pathToClean in @($runDir, $chainDir)) {
    if (Test-Path -LiteralPath $pathToClean) {
        Remove-Item -LiteralPath $pathToClean -Recurse -Force
    }
}
New-Item -ItemType Directory -Force -Path $scratchRoot | Out-Null

$previousTypedBridge = $env:FLIGHTENV_ALLOW_INLINE_JSON_TYPED_PAYLOAD_BRIDGE
$runtimeExitCode = 0
try {
    $env:FLIGHTENV_ALLOW_INLINE_JSON_TYPED_PAYLOAD_BRIDGE = '1'
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
        --branch-id audit.phase2_rate_transition `
        --branch-run-id phase2_rate_transition_branch `
        --branch-run-dir $runDir `
        --seed-runtime-outputs $seedPath `
        --trigger-frame-index 0 `
        --trigger-time-s 0 `
        --future-max-iterations 5 `
        --branch-chunk-iterations 5 `
        --execution-backend native_adapter_sessions `
        --python $Python `
        --require-adapter-registry
    $runtimeExitCode = $LASTEXITCODE
} finally {
    if ($null -eq $previousTypedBridge) {
        Remove-Item Env:\FLIGHTENV_ALLOW_INLINE_JSON_TYPED_PAYLOAD_BRIDGE -ErrorAction SilentlyContinue
    } else {
        $env:FLIGHTENV_ALLOW_INLINE_JSON_TYPED_PAYLOAD_BRIDGE = $previousTypedBridge
    }
}
if ($runtimeExitCode -ne 0) {
    throw "RuntimeHost Phase2 rate transition audit failed with exit code $runtimeExitCode"
}

$scheduler = Read-JsonFile -PathValue (Join-Path $runDir '_branch_chunks\chunk_000000\scheduler_timeline.json')
$runtimeEvidence = Read-JsonFile -PathValue (Join-Path $runDir 'runtime_evidence.json')
$runRatePlan = Read-JsonFile -PathValue (Join-Path $runDir 'rate_transition_plan.json')
$targetTransition = @($ratePlan.transitions | Where-Object {
    [string]$_.target_node_id -eq $targetNode -and [string]$_.rate_relation -ne "same_rate"
} | Select-Object -First 1)
Assert-Equal -Actual $targetTransition.Count -Expected 1 -Message "target cross-rate transition count"
$targetTransitionId = [string]$targetTransition[0].transition_id

Assert-Equal -Actual ([int]$runtimeEvidence.summary.cross_rate_transition_count) -Expected ([int]$ratePlan.summary.cross_rate_transition_count) -Message "runtime evidence cross-rate transition count"
Assert-Equal -Actual ([string]$runtimeEvidence.refs.rate_transition_plan) -Expected "rate_transition_plan.json" -Message "runtime evidence rate transition ref"
Assert-Equal -Actual ([int]$runRatePlan.summary.cross_rate_transition_count) -Expected ([int]$ratePlan.summary.cross_rate_transition_count) -Message "emitted rate transition summary"

$targetStart = @($scheduler.events | Where-Object {
    $null -ne $_.PSObject.Properties['node_id'] -and [string]$_.node_id -eq $targetNode -and [string]$_.event -eq 'start'
})
Assert-True -Condition ($targetStart.Count -gt 0) -Message "$targetNode did not start"
foreach ($event in $targetStart) {
    $alignments = @($event.input_alignment | Where-Object { [string]$_.transition_id -eq $targetTransitionId })
    Assert-Equal -Actual $alignments.Count -Expected 1 -Message "target start alignment count for $targetTransitionId"
    Assert-Equal -Actual ([bool]$alignments[0].available) -Expected $true -Message "target alignment availability"
    Assert-Equal -Actual ([string]$alignments[0].rate_relation) -Expected "fast_to_slow" -Message "target alignment relation"
    Assert-Equal -Actual ([string]$alignments[0].raw_strategy) -Expected "exact_or_substep" -Message "target alignment raw strategy"
}

$bindingEvents = @($scheduler.events | Where-Object {
    $null -ne $_.PSObject.Properties['node_id'] -and [string]$_.node_id -eq $targetNode -and [string]$_.event -eq 'input_binding'
})
$deferredBindings = @()
foreach ($event in $bindingEvents) {
    $deferredBindings += @($event.input_binding.bindings | Where-Object {
        [string]$_.binding_source -eq "runtime_rate_transition_alignment" -and
        [string]$_.rate_transition.transition_id -eq $targetTransitionId
    })
}
Assert-Equal -Actual $deferredBindings.Count -Expected $targetStart.Count -Message "runtime transition binding evidence count"

$readyEvents = @($scheduler.events | Where-Object {
    $null -ne $_.PSObject.Properties['node_id'] -and [string]$_.node_id -eq $targetNode -and [string]$_.event -eq 'ready_queue_admission'
})
$readyTransitionChecks = @()
foreach ($event in $readyEvents) {
    $readyTransitionChecks += @($event.port_check.checked_ports | Where-Object {
        [string]$_.rate_transition.transition_id -eq $targetTransitionId
    })
}
Assert-Equal -Actual $readyTransitionChecks.Count -Expected $targetStart.Count -Message "ready queue transition check count"

Write-Host "[OK] Phase2 rate transition runtime audit passed."
Write-Host "  run_dir = $runDir"
