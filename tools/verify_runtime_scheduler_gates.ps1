param(
    [string]$Python = "auto",

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',

    [string]$RunIdPrefix = "gate_runtime_scheduler_100",

    [switch]$SkipBuild,

    [switch]$StaticOnly,

    [switch]$SkipSlow,

    [switch]$RequireTypedZeroCopy
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
Import-Module (Join-Path $pdkRoot 'tools\PdkPython.psm1') -Force
$Python = Resolve-PdkPython -Python $Python -PdkRoot $pdkRoot
$failures = [System.Collections.Generic.List[string]]::new()

function Add-Failure {
    param([string]$Message)
    $failures.Add($Message) | Out-Null
}

function Assert-File {
    param(
        [Parameter(Mandatory=$true)][string]$PathValue,
        [Parameter(Mandatory=$true)][string]$Label
    )
    if (-not (Test-Path -LiteralPath $PathValue -PathType Leaf)) {
        Add-Failure "$Label missing: $PathValue"
    }
}

function Assert-TextContains {
    param(
        [Parameter(Mandatory=$true)][string]$PathValue,
        [Parameter(Mandatory=$true)][string]$Pattern,
        [Parameter(Mandatory=$true)][string]$Label
    )
    if (-not (Test-Path -LiteralPath $PathValue -PathType Leaf)) {
        Add-Failure "$Label source missing: $PathValue"
        return
    }
    $hit = Select-String -LiteralPath $PathValue -Pattern $Pattern -SimpleMatch -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $hit) {
        Add-Failure "$Label not found in $PathValue"
    }
}

function Assert-AnyTextContains {
    param(
        [Parameter(Mandatory=$true)][string[]]$PathValues,
        [Parameter(Mandatory=$true)][string]$Pattern,
        [Parameter(Mandatory=$true)][string]$Label
    )
    foreach ($pathValue in $PathValues) {
        if (-not (Test-Path -LiteralPath $pathValue -PathType Leaf)) {
            continue
        }
        $hit = Select-String -LiteralPath $pathValue -Pattern $Pattern -SimpleMatch -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $hit) {
            return
        }
    }
    Add-Failure "$Label not found in any checked source: $($PathValues -join ', ')"
}

function Invoke-GateStep {
    param(
        [Parameter(Mandatory=$true)][string]$Label,
        [Parameter(Mandatory=$true)][scriptblock]$Body
    )
    Write-Host ""
    Write-Host "== $Label =="
    try {
        & $Body
        Write-Host "[PASS] $Label"
    } catch {
        Add-Failure "$Label failed: $($_.Exception.Message)"
        Write-Host "[FAIL] $Label"
    }
}

function Read-JsonFile {
    param([Parameter(Mandatory=$true)][string]$PathValue)
    if (-not (Test-Path -LiteralPath $PathValue -PathType Leaf)) {
        throw "JSON file not found: $PathValue"
    }
    return Get-Content -LiteralPath $PathValue -Encoding UTF8 -Raw | ConvertFrom-Json
}

function Require-PositiveCount {
    param(
        [Parameter(Mandatory=$true)][int]$Count,
        [Parameter(Mandatory=$true)][string]$Label
    )
    if ($Count -le 0) {
        throw "$Label count is zero"
    }
}

function Get-PropertyOrNull {
    param(
        [Parameter(Mandatory=$true)]$ObjectValue,
        [Parameter(Mandatory=$true)][string]$Name
    )
    if ($null -eq $ObjectValue -or $null -eq $ObjectValue.PSObject.Properties[$Name]) {
        return $null
    }
    return $ObjectValue.PSObject.Properties[$Name].Value
}

function Test-TruthyProperty {
    param(
        [Parameter(Mandatory=$true)]$ObjectValue,
        [Parameter(Mandatory=$true)][string]$Name
    )
    $value = Get-PropertyOrNull -ObjectValue $ObjectValue -Name $Name
    return $value -eq $true -or [string]$value -eq 'true'
}

function Get-RuntimeEventKind {
    param([Parameter(Mandatory=$true)]$ObjectValue)
    $kind = Get-PropertyOrNull -ObjectValue $ObjectValue -Name 'runtime_event_kind'
    if ($null -ne $kind -and -not [string]::IsNullOrWhiteSpace([string]$kind)) {
        return [string]$kind
    }
    $kind = Get-PropertyOrNull -ObjectValue $ObjectValue -Name 'event_kind'
    if ($null -ne $kind -and -not [string]::IsNullOrWhiteSpace([string]$kind)) {
        return [string]$kind
    }
    return ''
}

