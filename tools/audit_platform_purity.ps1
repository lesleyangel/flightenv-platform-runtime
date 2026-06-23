param(
    [string]$ReportPath = ''
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
$workspaceHome = [System.IO.Path]::GetFullPath((Join-Path $runtimeRoot '..'))
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $workspaceHome '_local_artifacts\platform-runtime\scheduler-acceptance\platform_purity_audit.json'
}

$scanRoots = @(
    (Join-Path $runtimeRoot 'include'),
    (Join-Path $runtimeRoot 'src')
)
$domainPattern = '\b(pressure|heatflux|damage|ablation|remaining_life|landing|trajectory|shell)\b'
$violations = @()

foreach ($root in $scanRoots) {
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        continue
    }
    $files = Get-ChildItem -LiteralPath $root -Recurse -File |
        Where-Object { $_.Extension -in @('.h', '.hpp', '.hh', '.c', '.cc', '.cpp', '.cxx') }
    foreach ($file in $files) {
        $matches = @(Select-String -LiteralPath $file.FullName -Pattern $domainPattern -AllMatches -CaseSensitive:$false)
        foreach ($match in $matches) {
            $violations += [pscustomobject]@{
                file = $file.FullName
                line = $match.LineNumber
                text = $match.Line.Trim()
            }
        }
    }
}

$reportDir = Split-Path -Parent $ReportPath
New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
$report = [ordered]@{
    schema_version = 'flightenv.platform.purity_audit.v1'
    result = if ($violations.Count -eq 0) { 'pass' } else { 'fail' }
    scanned_roots = $scanRoots
    forbidden_pattern = $domainPattern
    violation_count = $violations.Count
    violations = $violations
    note = 'Platform runtime include/src must stay object-neutral. Object semantics belong in object packages, adapters, tests, examples, or object-owned evidence.'
}
$report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $ReportPath -Encoding UTF8

if ($violations.Count -gt 0) {
    Write-Error "Platform purity audit failed. Report: $ReportPath"
}

Write-Host "Platform purity audit passed. Report: $ReportPath"
