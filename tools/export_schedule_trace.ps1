param(
    [string]$RunDir = "",

    [string]$RunIdPrefix = "",

    [string]$RuntimeRunsRoot = "",

    [string]$OutputDir = "",

    [ValidateSet("ModelTime", "ExecutionTime", "WallClockApprox")]
    [string]$TimeAxis = "ModelTime",

    [switch]$Open
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

$runtimeRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$workspaceRoot = [System.IO.Path]::GetFullPath((Join-Path $runtimeRoot ".."))

function Resolve-PathFromRoot {
    param(
        [Parameter(Mandatory = $true)][string]$PathValue,
        [Parameter(Mandatory = $true)][string]$BasePath
    )
    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return [System.IO.Path]::GetFullPath($PathValue)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $PathValue))
}

function Read-JsonFile {
    param([Parameter(Mandatory = $true)][string]$PathValue)
    return Get-Content -Raw -Encoding UTF8 -LiteralPath $PathValue | ConvertFrom-Json
}

function Json-Prop {
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
    if ($null -eq $prop.Value) {
        return $DefaultValue
    }
    return $prop.Value
}

function String-Prop {
    param([object]$ObjectValue, [string]$Name, [string]$DefaultValue = "")
    $value = Json-Prop -ObjectValue $ObjectValue -Name $Name -DefaultValue $DefaultValue
    if ($null -eq $value) {
        return $DefaultValue
    }
    return [string]$value
}

function Number-Prop {
    param([object]$ObjectValue, [string]$Name, [double]$DefaultValue = 0.0)
    $value = Json-Prop -ObjectValue $ObjectValue -Name $Name -DefaultValue $DefaultValue
    if ($null -eq $value) {
        return $DefaultValue
    }
    try {
        return [double]$value
    } catch {
        return $DefaultValue
    }
}

function Html-Escape {
    param([object]$Value)
    return [System.Net.WebUtility]::HtmlEncode([string]$Value)
}

function Get-StageName {
    param([string]$NodeId)
    if ($NodeId -match "\.state_transition\.") { return "State transition" }
    if ($NodeId -match "\.observation_equation\.") { return "Observation" }
    if ($NodeId -match "\.online_estimation\.") { return "Estimation" }
    if ($NodeId -match "\.posterior_field_reconstruction\.") { return "Posterior fields" }
    if ($NodeId -match "\.failure_qoi\.") { return "QoI" }
    if ($NodeId -match "^future_step\.") { return "Future prediction" }
    return "Other"
}

function Get-StageColor {
    param([string]$Stage)
    switch ($Stage) {
        "State transition" { return "#2563eb" }
        "Observation" { return "#0891b2" }
        "Estimation" { return "#7c3aed" }
        "Posterior fields" { return "#16a34a" }
        "QoI" { return "#dc2626" }
        "Future prediction" { return "#ea580c" }
        default { return "#64748b" }
    }
}

function Short-NodeName {
    param([string]$NodeId)
    if ([string]::IsNullOrWhiteSpace($NodeId)) { return "" }
    $parts = $NodeId -split "\."
    if ($parts.Count -le 2) { return $NodeId }
    return ($parts[-2] + "." + $parts[-1])
}

function Parse-EventTimestampMs {
    param([object]$EventValue, [double]$FallbackMs)
    $timestamp = String-Prop -ObjectValue $EventValue -Name "timestamp_utc"
    if (-not [string]::IsNullOrWhiteSpace($timestamp)) {
        try {
            return [double]([DateTimeOffset]::Parse($timestamp).ToUnixTimeMilliseconds())
        } catch {
            return $FallbackMs
        }
    }
    return $FallbackMs
}

function Match-PathInt {
    param([string]$Text, [string]$Pattern)
    if ([string]::IsNullOrWhiteSpace($Text)) {
        return -1
    }
    if ($Text -match $Pattern) {
        return [int]$Matches[1]
    }
    return -1
}

function Get-EventPeriodInfo {
    param([object]$EventValue, [object]$PredictionRunInfo)
    $period = [double](Number-Prop -ObjectValue $EventValue -Name "effective_delta_t_s" -DefaultValue 0.0)
    if ($period -gt 0.0) {
        return [pscustomobject]@{ period_s = $period; known = $true; source = "event.effective_delta_t_s" }
    }
    $period = [double](Number-Prop -ObjectValue $EventValue -Name "output_period_s" -DefaultValue 0.0)
    if ($period -gt 0.0) {
        return [pscustomobject]@{ period_s = $period; known = $true; source = "event.output_period_s" }
    }
    if ($null -ne $PredictionRunInfo) {
        $period = [double](Number-Prop -ObjectValue $PredictionRunInfo -Name "base_dt_s" -DefaultValue 0.0)
        if ($period -gt 0.0) {
            return [pscustomobject]@{ period_s = $period; known = $true; source = "prediction_run.base_dt_s" }
        }
        $period = [double](Number-Prop -ObjectValue $PredictionRunInfo -Name "output_period_s" -DefaultValue 0.0)
        if ($period -gt 0.0) {
            return [pscustomobject]@{ period_s = $period; known = $true; source = "prediction_run.output_period_s" }
        }
    }
    return [pscustomobject]@{ period_s = 0.0; known = $false; source = "unknown" }
}