function Assert-PlatformRuntimeStaticGate {
    $requiredFiles = @(
        'include\FlightEnvPlatformRuntime\time\RuntimeTimeTypes.hpp',
        'include\FlightEnvPlatformRuntime\time\RuntimeEventQueue.hpp',
        'include\FlightEnvPlatformRuntime\time\RuntimePortSampleBuffer.hpp',
        'include\FlightEnvPlatformRuntime\time\RuntimeInputAlignment.hpp',
        'include\FlightEnvPlatformRuntime\RuntimeEventLoop.hpp',
        'include\FlightEnvPlatformRuntime\RuntimeReadyQueueExecutor.hpp',
        'include\FlightEnvPlatformRuntime\RuntimePublicFrameBuilder.hpp',
        'include\FlightEnvPlatformRuntime\RuntimePublicFramePolicy.hpp',
        'include\FlightEnvPlatformRuntime\RuntimeTimelineMaterializer.hpp',
        'src\RuntimeTimeScheduler.cpp',
        'src\RuntimeEventLoop.cpp',
        'src\NativeWorkflowNodePreparation.cpp',
        'src\NativeWorkflowNodePreparation.hpp',
        'src\RuntimeReadyQueueExecutor.cpp',
        'src\RuntimePublicFrameBuilder.cpp',
        'src\RuntimePublicFramePolicy.cpp',
        'src\RuntimeTimelineMaterializer.cpp',
        'src\time\RuntimePortSampleBuffer.cpp',
        'src\time\RuntimeInputAlignment.cpp'
    )
    foreach ($relative in $requiredFiles) {
        Assert-File -PathValue (Join-Path $runtimeRoot $relative) -Label "Gate runtime component"
    }

    $runner = Join-Path $runtimeRoot 'src\NativeWorkflowRunner.cpp'
    $nodePreparation = Join-Path $runtimeRoot 'src\NativeWorkflowNodePreparation.cpp'
    Assert-TextContains -PathValue $runner -Pattern 'RuntimeEventLoop event_loop' -Label 'Gate B event loop hot path'
    Assert-TextContains -PathValue $runner -Pattern 'recordExternalObservationSample' -Label 'Gate B input_arrived sample-buffer ingestion'
    Assert-TextContains -PathValue $runner -Pattern 'prepareNativeWorkflowNodeInputs' -Label 'Gate D node input preparation hot path'
    Assert-TextContains -PathValue $nodePreparation -Pattern 'RuntimePortBindingResolver::resolve' -Label 'Gate D strict port binding hot path'
    Assert-TextContains -PathValue $nodePreparation -Pattern 'RuntimeInputAlignment::alignNodeInputs' -Label 'Gate D input alignment hot path'
    Assert-TextContains -PathValue $runner -Pattern 'ready_executor.admitNode' -Label 'Gate C ReadyQueue admission hot path'
    Assert-TextContains -PathValue $runner -Pattern 'RuntimePublicFrameBuilder::build' -Label 'Gate F public materialization hot path'

    $eventQueue = Join-Path $runtimeRoot 'include\FlightEnvPlatformRuntime\time\RuntimeEventQueue.hpp'
    $eventQueueImpl = Join-Path $runtimeRoot 'src\time\RuntimeEventQueue.cpp'
    foreach ($kind in @('input_arrived', 'node_due', 'public_tick', 'checkpoint_due', 'branch_triggered', 'stop_check_due')) {
        Assert-AnyTextContains -PathValues @($eventQueue, $eventQueueImpl) -Pattern $kind -Label "Gate B event kind $kind"
    }

    $sampleBuffer = Join-Path $runtimeRoot 'src\time\RuntimePortSampleBuffer.cpp'
    Assert-TextContains -PathValue $sampleBuffer -Pattern 'sample.time.nanoseconds' -Label 'Gate A sample buffer nanosecond comparison'
    Assert-TextContains -PathValue $sampleBuffer -Pattern 'RuntimeTimePoint::fromSeconds' -Label 'Gate A boundary seconds-to-timepoint conversion'

    $domainPattern = '(?i)\b(pressure|heatflux|damage|ablation|remaining_life|landing)\b'
    $domainHits = Get-ChildItem -LiteralPath (Join-Path $runtimeRoot 'src'), (Join-Path $runtimeRoot 'include') -Recurse -File |
        Where-Object { $_.Extension -in @('.cpp', '.hpp', '.h') } |
        Select-String -Pattern $domainPattern -ErrorAction SilentlyContinue
    foreach ($hit in $domainHits) {
        Add-Failure "Gate A platform-neutrality violation: $($hit.Path):$($hit.LineNumber): $($hit.Line.Trim())"
    }
}

