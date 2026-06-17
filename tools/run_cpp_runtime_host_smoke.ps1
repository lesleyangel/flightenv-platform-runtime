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

    [string]$ExternalObservationStream = "",

    [string]$AdapterRegistry = "",

    [switch]$PreflightAdapters,

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
$deadline = (Get-Date).AddSeconds($BranchWaitTimeoutSeconds)
do {
    $summary = Get-Content -LiteralPath $summaryPath -Encoding UTF8 -Raw | ConvertFrom-Json
    $prediction = $summary.prediction
    $running = 0
    $runCount = 0
    if ($null -ne $prediction.PSObject.Properties['running_branch_count']) {
        $running = [int]$prediction.running_branch_count
    }
    if ($null -ne $prediction.PSObject.Properties['run_count']) {
        $runCount = [int]$prediction.run_count
    }
    if ($runCount -gt 0 -and $running -eq 0) {
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

Write-Host "[OK] C++ Runtime Host smoke passed."
Write-Host "  evidence = $chainDir"