function Resolve-ModelTime {
    param(
        [string]$FilePath,
        [object]$StartEvent,
        [hashtable]$PredictionRunByFrame
    )

    $localTimeS = [double](Number-Prop -ObjectValue $StartEvent -Name "runtime_event_time_s" -DefaultValue (Number-Prop -ObjectValue $StartEvent -Name "dispatch_time_s" -DefaultValue 0.0))
    $onlineFrameIndex = Match-PathInt -Text $FilePath -Pattern "online_frame_(\d+)"
    $predictFrameIndex = Match-PathInt -Text $FilePath -Pattern "predict_frame_(\d+)"
    $chunkIndex = Match-PathInt -Text $FilePath -Pattern "chunk_(\d+)"
    $predictionRun = $null
    if ($predictFrameIndex -ge 0 -and $PredictionRunByFrame.ContainsKey($predictFrameIndex)) {
        $predictionRun = $PredictionRunByFrame[$predictFrameIndex]
    }
    $periodInfo = Get-EventPeriodInfo -EventValue $StartEvent -PredictionRunInfo $predictionRun
    $periodS = [double]$periodInfo.period_s

    $modelTimeS = $localTimeS
    $origin = "workflow_local"
    if ($onlineFrameIndex -ge 0) {
        if ([bool]$periodInfo.known) {
            $modelTimeS = ([double]$onlineFrameIndex * $periodS) + $localTimeS
            $origin = "online_frame_time"
        } else {
            $modelTimeS = $localTimeS
            $origin = "online_frame_time_unknown_period"
        }
    } elseif ($predictFrameIndex -ge 0 -and $null -ne $predictionRun) {
        $triggerTimeS = [double](Number-Prop -ObjectValue $predictionRun -Name "trigger_time_s" -DefaultValue 0.0)
        $chunkOffsetS = if ($chunkIndex -ge 0 -and [bool]$periodInfo.known) { [double]$chunkIndex * $periodS } else { 0.0 }
        $modelTimeS = $triggerTimeS + $chunkOffsetS + $localTimeS
        $origin = if ([bool]$periodInfo.known) { "prediction_branch_time" } else { "prediction_branch_time_unknown_period" }
    }

    return [pscustomobject]@{
        local_time_s = [math]::Round($localTimeS, 6)
        model_time_s = [math]::Round($modelTimeS, 6)
        period_s = if ([bool]$periodInfo.known) { [math]::Round($periodS, 6) } else { $null }
        period_known = [bool]$periodInfo.known
        period_source = [string]$periodInfo.source
        origin = $origin
        online_frame_index = $onlineFrameIndex
        predict_frame_index = $predictFrameIndex
        chunk_index = $chunkIndex
    }
}

function New-ItemKey {
    param([object]$EventValue)
    $nodeId = String-Prop -ObjectValue $EventValue -Name "node_id"
    $loop = [int](Number-Prop -ObjectValue $EventValue -Name "loop_iteration_index" -DefaultValue -1)
    $eventId = String-Prop -ObjectValue $EventValue -Name "runtime_event_id"
    return "$nodeId|$loop|$eventId"
}

if ([string]::IsNullOrWhiteSpace($RunDir)) {
    $mainlineRoot = Join-Path $workspaceRoot "_local_artifacts\platform-runtime\mainline-runs"
    $latest = Get-ChildItem -LiteralPath $mainlineRoot -Directory |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $latest) {
        throw "No platform mainline run directory found under $mainlineRoot"
    }
    $RunDir = $latest.FullName
}
$RunDir = Resolve-PathFromRoot -PathValue $RunDir -BasePath $workspaceRoot
if (-not (Test-Path -LiteralPath $RunDir -PathType Container)) {
    throw "RunDir not found: $RunDir"
}

if ([string]::IsNullOrWhiteSpace($RunIdPrefix)) {
    $RunIdPrefix = Split-Path -Leaf $RunDir
}