function Assert-DocumentationGate {
    $designDoc = $null
    $designCandidates = @(Get-ChildItem -LiteralPath (Join-Path $workspaceRoot 'doc\design') -File -Filter '*.md' -ErrorAction SilentlyContinue |
        Where-Object {
            (Select-String -LiteralPath $_.FullName -Pattern 'RuntimeEventLoop' -SimpleMatch -Quiet -ErrorAction SilentlyContinue) -and
            (Select-String -LiteralPath $_.FullName -Pattern 'Gate A-G' -SimpleMatch -Quiet -ErrorAction SilentlyContinue)
        } |
        Select-Object -First 1)
    if ($designCandidates.Count -gt 0) {
        $designDoc = $designCandidates[0].FullName
    } else {
        $designDoc = Join-Path $workspaceRoot 'doc\design\platform_runtime_scheduler_gate_document_missing.md'
    }
    $runtimeReadme = Join-Path $runtimeRoot 'README.md'
    Assert-File -PathValue $designDoc -Label 'Gate G time scheduling design document'
    Assert-File -PathValue $runtimeReadme -Label 'Gate G platform runtime README'
    foreach ($term in @('RuntimeEventLoop', 'RuntimeReadyQueueExecutor', 'RuntimeInputAlignment', 'RuntimePublicFrameBuilder')) {
        Assert-TextContains -PathValue $designDoc -Pattern $term -Label "Gate G design doc term $term"
        Assert-TextContains -PathValue $runtimeReadme -Pattern $term -Label "Gate G README term $term"
    }
}

