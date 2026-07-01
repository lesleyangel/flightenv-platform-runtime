param(
    [string]$Python = "auto",

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',

    [string]$RunIdPrefix = "gate_checkpoint_replay_determinism",

    [int]$OnlineFrames = 3,

    [int]$PredictionEveryFrames = 1,

    [int]$FutureMaxIterations = 1,

    [int]$BranchChunkIterations = 1,

    [string]$ExternalObservationStream = "",

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
$pdkRoot = Join-Path $workspaceRoot 'flightenv-platform-pdk'
Import-Module (Join-Path $pdkRoot 'tools\PdkPython.psm1') -Force
$Python = Resolve-PdkPython -Python $Python -PdkRoot $pdkRoot
$reportRoot = Join-Path $workspaceRoot '_local_artifacts\platform-runtime\scheduler-acceptance'
$reportPath = Join-Path $reportRoot 'checkpoint_replay_determinism_audit.json'
$smokeScript = Join-Path $runtimeRoot 'tools\run_cpp_runtime_host_smoke.ps1'
$runtimeRunRoot = Join-Path $workspaceRoot '_local_artifacts\platform-runtime\runtime-host-runs'
$workspaceRootSlash = $workspaceRoot.Replace('\', '/')

if ([string]::IsNullOrWhiteSpace($ExternalObservationStream)) {
    $ExternalObservationStream = Join-Path $workspaceRoot 'flightenv-object-reentry-vehicle\fixtures\sensor_stream_db70.json'
}
if (-not (Test-Path -LiteralPath $ExternalObservationStream -PathType Leaf)) {
    throw "External observation stream not found: $ExternalObservationStream"
}
if (-not (Test-Path -LiteralPath $smokeScript -PathType Leaf)) {
    throw "Runtime host smoke script not found: $smokeScript"
}

function Read-JsonFile {
    param([Parameter(Mandatory=$true)][string]$PathValue)
    if (-not (Test-Path -LiteralPath $PathValue -PathType Leaf)) {
        throw "JSON file not found: $PathValue"
    }
    return Get-Content -LiteralPath $PathValue -Encoding UTF8 -Raw | ConvertFrom-Json
}

function Get-Prop {
    param(
        [object]$ObjectValue,
        [Parameter(Mandatory=$true)][string]$Name,
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

function As-Array {
    param([object]$Value)
    if ($null -eq $Value) {
        return @()
    }
    return @($Value)
}

function Normalize-Text {
    param(
        [object]$Value,
        [string]$RunA,
        [string]$RunB
    )
    if ($null -eq $Value) {
        return ""
    }
    $text = [string]$Value
    $text = $text.Replace('\', '/')
    $text = $text.Replace($workspaceRootSlash, '<WORKSPACE>')
    if (-not [string]::IsNullOrWhiteSpace($RunA)) {
        $text = $text.Replace($RunA, '<RUN>')
    }
    if (-not [string]::IsNullOrWhiteSpace($RunB)) {
        $text = $text.Replace($RunB, '<RUN>')
    }
    $text = [regex]::Replace($text, 'rtb_[0-9a-fA-F]+', 'rtb_<BUFFER>')
    return $text
}

function Round-Number {
    param([object]$Value)
    if ($null -eq $Value) {
        return $null
    }
    try {
        return [Math]::Round([double]$Value, 12)
    } catch {
        return $Value
    }
}

function Normalize-BranchId {
    param(
        [object]$BranchId,
        [hashtable]$BranchMap
    )
    $branch = [string]$BranchId
    if ([string]::IsNullOrWhiteSpace($branch)) {
        return ""
    }
    if ($BranchMap.ContainsKey($branch)) {
        return $BranchMap[$branch]
    }
    return $branch
}

function Normalize-Shape {
    param([object]$Shape)
    if ($null -eq $Shape) {
        return @()
    }
    return @(As-Array $Shape | ForEach-Object { [int]$_ })
}

function Get-StableHash {
    param([object]$Value)
    $json = if ($null -eq $Value) { 'null' } else { $Value | ConvertTo-Json -Depth 96 -Compress }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Normalize-Any {
    param(
        [object]$Value,
        [string]$RunA,
        [string]$RunB
    )
    if ($null -eq $Value) {
        return $null
    }
    if ($Value -is [string]) {
        return Normalize-Text -Value $Value -RunA $RunA -RunB $RunB
    }
    if ($Value -is [bool]) {
        return [bool]$Value
    }
    if ($Value -is [byte] -or $Value -is [int16] -or $Value -is [int32] -or $Value -is [int64]) {
        return [int64]$Value
    }
    if ($Value -is [float] -or $Value -is [double] -or $Value -is [decimal]) {
        return Round-Number $Value
    }
    if ($Value -is [System.Collections.IEnumerable] -and -not ($Value -is [string]) -and -not ($Value -is [pscustomobject])) {
        return @(As-Array $Value | ForEach-Object { Normalize-Any -Value $_ -RunA $RunA -RunB $RunB })
    }

    $volatileNames = @(
        'generated_at_utc',
        'created_at_utc',
        'updated_at_utc',
        'duration_ms',
        'wall_time_ms',
        'inline_byte_size',
        'request_digest',
        'library',
        'source_root',
        'source_run_dir',
        'source_runtime_outputs',
        'branch_chunk_run_dir',
        'run_root',
        'buffer_id',
        'path',
        'uri',
        'ref',
        'checksum',
        'input_hashes',
        'output_hashes',
        'typed_buffer_ref',
        'runtime_zero_copy_policy',
        'model_evidence',
        'session_snapshot',
        'adapter_snapshot',
        'request_digest',
        'shadow_write_count',
        'shadow_write_pending',
        'shadow_write_status',
        'shadow_error',
        'logical_ref_count',
        'lifecycle_state',
        'released'
    )
    $result = [ordered]@{}
    foreach ($prop in @($Value.PSObject.Properties | Sort-Object Name)) {
        if ($volatileNames -contains $prop.Name) {
            continue
        }
        if ($prop.Name -eq 'run_id') {
            $result[$prop.Name] = '<RUN>'
            continue
        }
        $result[$prop.Name] = Normalize-Any -Value $prop.Value -RunA $RunA -RunB $RunB
    }
    return [pscustomobject]$result
}

function Get-TimePointProjection {
    param([object]$Value)
    if ($null -eq $Value) {
        return $null
    }
    return [ordered]@{
        run_time_s = Round-Number (Get-Prop $Value 'run_time_s')
        source_time_s = Round-Number (Get-Prop $Value 'source_time_s')
        stamp_ns = Get-Prop $Value 'stamp_ns'
        tick_index = Get-Prop $Value 'tick_index'
    }
}

function New-BranchMap {
    param([object]$Timeline)
    $branches = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($collectionName in @('online_frames', 'branch_steps', 'artifact_refs', 'qoi_refs', 'checkpoint_refs', 'prediction_runs')) {
        foreach ($item in As-Array (Get-Prop $Timeline $collectionName)) {
            $branch = [string](Get-Prop $item 'branch_id')
            if (-not [string]::IsNullOrWhiteSpace($branch)) {
                $branches.Add($branch) | Out-Null
            }
        }
    }
    $map = @{}
    $predictBranches = @($branches | Where-Object { $_ -like 'predict.*' } | Sort-Object)
    for ($i = 0; $i -lt $predictBranches.Count; $i += 1) {
        $map[$predictBranches[$i]] = ('predict.{0:000}' -f $i)
    }
    return $map
}

function Project-OutputRef {
    param(
        [object]$Item,
        [hashtable]$BranchMap
    )
    return [ordered]@{
        branch_id = Normalize-BranchId -BranchId (Get-Prop $Item 'branch_id') -BranchMap $BranchMap
        node_id = [string](Get-Prop $Item 'node_id')
        operator_id = [string](Get-Prop $Item 'operator_id')
        port_id = [string](Get-Prop $Item 'port_id')
        contract_id = [string](Get-Prop $Item 'contract_id')
        direction = [string](Get-Prop $Item 'direction')
        field_name = [string](Get-Prop $Item 'field_name')
        component_id = [string](Get-Prop $Item 'component_id')
        layout_ref = [string](Get-Prop $Item 'layout_ref')
        mesh_ref = [string](Get-Prop $Item 'mesh_ref')
        representation = [string](Get-Prop $Item 'representation')
        node_count = Get-Prop $Item 'node_count'
        shape = Normalize-Shape (Get-Prop $Item 'shape')
        frame_index = Get-Prop $Item 'frame_index'
        mainline_frame_index = Get-Prop $Item 'mainline_frame_index'
        loop_iteration_index = Get-Prop $Item 'loop_iteration_index'
        source_loop_iteration_index = Get-Prop $Item 'source_loop_iteration_index'
        source_step_index = Get-Prop $Item 'source_step_index'
        step_index = Get-Prop $Item 'step_index'
        branch_chunk_index = Get-Prop $Item 'branch_chunk_index'
        time_point = Get-TimePointProjection (Get-Prop $Item 'time_point')
    }
}

function Build-TimelineProjection {
    param(
        [Parameter(Mandatory=$true)][string]$ChainDir,
        [string]$RunA,
        [string]$RunB
    )
    $timelinePath = Join-Path $ChainDir 'run_timeline_index.json'
    $timeline = Read-JsonFile $timelinePath
    $branchMap = New-BranchMap -Timeline $timeline

    $onlineFrames = @(As-Array (Get-Prop $timeline 'online_frames') | ForEach-Object {
        $selectedState = Get-Prop $_ 'selected_state'
        $values = Get-Prop (Get-Prop $_ 'frame') 'values'
        [ordered]@{
            branch_id = Normalize-BranchId -BranchId (Get-Prop $_ 'branch_id') -BranchMap $branchMap
            frame_index = Get-Prop $_ 'frame_index'
            mainline_frame_index = Get-Prop $_ 'mainline_frame_index'
            loop_iteration_index = Get-Prop $_ 'loop_iteration_index'
            sample_time_s = Round-Number (Get-Prop $_ 'sample_time_s')
            public_time_s = Round-Number (Get-Prop $_ 'public_time_s')
            source = [string](Get-Prop $_ 'source')
            status = [string](Get-Prop $_ 'status')
            stage_id = [string](Get-Prop $_ 'stage_id')
            step_index = Get-Prop $_ 'step_index'
            step_role = [string](Get-Prop $_ 'step_role')
            sensor_count = Get-Prop $_ 'sensor_count'
            selected_state_hash = if ($null -ne $selectedState) { Get-StableHash (Normalize-Any -Value $selectedState -RunA $RunA -RunB $RunB) } else { "" }
            values_hash = if ($null -ne $values) { Get-StableHash (Normalize-Any -Value $values -RunA $RunA -RunB $RunB) } else { "" }
        }
    } | Sort-Object { $_['frame_index'] }, { $_['step_index'] })

    $branchSteps = @(As-Array (Get-Prop $timeline 'branch_steps') | ForEach-Object {
        [ordered]@{
            branch_id = Normalize-BranchId -BranchId (Get-Prop $_ 'branch_id') -BranchMap $branchMap
            branch_kind = [string](Get-Prop $_ 'branch_kind')
            status = [string](Get-Prop $_ 'status')
            stage_id = [string](Get-Prop $_ 'stage_id')
            step_role = [string](Get-Prop $_ 'step_role')
            step_index = Get-Prop $_ 'step_index'
            source_step_index = Get-Prop $_ 'source_step_index'
            mainline_frame_index = Get-Prop $_ 'mainline_frame_index'
            frame_index = Get-Prop $_ 'frame_index'
            sample_time_s = Round-Number (Get-Prop $_ 'sample_time_s')
            public_time_s = Round-Number (Get-Prop $_ 'public_time_s')
            trigger_time_s = Round-Number (Get-Prop $_ 'trigger_time_s')
            branch_relative_time_s = Round-Number (Get-Prop $_ 'branch_relative_time_s')
        }
    } | Sort-Object { $_['branch_id'] }, { $_['mainline_frame_index'] }, { $_['step_index'] }, { $_['public_time_s'] })

    $artifactRefs = @(As-Array (Get-Prop $timeline 'artifact_refs') | ForEach-Object {
        Project-OutputRef -Item $_ -BranchMap $branchMap
    } | Sort-Object { $_['branch_id'] }, { $_['mainline_frame_index'] }, { $_['step_index'] }, { $_['node_id'] }, { $_['port_id'] }, { $_['contract_id'] })

    $qoiRefs = @(As-Array (Get-Prop $timeline 'qoi_refs') | ForEach-Object {
        Project-OutputRef -Item $_ -BranchMap $branchMap
    } | Sort-Object { $_['branch_id'] }, { $_['mainline_frame_index'] }, { $_['step_index'] }, { $_['node_id'] }, { $_['port_id'] }, { $_['contract_id'] })

    $checkpointRefs = @(As-Array (Get-Prop $timeline 'checkpoint_refs') | ForEach-Object {
        [ordered]@{
            branch_id = Normalize-BranchId -BranchId (Get-Prop $_ 'branch_id') -BranchMap $branchMap
            checkpoint_id = [string](Get-Prop $_ 'checkpoint_id')
            checkpoint_kind = [string](Get-Prop $_ 'checkpoint_kind')
            replay_mode = [string](Get-Prop $_ 'replay_mode')
            node_id = [string](Get-Prop $_ 'node_id')
            operator_id = [string](Get-Prop $_ 'operator_id')
            adapter_protocol = [string](Get-Prop $_ 'adapter_protocol')
            frame_index = Get-Prop $_ 'frame_index'
            mainline_frame_index = Get-Prop $_ 'mainline_frame_index'
            loop_iteration_index = Get-Prop $_ 'loop_iteration_index'
            source_loop_iteration_index = Get-Prop $_ 'source_loop_iteration_index'
            source_step_index = Get-Prop $_ 'source_step_index'
            step_index = Get-Prop $_ 'step_index'
            input_hash_count = @(As-Array (Get-Prop $_ 'input_hashes')).Count
            output_hash_count = @(As-Array (Get-Prop $_ 'output_hashes')).Count
            time_point = Get-TimePointProjection (Get-Prop $_ 'time_point')
        }
    } | Sort-Object { $_['branch_id'] }, { $_['mainline_frame_index'] }, { $_['step_index'] }, { $_['node_id'] }, { $_['checkpoint_id'] })

    $predictionRuns = @(As-Array (Get-Prop $timeline 'prediction_runs') | ForEach-Object {
        [ordered]@{
            branch_id = Normalize-BranchId -BranchId (Get-Prop $_ 'branch_id') -BranchMap $branchMap
            status = [string](Get-Prop $_ 'status')
            trigger_frame_index = Get-Prop $_ 'trigger_frame_index'
            trigger_time_s = Round-Number (Get-Prop $_ 'trigger_time_s')
            step_count = Get-Prop $_ 'step_count'
            qoi_count = Get-Prop $_ 'qoi_count'
            artifact_count = Get-Prop $_ 'artifact_count'
        }
    } | Sort-Object { $_['branch_id'] }, { $_['trigger_frame_index'] })

    return [ordered]@{
        schema_version = [string](Get-Prop $timeline 'schema_version')
        workflow_id = [string](Get-Prop $timeline 'workflow_id')
        online_frames = $onlineFrames
        branch_steps = $branchSteps
        prediction_runs = $predictionRuns
        artifact_refs = $artifactRefs
        qoi_refs = $qoiRefs
        checkpoint_refs = $checkpointRefs
    }
}

function Build-RuntimeProjection {
    param(
        [Parameter(Mandatory=$true)][string]$RunPrefix,
        [string]$RunA,
        [string]$RunB
    )
    if (-not (Test-Path -LiteralPath $runtimeRunRoot -PathType Container)) {
        throw "Runtime run root not found: $runtimeRunRoot"
    }

    $packetFiles = @(Get-ChildItem -LiteralPath $runtimeRunRoot -Recurse -Filter runtime_packets.json |
        Where-Object { $_.FullName -like "*$RunPrefix*" } |
        Sort-Object FullName)
    $checkpointFiles = @(Get-ChildItem -LiteralPath $runtimeRunRoot -Recurse -Filter state_checkpoint.json |
        Where-Object { $_.FullName -like "*$RunPrefix*" } |
        Sort-Object FullName)
    $outputFiles = @(Get-ChildItem -LiteralPath $runtimeRunRoot -Recurse -Filter runtime_outputs.json |
        Where-Object { $_.FullName -like "*$RunPrefix*" } |
        Sort-Object FullName)

    $packets = @()
    foreach ($file in $packetFiles) {
        $doc = Read-JsonFile $file.FullName
        foreach ($packet in As-Array (Get-Prop $doc 'packets')) {
            $tags = Normalize-Any -Value (Get-Prop $packet 'tags') -RunA $RunA -RunB $RunB
            $payload = Normalize-Any -Value (Get-Prop $packet 'payload') -RunA $RunA -RunB $RunB
            $packets += [ordered]@{
                workflow_id = [string](Get-Prop $doc 'workflow_id')
                node_id = [string](Get-Prop $packet 'node_id')
                producer_node = [string](Get-Prop $packet 'producer_node')
                port_name = [string](Get-Prop $packet 'port_name')
                payload_kind = [string](Get-Prop $packet 'payload_kind')
                payload_ref = Normalize-Text -Value (Get-Prop $packet 'payload_ref') -RunA $RunA -RunB $RunB
                source_time_s = Round-Number (Get-Prop $packet 'source_time_s')
                time_s = Round-Number (Get-Prop $packet 'time_s')
                stamp_ns = Get-Prop $packet 'stamp_ns'
                tick_index = Get-Prop $packet 'tick_index'
            }
        }
    }

    $checkpoints = @()
    foreach ($file in $checkpointFiles) {
        $doc = Read-JsonFile $file.FullName
        foreach ($checkpoint in As-Array (Get-Prop $doc 'checkpoints')) {
            $checkpoints += (Normalize-Any -Value $checkpoint -RunA $RunA -RunB $RunB)
        }
    }

    $outputs = @()
    foreach ($file in $outputFiles) {
        $doc = Read-JsonFile $file.FullName
        $outputs += [ordered]@{
            workflow_id = [string](Get-Prop $doc 'workflow_id')
            outputs_hash = Get-StableHash (Normalize-Any -Value (Get-Prop $doc 'outputs') -RunA $RunA -RunB $RunB)
            typed_buffer_store_hash = Get-StableHash (Normalize-Any -Value (Get-Prop $doc 'typed_buffer_store') -RunA $RunA -RunB $RunB)
        }
    }

    $typedBufferContent = @()
    foreach ($file in $outputFiles) {
        $tbDir = Join-Path $file.DirectoryName 'tb'
        if (-not (Test-Path -LiteralPath $tbDir -PathType Container)) {
            continue
        }
        foreach ($bin in @(Get-ChildItem -LiteralPath $tbDir -File -Filter *.bin | Sort-Object Length, Name)) {
            $typedBufferContent += [ordered]@{
                length = $bin.Length
                sha256 = (Get-FileHash -LiteralPath $bin.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        }
    }

    return [ordered]@{
        packet_file_count = $packetFiles.Count
        checkpoint_file_count = $checkpointFiles.Count
        output_file_count = $outputFiles.Count
        runtime_packets = @($packets | Sort-Object { $_['workflow_id'] }, { $_['node_id'] }, { $_['port_name'] }, { $_['source_time_s'] }, { $_['tick_index'] }, { $_['payload_kind'] })
        state_checkpoints = @($checkpoints | Sort-Object { Get-Prop $_ 'node_id' }, { Get-Prop $_ 'checkpoint_id' }, { Get-Prop $_ 'step_index' }, { Get-Prop $_ 'loop_iteration_index' })
        runtime_outputs = @($outputs | Sort-Object { $_['workflow_id'] }, { $_['outputs_hash'] }, { $_['typed_buffer_store_hash'] })
        typed_buffer_content = @($typedBufferContent | Sort-Object { $_['length'] }, { $_['sha256'] })
    }
}

function Invoke-Smoke {
    param(
        [Parameter(Mandatory=$true)][string]$Prefix,
        [switch]$Build
    )
    $argsList = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', $smokeScript,
        '-Python', $Python,
        '-Configuration', $Configuration,
        '-Platform', $Platform,
        '-RunIdPrefix', $Prefix,
        '-OnlineFrames', $OnlineFrames,
        '-PredictionEveryFrames', $PredictionEveryFrames,
        '-FutureMaxIterations', $FutureMaxIterations,
        '-BranchChunkIterations', $BranchChunkIterations,
        '-ExternalObservationStream', $ExternalObservationStream
    )
    if (-not $Build) {
        $argsList += '-SkipBuild'
    }
    & powershell @argsList | ForEach-Object { Write-Host $_ }
    $exitCode = if (Test-Path -LiteralPath 'variable:LASTEXITCODE') { $LASTEXITCODE } else { 0 }
    if ($exitCode -ne 0) {
        throw "Runtime host smoke failed for prefix '$Prefix' with exit code $exitCode"
    }
    $chainDir = Join-Path $workspaceRoot "_local_artifacts\platform-runtime\mainline-runs\$Prefix"
    if (-not (Test-Path -LiteralPath (Join-Path $chainDir 'run_timeline_index.json') -PathType Leaf)) {
        throw "Timeline not generated for prefix '$Prefix': $chainDir"
    }
    return $chainDir
}

function Build-RunProjection {
    param(
        [Parameter(Mandatory=$true)][string]$Prefix,
        [Parameter(Mandatory=$true)][string]$ChainDir,
        [string]$RunA,
        [string]$RunB
    )
    $timeline = Build-TimelineProjection -ChainDir $ChainDir -RunA $RunA -RunB $RunB
    $runtime = Build-RuntimeProjection -RunPrefix $Prefix -RunA $RunA -RunB $RunB
    $branchIndex = [ordered]@{
        branch_steps = $timeline.branch_steps
        prediction_runs = $timeline.prediction_runs
    }
    $keyOutputs = [ordered]@{
        artifact_refs = $timeline.artifact_refs
        qoi_refs = $timeline.qoi_refs
    }
    $runtimeOutputsProjection = $runtime.runtime_outputs
    return [ordered]@{
        chain_dir = $ChainDir
        projections = [ordered]@{
            public_timeline = $timeline
            branch_index = $branchIndex
            runtime_packets = $runtime.runtime_packets
            checkpoints = $runtime.state_checkpoints
            key_output_refs = $keyOutputs
            runtime_outputs = $runtimeOutputsProjection
        }
        hashes = [ordered]@{
            public_timeline = Get-StableHash $timeline
            branch_index = Get-StableHash $branchIndex
            runtime_packets = Get-StableHash $runtime.runtime_packets
            checkpoints = Get-StableHash $runtime.state_checkpoints
            key_output_refs = Get-StableHash $keyOutputs
        }
        counts = [ordered]@{
            online_frames = $timeline.online_frames.Count
            branch_steps = $timeline.branch_steps.Count
            prediction_runs = $timeline.prediction_runs.Count
            artifact_refs = $timeline.artifact_refs.Count
            qoi_refs = $timeline.qoi_refs.Count
            checkpoint_refs = $timeline.checkpoint_refs.Count
            runtime_packets = $runtime.runtime_packets.Count
            state_checkpoints = $runtime.state_checkpoints.Count
            runtime_outputs = $runtime.runtime_outputs.Count
            typed_buffer_blobs = $runtime.typed_buffer_content.Count
        }
    }
}

$runA = "$RunIdPrefix.a"
$runB = "$RunIdPrefix.b"
Write-Host "FlightEnv checkpoint/replay determinism audit"
Write-Host "  run A   = $runA"
Write-Host "  run B   = $runB"
Write-Host "  stream  = $ExternalObservationStream"

$chainA = Invoke-Smoke -Prefix $runA -Build:(!$SkipBuild)
$chainB = Invoke-Smoke -Prefix $runB -Build:$false

$projectionA = Build-RunProjection -Prefix $runA -ChainDir $chainA -RunA $runA -RunB $runB
$projectionB = Build-RunProjection -Prefix $runB -ChainDir $chainB -RunA $runA -RunB $runB

New-Item -ItemType Directory -Force -Path $reportRoot | Out-Null
$projectionAPath = Join-Path $reportRoot 'checkpoint_replay_projection_a.json'
$projectionBPath = Join-Path $reportRoot 'checkpoint_replay_projection_b.json'
$projectionA.projections | ConvertTo-Json -Depth 96 | Set-Content -LiteralPath $projectionAPath -Encoding UTF8
$projectionB.projections | ConvertTo-Json -Depth 96 | Set-Content -LiteralPath $projectionBPath -Encoding UTF8

$mismatches = @()
foreach ($name in @('public_timeline', 'branch_index', 'runtime_packets', 'checkpoints', 'key_output_refs')) {
    if ($projectionA.hashes[$name] -ne $projectionB.hashes[$name]) {
        $mismatches += [ordered]@{
            category = $name
            run_a_hash = $projectionA.hashes[$name]
            run_b_hash = $projectionB.hashes[$name]
        }
    }
}

$summary = [ordered]@{
    schema_version = 'flightenv.platform.checkpoint_replay_determinism_audit.v1'
    generated_at_utc = [DateTime]::UtcNow.ToString('o')
    result = if ($mismatches.Count -eq 0) { 'pass' } else { 'fail' }
    note = 'This Phase4 gate validates deterministic replay/evidence equivalence over repeated identical runs. Executable restore from checkpoint is still a separate adapter/session capability.'
    configuration = $Configuration
    platform = $Platform
    external_observation_stream = $ExternalObservationStream
    projection_a_path = $projectionAPath
    projection_b_path = $projectionBPath
    run_a = $projectionA
    run_b = $projectionB
    mismatches = @($mismatches)
}
$summary | ConvertTo-Json -Depth 96 | Set-Content -LiteralPath $reportPath -Encoding UTF8

Write-Host "Report: $reportPath"
if ($mismatches.Count -gt 0) {
    Write-Host "Result: FAIL"
    foreach ($mismatch in $mismatches) {
        Write-Host ("  - {0}: {1} != {2}" -f $mismatch.category, $mismatch.run_a_hash, $mismatch.run_b_hash)
    }
    exit 1
}

Write-Host "Result: PASS"