if ([string]::IsNullOrWhiteSpace($RuntimeRunsRoot)) {
    $RuntimeRunsRoot = Join-Path $workspaceRoot "_local_artifacts\platform-runtime\runtime-host-runs"
}
$RuntimeRunsRoot = Resolve-PathFromRoot -PathValue $RuntimeRunsRoot -BasePath $workspaceRoot
if (-not (Test-Path -LiteralPath $RuntimeRunsRoot -PathType Container)) {
    throw "RuntimeRunsRoot not found: $RuntimeRunsRoot"
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $RunDir "schedule_trace"
}
$OutputDir = Resolve-PathFromRoot -PathValue $OutputDir -BasePath $workspaceRoot
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$mainlineSummaryPath = Join-Path $RunDir "mainline_summary.json"
$mainlineSummary = $null
$predictionRunByFrame = @{}
if (Test-Path -LiteralPath $mainlineSummaryPath -PathType Leaf) {
    $mainlineSummary = Read-JsonFile -PathValue $mainlineSummaryPath
    $prediction = Json-Prop -ObjectValue $mainlineSummary -Name "prediction"
    $predictionRuns = @(Json-Prop -ObjectValue $prediction -Name "runs")
    foreach ($predictionRun in $predictionRuns) {
        $frameIndex = [int](Number-Prop -ObjectValue $predictionRun -Name "trigger_frame_index" -DefaultValue -1)
        if ($frameIndex -ge 0 -and -not $predictionRunByFrame.ContainsKey($frameIndex)) {
            $predictionRunByFrame[$frameIndex] = $predictionRun
        }
    }
}

$schedulerFiles = @(Get-ChildItem -LiteralPath $RuntimeRunsRoot -Recurse -File -Filter "scheduler_timeline.json" |
    Where-Object { $_.FullName -like "*$RunIdPrefix*" } |
    Sort-Object FullName)
if ($schedulerFiles.Count -le 0) {
    throw "No scheduler_timeline.json matched RunIdPrefix '$RunIdPrefix' under $RuntimeRunsRoot"
}

$items = [System.Collections.Generic.List[object]]::new()
$sourceFiles = [System.Collections.Generic.List[string]]::new()
$globalSequence = 0