function Assert-SmokeEvidenceGate {
    param([Parameter(Mandatory=$true)][string]$Prefix)

    $chainDir = Join-Path $workspaceRoot "_local_artifacts\platform-runtime\mainline-runs\$Prefix"
    $summary = Read-JsonFile -PathValue (Join-Path $chainDir 'mainline_summary.json')
    if ([int]$summary.online.effective_frames -le 0) {
        throw 'Gate D online effective frame count is zero'
    }
    if ([int]$summary.prediction.run_count -le 0) {
        throw 'Gate D prediction branch count is zero'
    }

    $timeline = Read-JsonFile -PathValue (Join-Path $chainDir 'run_timeline_index.json')
    Require-PositiveCount -Count @($timeline.online_frames).Count -Label 'Gate F online public timeline'
    Require-PositiveCount -Count @($timeline.branch_steps).Count -Label 'Gate F branch steps'
    Require-PositiveCount -Count @($timeline.artifact_refs).Count -Label 'Gate F artifact refs'

    $schedulerFiles = @(Get-ChildItem -LiteralPath (Join-Path $workspaceRoot '_local_artifacts\platform-runtime\runtime-host-runs') -Recurse -Filter scheduler_timeline.json -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like "*$Prefix*" })
    Require-PositiveCount -Count $schedulerFiles.Count -Label 'Gate B scheduler timeline files'

    $events = @()
    foreach ($file in $schedulerFiles) {
        $scheduler = Read-JsonFile -PathValue $file.FullName
        $events += @($scheduler.events)
    }
    Require-PositiveCount -Count $events.Count -Label 'Gate B scheduler events'

    $droppedNodeDue = @($events | Where-Object {
        [string](Get-PropertyOrNull -ObjectValue $_ -Name 'event') -eq 'node_due_dropped'
    })
    if ($droppedNodeDue.Count -gt 0) {
        throw "Gate C observed dropped node_due events: $($droppedNodeDue.Count)"
    }

    foreach ($kind in @('checkpoint_due', 'stop_check_due')) {
        $count = @($events | Where-Object {
            (Get-RuntimeEventKind -ObjectValue $_) -eq $kind -or [string](Get-PropertyOrNull -ObjectValue $_ -Name 'event') -eq $kind
        }).Count
        Require-PositiveCount -Count $count -Label "Gate B event $kind"
    }

    $nodeDueStart = @($events | Where-Object {
        [string](Get-PropertyOrNull -ObjectValue $_ -Name 'event') -eq 'start' -and
        (Get-RuntimeEventKind -ObjectValue $_) -eq 'node_due'
    })
    Require-PositiveCount -Count $nodeDueStart.Count -Label 'Gate C node_due start events'

    $readyAdmissions = @($events | Where-Object { [string](Get-PropertyOrNull -ObjectValue $_ -Name 'event') -eq 'ready_queue_admission' })
    Require-PositiveCount -Count $readyAdmissions.Count -Label 'Gate C ready_queue_admission events'
    $acceptedAdmissions = @($readyAdmissions | Where-Object { Test-TruthyProperty -ObjectValue $_ -Name 'accepted' })
    Require-PositiveCount -Count $acceptedAdmissions.Count -Label 'Gate C accepted ReadyQueue admissions'

    $badAdmission = @($readyAdmissions | Where-Object {
        $null -eq $_.PSObject.Properties['dependency_check'] -or
        $null -eq $_.PSObject.Properties['port_check'] -or
        $null -eq $_.PSObject.Properties['resource_check'] -or
        $null -eq $_.PSObject.Properties['parallel_check'] -or
        $null -eq $_.PSObject.Properties['deadline_check']
    })
    if ($badAdmission.Count -gt 0) {
        throw "Gate C ReadyQueue admission evidence is incomplete: $($badAdmission.Count)"
    }

    $inputEvents = @($events | Where-Object {
        (Get-RuntimeEventKind -ObjectValue $_) -eq 'input_arrived' -or
        [string](Get-PropertyOrNull -ObjectValue $_ -Name 'event') -eq 'input_arrived'
    })
    if ($inputEvents.Count -gt 0) {
        $recordedInputs = @($inputEvents | Where-Object { Test-TruthyProperty -ObjectValue $_ -Name 'sample_recorded' })
        Require-PositiveCount -Count $recordedInputs.Count -Label 'Gate B input_arrived sample-buffer records'
        $missingInputNs = @($recordedInputs | Where-Object { $null -eq $_.PSObject.Properties['sample_time_ns'] })
        if ($missingInputNs.Count -gt 0) {
            throw "Gate A input sample events missing sample_time_ns: $($missingInputNs.Count)"
        }
    } else {
        $committedOnlineFrames = @($timeline.online_frames | Where-Object {
            $null -ne $_.PSObject.Properties['posterior_checkpoint'] -and
            [string](Get-PropertyOrNull -ObjectValue $_ -Name 'source') -eq 'runtime_loop_summary'
        })
        Require-PositiveCount -Count $committedOnlineFrames.Count -Label 'Gate B online frame input commits'
    }

    $inputBindings = @($events | Where-Object { [string](Get-PropertyOrNull -ObjectValue $_ -Name 'event') -eq 'input_binding' })
    Require-PositiveCount -Count $inputBindings.Count -Label 'Gate D input binding evidence'
    $readyBindings = @($inputBindings | Where-Object {
        $null -ne $_.PSObject.Properties['branch_id'] -and
        $null -ne $_.PSObject.Properties['timeline_id'] -and
        $null -ne $_.PSObject.Properties['event_time_ns']
    })
    Require-PositiveCount -Count $readyBindings.Count -Label 'Gate D branch/time indexed input binding evidence'

    $finishEvents = @($events | Where-Object {
        [string](Get-PropertyOrNull -ObjectValue $_ -Name 'event') -eq 'loop_iteration_finish' -and
        (Get-RuntimeEventKind -ObjectValue $_) -eq 'public_tick'
    })
    Require-PositiveCount -Count $finishEvents.Count -Label 'Gate F public_tick materialization events'
}

Invoke-GateStep -Label 'Gate A/G static architecture and documentation' -Body {
    Assert-PlatformRuntimeStaticGate
    Assert-DocumentationGate
}

