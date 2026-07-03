param(
    [string]$Python = "auto",

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',

    [string]$RunIdPrefix = "m2_runtime_cleanup",

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
$adapterRegistry = Join-Path $objectRoot 'tools\adapter_registries\ballistic_adapters.local.json'
$exe = Join-Path $workspaceRoot "_deps\workspace\$Platform\$Configuration\FlightEnvPlatformRuntimeHost.exe"
$chainDir = Join-Path $workspaceRoot "_local_artifacts\platform-runtime\mainline-runs\$RunIdPrefix"
$runRoot = Join-Path $workspaceRoot '_local_artifacts\platform-runtime\runtime-host-runs'
$reportRoot = Join-Path $workspaceRoot '_local_artifacts\platform-runtime\scheduler-acceptance'
$reportPath = Join-Path $reportRoot 'runtime_m2_cleanup_audit.json'

Import-Module (Join-Path $pdkRoot 'tools\PdkPython.psm1') -Force
$Python = Resolve-PdkPython -Python $Python -PdkRoot $pdkRoot

function Read-JsonFile {
    param([Parameter(Mandatory = $true)][string]$PathValue)
    if (-not (Test-Path -LiteralPath $PathValue -PathType Leaf)) {
        throw "JSON file not found: $PathValue"
    }
    return Get-Content -LiteralPath $PathValue -Encoding UTF8 -Raw | ConvertFrom-Json
}

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

function Get-Prop {
    param(
        [object]$ObjectValue,
        [Parameter(Mandatory = $true)][string]$Name,
        [object]$DefaultValue = $null
    )
    if ($null -eq $ObjectValue) {
        return $DefaultValue
    }
    $prop = $ObjectValue.PSObject.Properties[$Name]
    if ($null -eq $prop) {
        return $DefaultValue
    }
    return $prop.Value
}

function Invoke-CheckedPowershell {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    $output = & powershell @Arguments 2>&1
    foreach ($line in $output) {
        Write-Host $line
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Child PowerShell command failed with exit code $LASTEXITCODE"
    }
}

New-Item -ItemType Directory -Force -Path $reportRoot | Out-Null

$smokeArgs = @(
    '-NoProfile',
    '-ExecutionPolicy',
    'Bypass',
    '-File',
    (Join-Path $runtimeRoot 'tools\run_cpp_runtime_host_smoke.ps1'),
    '-Python',
    $Python,
    '-Configuration',
    $Configuration,
    '-Platform',
    $Platform,
    '-RunIdPrefix',
    $RunIdPrefix,
    '-OnlineFrames',
    '4',
    '-PredictionEveryFrames',
    '2',
    '-FutureMaxIterations',
    '1',
    '-BranchChunkIterations',
    '1',
    '-ExecutionBackend',
    'native_adapter_sessions'
)
if ($SkipBuild) {
    $smokeArgs += '-SkipBuild'
}
Invoke-CheckedPowershell -Arguments $smokeArgs

$runtimeHostEvidencePath = Join-Path $chainDir 'runtime_host_evidence.json'
$summaryPath = Join-Path $chainDir 'mainline_summary.json'
$runtimeHostEvidence = Read-JsonFile -PathValue $runtimeHostEvidencePath
$summary = Read-JsonFile -PathValue $summaryPath
$hostEvidence = $runtimeHostEvidence.host
$externalStream = [string]$summary.online.external_observation_stream

Assert-True ($hostEvidence.execution_backend -eq 'native_adapter_sessions') 'M2 audit expected native_adapter_sessions backend'
Assert-True ($hostEvidence.execution_backend_class -eq 'production_native_adapter_sessions') 'runtime_host_evidence missing production backend classification'
Assert-True ($hostEvidence.compatibility_status -eq 'production') 'runtime_host_evidence compatibility_status must be production'
Assert-True ([bool]$hostEvidence.in_process_adapter_sessions) 'native backend should use in-process adapter sessions'
Assert-True (-not [bool]$hostEvidence.legacy_process_backend_allowed) 'legacy process backend must be disabled by default'
Assert-True (-not [bool]$hostEvidence.legacy_process_backend_required_flag) 'native run must not require legacy process backend flag'
Assert-True (Test-Path -LiteralPath $externalStream -PathType Leaf) "external observation stream does not exist: $externalStream"
Assert-True (-not ($externalStream -like '*_local_artifacts*platform-pdk*runtime-host-runs*')) 'default production smoke must not depend on historical PDK runtime-host-runs stream'

$scheduleTraceDir = Join-Path $chainDir 'schedule_trace_m2'
Invoke-CheckedPowershell -Arguments @(
    '-NoProfile',
    '-ExecutionPolicy',
    'Bypass',
    '-File',
    (Join-Path $runtimeRoot 'tools\export_schedule_trace.ps1'),
    '-RunDir',
    $chainDir,
    '-RunIdPrefix',
    $RunIdPrefix,
    '-OutputDir',
    $scheduleTraceDir
)
$scheduleTracePath = Join-Path $scheduleTraceDir 'schedule_timeline.json'
$scheduleTrace = Read-JsonFile -PathValue $scheduleTracePath
$unknownPeriodItems = @($scheduleTrace.items | Where-Object { (Get-Prop -ObjectValue $_ -Name 'source_period_known' -DefaultValue $true) -ne $true })
$badFallbackItems = @($scheduleTrace.items | Where-Object {
    (Get-Prop -ObjectValue $_ -Name 'source_period_source' -DefaultValue '') -eq 'default_fallback_2s' -or
    ((Get-Prop -ObjectValue $_ -Name 'source_period_known' -DefaultValue $true) -ne $true -and
     $null -ne (Get-Prop -ObjectValue $_ -Name 'source_period_s' -DefaultValue $null))
})
Assert-True ($badFallbackItems.Count -eq 0) 'schedule trace still contains a silent 2.0s fallback or non-null unknown period'

$legacyRejectChainDir = Join-Path $workspaceRoot "_local_artifacts\platform-runtime\mainline-runs\$RunIdPrefix.legacy_reject"
$legacyArgs = @(
    '--workspace-root', $workspaceRoot,
    '--pdk-root', $pdkRoot,
    '--object-package-root', $objectRoot,
    '--compiled-online', $compiledOnline,
    '--compiled-future', $compiledFuture,
    '--adapter-registry', $adapterRegistry,
    '--run-id-prefix', "$RunIdPrefix.legacy_reject",
    '--run-root', $runRoot,
    '--chain-dir', $legacyRejectChainDir,
    '--python', $Python,
    '--execution-backend', 'compiled_workflow_process_backend',
    '--prepare-only'
)
$previousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $legacyOutput = & $exe @legacyArgs 2>&1
    $legacyExitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $previousErrorActionPreference
}
foreach ($line in $legacyOutput) {
    Write-Host $line
}
Assert-True ($legacyExitCode -ne 0) 'compiled_workflow_process_backend unexpectedly ran without --allow-legacy-process-backend'
Assert-True (([string]($legacyOutput -join "`n")) -match 'deprecated and disabled by default') 'legacy backend rejection message did not explain the explicit compatibility flag'

$report = [ordered]@{
    schema_version = 'flightenv.platform.runtime_m2_cleanup_audit.v1'
    generated_at_utc = (Get-Date).ToUniversalTime().ToString('o')
    status = 'passed'
    run_id_prefix = $RunIdPrefix
    chain_dir = $chainDir
    runtime_host_evidence = $runtimeHostEvidencePath
    mainline_summary = $summaryPath
    schedule_trace = $scheduleTracePath
    production_backend = $hostEvidence.execution_backend
    production_backend_class = $hostEvidence.execution_backend_class
    compatibility_status = $hostEvidence.compatibility_status
    external_observation_stream = $externalStream
    schedule_trace_unknown_period_item_count = $unknownPeriodItems.Count
    legacy_backend_rejected_without_flag = $true
}
$report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $reportPath -Encoding UTF8

Write-Host '[OK] Runtime M2 cleanup audit passed.'
Write-Host "  report = $reportPath"