foreach ($file in $schedulerFiles) {
    $sourceFiles.Add($file.FullName) | Out-Null
    $timeline = Read-JsonFile -PathValue $file.FullName
    $workflowId = String-Prop -ObjectValue $timeline -Name "workflow_id"
    $workflowRunId = String-Prop -ObjectValue $timeline -Name "run_id" -DefaultValue $file.Directory.Name
    $objectId = String-Prop -ObjectValue $timeline -Name "object_id"
    $events = @($timeline.events)

    $starts = @{}
    foreach ($event in $events) {
        if ((String-Prop -ObjectValue $event -Name "event") -ne "start") {
            continue
        }
        $key = New-ItemKey -EventValue $event
        if (-not $starts.ContainsKey($key)) {
            $starts[$key] = $event
        }
    }

    foreach ($finish in $events) {
        if ((String-Prop -ObjectValue $finish -Name "event") -ne "finish") {
            continue
        }
        $key = New-ItemKey -EventValue $finish
        if (-not $starts.ContainsKey($key)) {
            continue
        }
        $start = $starts[$key]
        $nodeId = String-Prop -ObjectValue $finish -Name "node_id"
        $operatorId = String-Prop -ObjectValue (Json-Prop -ObjectValue $start -Name "ready_queue") -Name "operator_id"
        if ([string]::IsNullOrWhiteSpace($operatorId)) {
            $operatorId = String-Prop -ObjectValue (Json-Prop -ObjectValue $finish -Name "ready_queue") -Name "operator_id"
        }
        $inputBinding = Json-Prop -ObjectValue $start -Name "input_binding"
        $branchId = String-Prop -ObjectValue $inputBinding -Name "branch_id"
        if ([string]::IsNullOrWhiteSpace($branchId)) {
            $branchId = if ($workflowId -match "posterior_future_prediction") { "future.prediction" } else { "main.online" }
        }
        $timelineId = String-Prop -ObjectValue $inputBinding -Name "timeline_id"
        $stage = Get-StageName -NodeId $nodeId
        $dispatchTick = [int](Number-Prop -ObjectValue $start -Name "dispatch_tick_index" -DefaultValue $globalSequence)
        $schedulingLevel = [int](Number-Prop -ObjectValue $start -Name "scheduling_level" -DefaultValue -1)
        $loop = [int](Number-Prop -ObjectValue $finish -Name "loop_iteration_index" -DefaultValue -1)
        $durationMs = [double](Number-Prop -ObjectValue $finish -Name "duration_ms" -DefaultValue 1.0)
        if ($durationMs -lt 1.0) {
            $durationMs = 1.0
        }
        $fallbackStartMs = ([double](Number-Prop -ObjectValue $start -Name "runtime_event_time_s" -DefaultValue 0.0) * 1000.0)
        $modelTime = Resolve-ModelTime -FilePath $file.FullName -StartEvent $start -PredictionRunByFrame $predictionRunByFrame
        $steadyStartNs = [double](Number-Prop -ObjectValue $start -Name "execution_started_steady_ns" -DefaultValue 0.0)
        $steadyFinishNs = [double](Number-Prop -ObjectValue $finish -Name "execution_finished_steady_ns" -DefaultValue 0.0)
        if ($TimeAxis -eq "ExecutionTime") {
            if ($steadyStartNs -le 0.0 -or $steadyFinishNs -le $steadyStartNs) {
                throw "ExecutionTime axis requires execution_started_steady_ns/execution_finished_steady_ns. Re-run the runtime with the updated FlightEnvPlatformRuntimeHost, or use -TimeAxis ModelTime for existing evidence. Missing source: $($file.FullName)"
            }
            $startMs = $steadyStartNs / 1000000.0
            $durationMs = ($steadyFinishNs - $steadyStartNs) / 1000000.0
        } elseif ($TimeAxis -eq "WallClockApprox") {
            $startMs = (Parse-EventTimestampMs -EventValue $start -FallbackMs $fallbackStartMs) + ($dispatchTick * 3.0)
        } else {
            $startMs = ([double]$modelTime.model_time_s * 1000.0) + ($dispatchTick * 50.0)
        }
        $logicalTimeS = [double]$modelTime.local_time_s
        $resourceCheck = Json-Prop -ObjectValue $start -Name "resource_check"
        $parallelCheck = Json-Prop -ObjectValue $start -Name "parallel_check"
        $lockedResources = @()
        $lockedValue = Json-Prop -ObjectValue $resourceCheck -Name "locked_resource_ids"
        if ($null -ne $lockedValue) {
            $lockedResources = @($lockedValue)
        }
        $rowKey = "$branchId|$nodeId"
        $items.Add([pscustomobject]@{
            id = "sched.$globalSequence"
            sequence = $globalSequence
            branch_id = $branchId
            timeline_id = $timelineId
            workflow_id = $workflowId
            workflow_run_id = $workflowRunId
            object_id = $objectId
            node_id = $nodeId
            node_label = (Short-NodeName -NodeId $nodeId)
            operator_id = $operatorId
            stage = $stage
            status = (String-Prop -ObjectValue $finish -Name "status" -DefaultValue "ok")
            loop_iteration_index = $loop
            dispatch_tick_index = $dispatchTick
            scheduling_level = $schedulingLevel
            logical_time_s = $logicalTimeS
            model_time_s = $modelTime.model_time_s
            model_time_origin = $modelTime.origin
            source_period_s = $modelTime.period_s
            source_period_known = $modelTime.period_known
            source_period_source = $modelTime.period_source
            source_online_frame_index = $modelTime.online_frame_index
            source_predict_frame_index = $modelTime.predict_frame_index
            source_chunk_index = $modelTime.chunk_index
            execution_started_steady_ns = $steadyStartNs
            execution_finished_steady_ns = $steadyFinishNs
            start_ms = [math]::Round($startMs, 3)
            duration_ms = [math]::Round($durationMs, 3)
            end_ms = [math]::Round($startMs + $durationMs, 3)
            runtime_event_kind = (String-Prop -ObjectValue $finish -Name "runtime_event_kind")
            runtime_event_id = (String-Prop -ObjectValue $finish -Name "runtime_event_id")
            parallel_group_id = (String-Prop -ObjectValue $start -Name "parallel_group_id")
            capacity_group = (String-Prop -ObjectValue $parallelCheck -Name "capacity_group")
            locked_resource_ids = $lockedResources
            row_key = $rowKey
            source_file = $file.FullName
        }) | Out-Null
        $globalSequence++
    }
}

if ($items.Count -le 0) {
    throw "No start/finish schedule items were exported from matched scheduler timelines."
}

