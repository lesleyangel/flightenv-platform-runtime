param(
    [Parameter(Mandatory = $true)]
    [string]$RunRoot,

    [double]$MinGrowthRatio = 1.0,

    [switch]$WriteReport
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
    param([Parameter(Mandatory = $true)][string]$PathValue)
    if (-not (Test-Path -LiteralPath $PathValue -PathType Leaf)) {
        throw "JSON file not found: $PathValue"
    }
    return Get-Content -LiteralPath $PathValue -Raw | ConvertFrom-Json
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

$RunRoot = [System.IO.Path]::GetFullPath($RunRoot)
Assert-True (Test-Path -LiteralPath $RunRoot -PathType Container) "Run root not found: $RunRoot"

$evidenceFiles = @(Get-ChildItem -Path $RunRoot -Recurse -Filter 'forecast_uncertainty_evidence.json' |
    Sort-Object FullName)
Assert-True ($evidenceFiles.Count -gt 0) "No forecast_uncertainty_evidence.json files found under $RunRoot"

$rows = @()
$failures = @()
foreach ($file in $evidenceFiles) {
    $payload = Read-JsonFile -PathValue $file.FullName
    $summary = $payload.summary
    $events = @($payload.events)
    $seedTrace = [double]$summary.seed_covariance_trace
    $latestTrace = [double]$summary.latest_covariance_trace
    $growthRatio = [double]$summary.growth_ratio
    $stepCount = [int]$summary.step_count
    $enabled = [bool]$summary.enabled

    $checkpointPath = Join-Path $file.DirectoryName 'state_checkpoint.json'
    $checkpointPayload = Read-JsonFile -PathValue $checkpointPath
    $forecastCheckpoints = @($checkpointPayload.checkpoints |
        Where-Object { $_.checkpoint_kind -eq 'forecast_predict_only' })

    if (-not $enabled) {
        $failures += "$($file.FullName): forecast uncertainty tracker is disabled"
    }
    if ($stepCount -le 0) {
        $failures += "$($file.FullName): step_count must be > 0"
    }
    if ($events.Count -ne $stepCount) {
        $failures += "$($file.FullName): event count $($events.Count) does not match step_count $stepCount"
    }
    if ($forecastCheckpoints.Count -ne $stepCount) {
        $failures += "$($file.FullName): forecast checkpoint count $($forecastCheckpoints.Count) does not match step_count $stepCount"
    }
    if ($latestTrace -le $seedTrace) {
        $failures += "$($file.FullName): covariance trace did not grow ($seedTrace -> $latestTrace)"
    }
    if ($growthRatio -le $MinGrowthRatio) {
        $failures += "$($file.FullName): growth_ratio $growthRatio <= MinGrowthRatio $MinGrowthRatio"
    }

    $rows += [pscustomobject]@{
        path = $file.FullName
        enabled = $enabled
        step_count = $stepCount
        seed_covariance_trace = $seedTrace
        latest_covariance_trace = $latestTrace
        growth_ratio = $growthRatio
        event_count = $events.Count
        forecast_checkpoint_count = $forecastCheckpoints.Count
    }
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ }
    throw "Forecast uncertainty evidence verification failed with $($failures.Count) failure(s)."
}

$report = [pscustomobject]@{
    schema_version = 'flightenv.platform.forecast_uncertainty_verification.v1'
    run_root = $RunRoot
    evidence_file_count = $evidenceFiles.Count
    min_growth_ratio = $MinGrowthRatio
    status = 'passed'
    rows = $rows
}

if ($WriteReport) {
    $reportPath = Join-Path $RunRoot 'forecast_uncertainty_verification.json'
    $report | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $reportPath -Encoding UTF8
    Write-Host "[OK] forecast uncertainty evidence verification passed."
    Write-Host "  report = $reportPath"
} else {
    Write-Host "[OK] forecast uncertainty evidence verification passed."
}
$rows | Format-Table -AutoSize