if (-not $StaticOnly) {
    Invoke-GateStep -Label 'Gate B build platform runtime' -Body {
        if (-not $SkipBuild) {
            & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $runtimeRoot 'tools\build_platform_runtime.ps1') `
                -Configuration $Configuration `
                -Platform $Platform
            if ($LASTEXITCODE -ne 0) {
                throw "platform runtime build failed with exit code $LASTEXITCODE"
            }
        } else {
            Write-Host 'SkipBuild set; build gate uses existing artifacts.'
        }
    }

    Invoke-GateStep -Label 'Gate F negative binding and failure-policy audit' -Body {
        $negativeAudit = Join-Path $pdkRoot 'tools\audit_edge_binding_negative.ps1'
        if (-not (Test-Path -LiteralPath $negativeAudit -PathType Leaf)) {
            throw "negative edge binding audit missing: $negativeAudit"
        }
        & powershell -NoProfile -ExecutionPolicy Bypass -File $negativeAudit -Python $Python
        if ($LASTEXITCODE -ne 0) {
            throw "negative edge binding audit failed with exit code $LASTEXITCODE"
        }
    }

    Invoke-GateStep -Label 'Gate C synthetic non-integer multirate scheduling' -Body {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $runtimeRoot 'tools\audit_multirate_runtime_round2.ps1') `
            -Python $Python `
            -Configuration $Configuration `
            -Platform $Platform `
            -RunIdPrefix "$RunIdPrefix.synthetic" `
            -SkipBuild
        if ($LASTEXITCODE -ne 0) {
            throw "multirate runtime audit failed with exit code $LASTEXITCODE"
        }
    }

    if (-not $SkipSlow) {
        Invoke-GateStep -Label 'Gate D/E/F real object online future branch smoke' -Body {
            $externalStream = Join-Path $objectRoot 'fixtures\sensor_stream_db70.json'
            & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $runtimeRoot 'tools\run_cpp_runtime_host_smoke.ps1') `
                -Python $Python `
                -Configuration $Configuration `
                -Platform $Platform `
                -RunIdPrefix "$RunIdPrefix.real" `
                -OnlineFrames 4 `
                -PredictionEveryFrames 2 `
                -FutureMaxIterations 2 `
                -BranchChunkIterations 2 `
                -ExternalObservationStream $externalStream `
                -SkipBuild `
                -ZeroCopyMode auto `
                -TypedBufferPersistence shadow_artifact
            if ($LASTEXITCODE -ne 0) {
                throw "real object runtime smoke failed with exit code $LASTEXITCODE"
            }
            Assert-SmokeEvidenceGate -Prefix "$RunIdPrefix.real"
        }

        if ($RequireTypedZeroCopy) {
            Invoke-GateStep -Label 'Gate E typed zero-copy same-process audit' -Body {
                $externalStream = Join-Path $objectRoot 'fixtures\sensor_stream_db70.json'
                & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $runtimeRoot 'tools\run_cpp_runtime_host_smoke.ps1') `
                    -Python $Python `
                    -Configuration $Configuration `
                    -Platform $Platform `
                    -RunIdPrefix "$RunIdPrefix.zerocopy" `
                    -OnlineFrames 2 `
                    -PredictionEveryFrames 2 `
                    -FutureMaxIterations 1 `
                    -BranchChunkIterations 1 `
                    -ExternalObservationStream $externalStream `
                    -SkipBuild `
                    -ZeroCopyMode require `
                    -TypedBufferPersistence memory_only `
                    -RequireTypedZeroCopy
                if ($LASTEXITCODE -ne 0) {
                    throw "typed zero-copy smoke failed with exit code $LASTEXITCODE"
                }
            }
        }
    } else {
        Write-Host ''
        Write-Host 'SkipSlow set; Gate D/E/F real object runtime smoke was not executed.'
    }
}

Write-Host ''
Write-Host 'FlightEnv platform runtime scheduler gates'
Write-Host "  runtime    = $runtimeRoot"
Write-Host "  workspace  = $workspaceRoot"
Write-Host "  config     = $Configuration|$Platform"
Write-Host "  run prefix = $RunIdPrefix"

if ($failures.Count -gt 0) {
    Write-Host ''
    Write-Host 'Failures:'
    foreach ($failure in $failures) {
        Write-Host "  - $failure"
    }
    exit 1
}

Write-Host ''
Write-Host 'Result: Gate A-G runtime scheduler acceptance passed.'