$minStart = ($items | Measure-Object -Property start_ms -Minimum).Minimum
$maxEnd = ($items | Measure-Object -Property end_ms -Maximum).Maximum
$axisStartMs = if ($TimeAxis -eq "ModelTime") { 0.0 } else { [double]$minStart }
$unknownPeriodItemCount = @($items | Where-Object { $_.source_period_known -ne $true }).Count
foreach ($item in $items) {
    $item | Add-Member -NotePropertyName "relative_start_ms" -NotePropertyValue ([math]::Round(([double]$item.start_ms - [double]$axisStartMs), 3))
    $item | Add-Member -NotePropertyName "relative_end_ms" -NotePropertyValue ([math]::Round(([double]$item.end_ms - [double]$axisStartMs), 3))
}

$rows = @($items |
    Sort-Object branch_id, workflow_run_id, node_id |
    Group-Object row_key |
    ForEach-Object {
        $first = $_.Group | Select-Object -First 1
        [pscustomobject]@{
            row_key = $_.Name
            branch_id = $first.branch_id
            node_id = $first.node_id
            label = "$($first.branch_id) / $($first.node_label)"
            item_count = @($_.Group).Count
        }
    })

$rowIndexByKey = @{}
for ($i = 0; $i -lt $rows.Count; $i++) {
    $rows[$i] | Add-Member -NotePropertyName "row_index" -NotePropertyValue $i
    $rowIndexByKey[$rows[$i].row_key] = $i
}
foreach ($item in $items) {
    $item | Add-Member -NotePropertyName "row_index" -NotePropertyValue $rowIndexByKey[$item.row_key]
}

$traceEvents = [System.Collections.Generic.List[object]]::new()
$pidByBranch = @{}
$nextPid = 1
foreach ($branch in @($items | Select-Object -ExpandProperty branch_id -Unique | Sort-Object)) {
    $pidByBranch[$branch] = $nextPid
    $traceEvents.Add([pscustomobject]@{
        name = "process_name"
        ph = "M"
        pid = $nextPid
        tid = 0
        args = @{ name = $branch }
    }) | Out-Null
    $nextPid++
}
foreach ($row in $rows) {
    $traceEvents.Add([pscustomobject]@{
        name = "thread_name"
        ph = "M"
        pid = $pidByBranch[$row.branch_id]
        tid = $row.row_index + 1
        args = @{ name = $row.node_id }
    }) | Out-Null
}
foreach ($item in $items) {
    $traceEvents.Add([pscustomobject]@{
        name = $item.node_label
        cat = $item.stage
        ph = "X"
        ts = [int64]([double]$item.relative_start_ms * 1000.0)
        dur = [int64]([double]$item.duration_ms * 1000.0)
        pid = $pidByBranch[$item.branch_id]
        tid = $item.row_index + 1
        args = @{
            branch_id = $item.branch_id
            workflow_id = $item.workflow_id
            workflow_run_id = $item.workflow_run_id
            node_id = $item.node_id
            operator_id = $item.operator_id
            loop_iteration_index = $item.loop_iteration_index
            dispatch_tick_index = $item.dispatch_tick_index
            scheduling_level = $item.scheduling_level
            status = $item.status
            runtime_event_kind = $item.runtime_event_kind
            logical_time_s = $item.logical_time_s
            source_file = $item.source_file
        }
    }) | Out-Null
}

$timeline = [ordered]@{
    schema_version = "flightenv.platform.schedule_timeline.v1"
    generated_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    run_dir = $RunDir
    run_id_prefix = $RunIdPrefix
    runtime_runs_root = $RuntimeRunsRoot
    source_scheduler_timeline_count = $schedulerFiles.Count
    source_scheduler_timeline_files = @($sourceFiles)
    time_axis = $TimeAxis
    time_basis = if ($TimeAxis -eq "ModelTime") {
        "platform model time: online frame index plus runtime_event_time_s; prediction trigger_time_s plus branch chunk index and runtime_event_time_s; dispatch_tick_index adds a small visual offset"
    } elseif ($TimeAxis -eq "ExecutionTime") {
        "measured execution time: execution_started_steady_ns/execution_finished_steady_ns from scheduler evidence; falls back only when old evidence lacks steady-clock fields"
    } else {
        "approximate wall clock: scheduler event timestamp_utc plus dispatch_tick_index offset; duration_ms from finish events"
    }
    warning = if ($TimeAxis -eq "ModelTime") {
        "X axis is model/platform time, not measured wall-clock execution time. Bar widths still use measured operator duration_ms."
    } elseif ($TimeAxis -eq "ExecutionTime") {
        "ExecutionTime is exact only for runs produced after scheduler evidence records steady-clock start/finish fields."
    } else {
        "Current runtime evidence has duration_ms and second-level event timestamps. Add monotonic begin/end timestamps in the runtime hot path for exact wall-clock traces."
    }
    summary = [ordered]@{
        item_count = $items.Count
        row_count = $rows.Count
        branch_count = @($items | Select-Object -ExpandProperty branch_id -Unique).Count
        min_start_ms = [math]::Round([double]$minStart, 3)
        max_end_ms = [math]::Round([double]$maxEnd, 3)
        axis_start_ms = [math]::Round([double]$axisStartMs, 3)
        span_ms = [math]::Round(([double]$maxEnd - [double]$axisStartMs), 3)
        unknown_period_item_count = $unknownPeriodItemCount
    }
    rows = @($rows)
    items = @($items | Sort-Object start_ms, sequence)
}

