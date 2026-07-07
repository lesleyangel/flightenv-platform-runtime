param(
    [string]$RunIdPrefix = 'gate_runtime_scheduler_100',

    [int]$MinEventCount = 10000,

    [int]$MinBranchCount = 64,

    [double]$ObservedGateSeconds = -1.0,

    [double]$MaxGateSeconds = 900.0,

    [switch]$RequireMemoryOnlyTypedBuffers
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
$acceptanceRoot = Join-Path $workspaceRoot '_local_artifacts\platform-runtime\scheduler-acceptance'
$stressReportPath = Join-Path $acceptanceRoot 'scheduler_stress_fault_backpressure_audit.json'
$reportPath = Join-Path $acceptanceRoot 'scheduler_performance_budget_audit.json'
$runtimeRunRoot = Join-Path $workspaceRoot '_local_artifacts\platform-runtime\runtime-host-runs'

function Read-JsonFile {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "JSON file not found: $Path"
    }
    return Get-Content -LiteralPath $Path -Encoding UTF8 -Raw | ConvertFrom-Json
}

$stress = Read-JsonFile $stressReportPath
$runtimeOutputFiles = @(Get-ChildItem -LiteralPath $runtimeRunRoot -Recurse -Filter runtime_outputs.json -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -like "*$RunIdPrefix*" })

$typedExecuteCount = 0
$defaultAbiCount = 0
$maxExtraLogicalRefCount = 0
$maxShadowArtifactCount = 0
$maxMemoryOnlyShadowArtifactCount = 0
$maxMemoryOnlyCount = 0
foreach ($file in $runtimeOutputFiles) {
    $runtimeOutputs = Read-JsonFile $file.FullName
    if ($null -ne $runtimeOutputs.PSObject.Properties['typed_buffer_store']) {
        $store = $runtimeOutputs.typed_buffer_store
        if ($null -ne $store.PSObject.Properties['extra_logical_ref_count']) {
            $maxExtraLogicalRefCount = [Math]::Max($maxExtraLogicalRefCount, [int]$store.extra_logical_ref_count)
        }
        if ($null -ne $store.PSObject.Properties['shadow_artifact_count']) {
            $maxShadowArtifactCount = [Math]::Max($maxShadowArtifactCount, [int]$store.shadow_artifact_count)
        }
        if ($null -ne $store.PSObject.Properties['memory_only_count']) {
            $maxMemoryOnlyCount = [Math]::Max($maxMemoryOnlyCount, [int]$store.memory_only_count)
        }
        if ($null -ne $store.PSObject.Properties['memory_only_count'] -and
            $null -ne $store.PSObject.Properties['shadow_artifact_count'] -and
            [int]$store.memory_only_count -gt 0) {
            $maxMemoryOnlyShadowArtifactCount = [Math]::Max(
                $maxMemoryOnlyShadowArtifactCount,
                [int]$store.shadow_artifact_count
            )
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

$checks = @()
$checks += [pscustomobject]@{
    name = 'stress_report_pass'
    passed = ([string]$stress.result -eq 'pass' -and [int]$stress.failure_count -eq 0)
    evidence = @{ result = $stress.result; failure_count = [int]$stress.failure_count }
}
$checks += [pscustomobject]@{
    name = 'stress_event_count_budget'
    passed = ([int]$stress.event_count -ge $MinEventCount)
    evidence = @{ event_count = [int]$stress.event_count; min_event_count = $MinEventCount }
}
$checks += [pscustomobject]@{
    name = 'stress_branch_count_budget'
    passed = ([int]$stress.branch_count -ge $MinBranchCount)
    evidence = @{ branch_count = [int]$stress.branch_count; min_branch_count = $MinBranchCount }
}
$checks += [pscustomobject]@{
    name = 'typed_zero_copy_observed'
    passed = ($typedExecuteCount -gt 0 -and $defaultAbiCount -eq 0)
    evidence = @{ typed_execute_count = $typedExecuteCount; default_abi_count = $defaultAbiCount }
}
$checks += [pscustomobject]@{
    name = 'typed_buffer_reference_budget'
    passed = ($maxExtraLogicalRefCount -eq 0)
    evidence = @{ max_extra_logical_ref_count = $maxExtraLogicalRefCount }
}
if ($RequireMemoryOnlyTypedBuffers) {
    $checks += [pscustomobject]@{
        name = 'memory_only_shadow_artifact_budget'
        passed = ($maxMemoryOnlyShadowArtifactCount -eq 0 -and $maxMemoryOnlyCount -gt 0)
        evidence = @{
            max_shadow_artifact_count = $maxShadowArtifactCount
            max_memory_only_shadow_artifact_count = $maxMemoryOnlyShadowArtifactCount
            max_memory_only_count = $maxMemoryOnlyCount
        }
    }
}
if ($ObservedGateSeconds -ge 0.0) {
    $checks += [pscustomobject]@{
        name = 'final_gate_wall_clock_budget'
        passed = ($ObservedGateSeconds -le $MaxGateSeconds)
        evidence = @{ observed_gate_seconds = $ObservedGateSeconds; max_gate_seconds = $MaxGateSeconds }
    }
}

$failed = @($checks | Where-Object { -not $_.passed })
$report = [ordered]@{
    schema_version = 'flightenv.platform.scheduler_performance_budget_audit.v1'
    result = if ($failed.Count -eq 0) { 'pass' } else { 'fail' }
    run_id_prefix = $RunIdPrefix
    stress_report_path = $stressReportPath
    runtime_output_file_count = $runtimeOutputFiles.Count
    typed_execute_count = $typedExecuteCount
    default_abi_count = $defaultAbiCount
    max_extra_logical_ref_count = $maxExtraLogicalRefCount
    max_shadow_artifact_count = $maxShadowArtifactCount
    max_memory_only_shadow_artifact_count = $maxMemoryOnlyShadowArtifactCount
    max_memory_only_count = $maxMemoryOnlyCount
    checks = $checks
}
$report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $reportPath -Encoding UTF8

if ($failed.Count -gt 0) {
    throw "Scheduler performance budget audit failed. Report: $reportPath"
}

Write-Host "Scheduler performance budget audit passed. Report: $reportPath"