$timelinePath = Join-Path $OutputDir "schedule_timeline.json"
$chromeTracePath = Join-Path $OutputDir "chrome_trace.json"
$htmlPath = Join-Path $OutputDir "schedule_timeline.html"
$svgPath = Join-Path $OutputDir "schedule_timeline.svg"
$pngPath = Join-Path $OutputDir "schedule_timeline.png"

$timeline | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 -LiteralPath $timelinePath
@{ traceEvents = @($traceEvents); displayTimeUnit = "ms" } |
    ConvertTo-Json -Depth 12 |
    Set-Content -Encoding UTF8 -LiteralPath $chromeTracePath

$left = 360
$top = 86
$rowHeight = 28
$rightPad = 80
$bottomPad = 80
$spanMs = [math]::Max(1.0, ([double]$maxEnd - [double]$axisStartMs))
$scale = if ($spanMs -gt 0) { [math]::Min(0.18, [math]::Max(0.04, 1400.0 / $spanMs)) } else { 0.1 }
$chartWidth = [int]([math]::Max(1200, $left + $rightPad + ($spanMs * $scale)))
$chartHeight = [int]($top + $bottomPad + ($rows.Count * $rowHeight))
$tickStepMs = if ($spanMs -le 5000) { 500 } elseif ($spanMs -le 20000) { 1000 } else { 5000 }

$svg = [System.Text.StringBuilder]::new()
[void]$svg.AppendLine("<svg width=""$chartWidth"" height=""$chartHeight"" viewBox=""0 0 $chartWidth $chartHeight"" xmlns=""http://www.w3.org/2000/svg"">")
[void]$svg.AppendLine("<style>.axis{font:12px Segoe UI,Arial;fill:#475569}.row{font:11px Consolas,Segoe UI,Arial;fill:#334155}.barText{font:10px Segoe UI,Arial;fill:white;pointer-events:none}.small{font:10px Segoe UI,Arial;fill:#64748b}.grid{stroke:#e2e8f0;stroke-width:1}.rowLine{stroke:#f1f5f9;stroke-width:1}</style>")
[void]$svg.AppendLine("<rect x=""0"" y=""0"" width=""$chartWidth"" height=""$chartHeight"" fill=""#ffffff""/>")
[void]$svg.AppendLine("<text x=""20"" y=""30"" style=""font:700 18px Segoe UI,Arial;fill:#0f172a"">FlightEnv schedule timeline</text>")
[void]$svg.AppendLine("<text x=""20"" y=""52"" class=""axis"">run: $(Html-Escape $RunIdPrefix) - axis: $(Html-Escape $TimeAxis) - items: $($items.Count) - rows: $($rows.Count) - span: $([math]::Round($spanMs / 1000.0, 2)) s</text>")

for ($t = 0; $t -le $spanMs + $tickStepMs; $t += $tickStepMs) {
    $x = [math]::Round($left + ($t * $scale), 2)
    [void]$svg.AppendLine("<line x1=""$x"" y1=""$top"" x2=""$x"" y2=""$($chartHeight - $bottomPad + 12)"" class=""grid""/>")
    [void]$svg.AppendLine("<text x=""$($x + 3)"" y=""$($top - 10)"" class=""axis"">$([math]::Round($t / 1000.0, 1))s</text>")
}

foreach ($row in $rows) {
    $y = $top + ([int]$row.row_index * $rowHeight)
    [void]$svg.AppendLine("<line x1=""0"" y1=""$($y + $rowHeight)"" x2=""$chartWidth"" y2=""$($y + $rowHeight)"" class=""rowLine""/>")
    [void]$svg.AppendLine("<text x=""20"" y=""$($y + 18)"" class=""row"">$(Html-Escape $row.label)</text>")
}

foreach ($item in @($items | Sort-Object relative_start_ms, sequence)) {
    $x = [math]::Round($left + ([double]$item.relative_start_ms * $scale), 2)
    $w = [math]::Max(3.0, [math]::Round([double]$item.duration_ms * $scale, 2))
    $y = $top + ([int]$item.row_index * $rowHeight) + 5
    $h = 18
    $color = Get-StageColor -Stage $item.stage
    $title = "$(Html-Escape $item.node_id) | $(Html-Escape $item.operator_id) | branch=$(Html-Escape $item.branch_id) | loop=$($item.loop_iteration_index) | duration=$($item.duration_ms)ms"
    [void]$svg.AppendLine("<rect x=""$x"" y=""$y"" width=""$w"" height=""$h"" rx=""3"" fill=""$color"" opacity=""0.88""><title>$title</title></rect>")
    if ($w -gt 74) {
        [void]$svg.AppendLine("<text x=""$($x + 5)"" y=""$($y + 13)"" class=""barText"">$(Html-Escape $item.node_label)</text>")
    }
}

[void]$svg.AppendLine("</svg>")
$svg.ToString() | Set-Content -Encoding UTF8 -LiteralPath $svgPath

$legendStages = @("State transition", "Observation", "Filter", "Posterior fields", "QoI", "Future prediction", "Other")
$legendHtml = ($legendStages | ForEach-Object {
    $c = Get-StageColor -Stage $_
    "<span class=""legend-item""><span class=""swatch"" style=""background:$c""></span>$(Html-Escape $_)</span>"
}) -join "`n"

$topItemsHtml = ($items |
    Sort-Object @{ Expression = "duration_ms"; Descending = $true } |
    Select-Object -First 12 |
    ForEach-Object {
        "<tr><td>$(Html-Escape $_.branch_id)</td><td>$(Html-Escape $_.node_id)</td><td>$(Html-Escape $_.operator_id)</td><td class=""num"">$($_.duration_ms)</td><td class=""num"">$($_.loop_iteration_index)</td></tr>"
    }) -join "`n"

$html = @"
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <title>FlightEnv schedule timeline - $([System.Net.WebUtility]::HtmlEncode($RunIdPrefix))</title>
  <style>
    body { margin: 0; font-family: "Segoe UI", "Microsoft YaHei", Arial, sans-serif; background: #f8fafc; color: #0f172a; }
    header { padding: 18px 24px 10px; border-bottom: 1px solid #dbe3ea; background: #fff; position: sticky; top: 0; z-index: 2; }
    h1 { margin: 0 0 8px; font-size: 20px; }
    .meta { color: #475569; font-size: 13px; line-height: 1.7; }
    .legend { margin-top: 10px; display: flex; flex-wrap: wrap; gap: 12px; font-size: 13px; }
    .legend-item { display: inline-flex; align-items: center; gap: 6px; }
    .swatch { width: 12px; height: 12px; border-radius: 3px; display: inline-block; }
    .wrap { padding: 18px 24px 36px; }
    .panel { background: #fff; border: 1px solid #dbe3ea; border-radius: 8px; overflow: auto; margin-bottom: 18px; }
    .svgbox { min-width: 100%; }
    table { width: 100%; border-collapse: collapse; font-size: 13px; background: #fff; }
    th, td { padding: 7px 9px; border-bottom: 1px solid #e2e8f0; text-align: left; vertical-align: top; }
    th { background: #f1f5f9; color: #334155; position: sticky; top: 0; }
    .num { text-align: right; font-variant-numeric: tabular-nums; }
    code { background: #f1f5f9; padding: 1px 4px; border-radius: 4px; }
  </style>
</head>
<body>
  <header>
    <h1>FlightEnv schedule timeline</h1>
    <div class="meta">
      run: <code>$(Html-Escape $RunIdPrefix)</code><br>
      axis: <code>$(Html-Escape $TimeAxis)</code><br>
      output: <code>$(Html-Escape $OutputDir)</code><br>
      source scheduler_timeline files: $($schedulerFiles.Count); operator items: $($items.Count); branches: $(@($items | Select-Object -ExpandProperty branch_id -Unique).Count)
    </div>
    <div class="legend">
      $legendHtml
    </div>
  </header>
  <div class="wrap">
    <div class="panel svgbox">
      $($svg.ToString())
    </div>
    <div class="panel">
      <table>
        <thead><tr><th>Branch</th><th>Node</th><th>Operator</th><th class="num">Duration(ms)</th><th class="num">Loop</th></tr></thead>
        <tbody>
          $topItemsHtml
        </tbody>
      </table>
    </div>
    <p class="meta">
      This directory also contains <code>schedule_timeline.json</code> and <code>chrome_trace.json</code>.
      Import <code>chrome_trace.json</code> into Chrome Trace or Perfetto-style tools for deeper debugging.
    </p>
  </div>
</body>
</html>
"@
$html | Set-Content -Encoding UTF8 -LiteralPath $htmlPath

try {
    Add-Type -AssemblyName System.Drawing
    $bitmap = New-Object System.Drawing.Bitmap($chartWidth, $chartHeight)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit

    $white = [System.Drawing.Brushes]::White
    $textBrush = New-Object System.Drawing.SolidBrush([System.Drawing.ColorTranslator]::FromHtml("#0f172a"))
    $mutedBrush = New-Object System.Drawing.SolidBrush([System.Drawing.ColorTranslator]::FromHtml("#475569"))
    $rowBrush = New-Object System.Drawing.SolidBrush([System.Drawing.ColorTranslator]::FromHtml("#334155"))
    $gridPen = New-Object System.Drawing.Pen([System.Drawing.ColorTranslator]::FromHtml("#e2e8f0"), 1)
    $rowPen = New-Object System.Drawing.Pen([System.Drawing.ColorTranslator]::FromHtml("#f1f5f9"), 1)
    $fontTitle = New-Object System.Drawing.Font("Segoe UI", 14, [System.Drawing.FontStyle]::Bold)
    $fontAxis = New-Object System.Drawing.Font("Segoe UI", 9)
    $fontRow = New-Object System.Drawing.Font("Consolas", 8)
    $fontBar = New-Object System.Drawing.Font("Segoe UI", 7)
    $whiteText = [System.Drawing.Brushes]::White

    $graphics.FillRectangle($white, 0, 0, $chartWidth, $chartHeight)
    $graphics.DrawString("FlightEnv schedule timeline", $fontTitle, $textBrush, 20, 14)
    $graphics.DrawString("run: $RunIdPrefix - items: $($items.Count) - rows: $($rows.Count) - span: $([math]::Round($spanMs / 1000.0, 2)) s", $fontAxis, $mutedBrush, 20, 44)

    for ($t = 0; $t -le $spanMs + $tickStepMs; $t += $tickStepMs) {
        $x = [single]($left + ($t * $scale))
        $graphics.DrawLine($gridPen, $x, [single]$top, $x, [single]($chartHeight - $bottomPad + 12))
        $graphics.DrawString("$([math]::Round($t / 1000.0, 1))s", $fontAxis, $mutedBrush, $x + 3, $top - 26)
    }

    foreach ($row in $rows) {
        $y = [single]($top + ([int]$row.row_index * $rowHeight))
        $graphics.DrawLine($rowPen, 0, $y + $rowHeight, $chartWidth, $y + $rowHeight)
        $graphics.DrawString([string]$row.label, $fontRow, $rowBrush, 20, $y + 8)
    }

    foreach ($item in @($items | Sort-Object relative_start_ms, sequence)) {
        $x = [single]($left + ([double]$item.relative_start_ms * $scale))
        $w = [single]([math]::Max(3.0, ([double]$item.duration_ms * $scale)))
        $y = [single]($top + ([int]$item.row_index * $rowHeight) + 5)
        $h = [single]18
        $barBrush = New-Object System.Drawing.SolidBrush([System.Drawing.ColorTranslator]::FromHtml((Get-StageColor -Stage $item.stage)))
        $graphics.FillRectangle($barBrush, $x, $y, $w, $h)
        if ($w -gt 74) {
            $graphics.DrawString([string]$item.node_label, $fontBar, $whiteText, $x + 4, $y + 3)
        }
        $barBrush.Dispose()
    }

    $bitmap.Save($pngPath, [System.Drawing.Imaging.ImageFormat]::Png)

    $fontTitle.Dispose()
    $fontAxis.Dispose()
    $fontRow.Dispose()
    $fontBar.Dispose()
    $gridPen.Dispose()
    $rowPen.Dispose()
    $textBrush.Dispose()
    $mutedBrush.Dispose()
    $rowBrush.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
} catch {
    Write-Warning "PNG export skipped: $($_.Exception.Message)"
}

Write-Host "[OK] FlightEnv schedule trace exported."
Write-Host "  run_dir       = $RunDir"
Write-Host "  scheduler     = $($schedulerFiles.Count)"
Write-Host "  items         = $($items.Count)"
Write-Host "  rows          = $($rows.Count)"
Write-Host "  timeline_json = $timelinePath"
Write-Host "  chrome_trace  = $chromeTracePath"
Write-Host "  svg           = $svgPath"
if (Test-Path -LiteralPath $pngPath -PathType Leaf) {
    Write-Host "  png           = $pngPath"
}
Write-Host "  html          = $htmlPath"

if ($Open) {
    Start-Process -FilePath $htmlPath | Out-Null
}
