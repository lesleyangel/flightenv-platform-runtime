# FlightEnv Platform Runtime

`flightenv-platform-runtime` is the production-facing runtime host repository for
the platform layer. It consumes compiled workflows and adapter registries, then
owns online clocks, external measurement input, prediction branches, and runtime
evidence indexing.

It intentionally does not contain reentry vehicle algorithms or object assets.
Those remain in:

- `flightenv-object-reentry-vehicle`: object package, resources, workflows,
  operator specs.
- `flightenv-reentry-operators`: object-specific AtomicOperator DLLs.
- `flightenv-platform-pdk`: schemas, headers, validators, and compiler tools.

Runtime defaults are object-driven. `FlightEnvPlatformRuntimeHost.exe` reads the
loaded object's `runtime/platform_runtime_profile.json` through
`object/twin_object.json::platform_runtime_profile` and uses it to resolve
online/future workflow roles, branch display templates, field display roles,
state labels, health-ledger metric keys, adapter registry candidates, and
external observation stream candidates. Reentry-specific meanings should be
declared in the object package, not added to this platform host.

## Current Runtime

`FlightEnvPlatformRuntimeHost.exe` implements the B2/B3/B4 runtime path and the
native adapter-session execution backend:

- B2: C++ host project, initialization/preflight, branch registry, runtime
  cursor, timeline index, series manifest.
- B3: external observation stream input, one online frame per platform tick,
  recurrent seed handoff between frames.
- B4: prediction branches forked from online posterior outputs and executed in
  background workers without blocking the online mainline.
- R3 event ledger: each committed online posterior frame writes a
  `posterior_frame_committed` event; prediction branches carry the triggering
  event id through queued/running/completed/failed evidence.
- Native adapter sessions: the default backend is `native_adapter_sessions`.
  The host loads DLL adapters in-process, creates one session per node, runs
  `initialize`/`warmup` once, then reuses the session across online frames or
  prediction steps.
- External process sessions: `json_file.v1` and `python_worker.v1` run through
  the same `IAdapterSession` lifecycle by writing request/response JSON files
  and launching the configured command directly from the C++ host.
- Declared external sessions: `ros2_node.v1`, `onnx_runtime.v1`, and
  `db_query.v1` can be preflighted and recorded in lifecycle evidence; their
  `execute` path is intentionally not implemented yet and fails explicitly.
- Health ledger: the host writes `health_ledger.json` and
  `health_ledger_summary.json` to connect online posterior state, prediction
  branch health summaries, checkpoint refs, and next-run seed policy.
- Event-driven multirate dispatch: `RuntimeTimeScheduler` and the host runtime
  drive work through a single event queue abstraction for `input_arrived`,
  `node_due`, `public_tick`, `checkpoint_due`, `branch_triggered`, and
  `stop_check_due` event kinds. Nodes become executable only after
  `RuntimeReadyQueueExecutor` admits the due event by checking declared
  dependencies, explicit edge-bound inputs, resource locks, parallel groups,
  max parallelism, and deadline status. The runner records
  `ready_queue_admission`, `runtime_event_kind`, `runtime_event_time_s`,
  `effective_delta_t_s`, `output_period_s`, and held-output evidence in
  `scheduler_timeline.json` and `runtime_loop_summary.json`.
- Runtime time scheduler: node cadence, public output time, held/carry-forward
  decisions, and `runtime_time` evidence are centralized in
  `RuntimeTimeScheduler`. `NativeWorkflowRunner` consumes these dispatch
  decisions and owns adapter execution; `PlatformRuntimeHost` owns online
  mainline and prediction-branch evidence aggregation.
- Runtime service split: the host is being decomposed into small platform
  services so future scheduler, adapter and evidence changes do not keep
  growing `NativeWorkflowRunner` and `PlatformRuntimeHost`. Current service
  boundaries are:
  `RuntimeClock` for wall/steady time helpers,
  `RuntimeEventLoop` for generic event-loop bookkeeping,
  `RuntimeReadyQueueExecutor` for compiled scheduler-plan loading, ready-queue
  admission checks, edge-port readiness evidence, resource/parallel occupancy,
  and admission evidence,
  `RuntimeAdapterInvoker` for adapter execute plus typed-output/zero-copy
  validation,
  `RuntimePortPacketWriter` for turning each node output port into a
  versioned `RuntimePacket` without copying large payloads,
  `RuntimePortStoreView` for read-only packet-to-port-ref evidence views over
  `ThreadSafePortStore`,
  `RuntimePublicFrameBuilder` for public-tick frame assembly,
  `RuntimePublicFramePolicy` for stop/held/public-frame evidence rules,
  `RuntimeTimelineMaterializer` for branch-step, artifact-ref, and QoI-ref
  timeline entries,
  `RuntimeMaterialization` for public/data-plane frame entries,
  `RuntimeEvidenceWriter` for run-package JSON evidence writes, and
  `RuntimeBranchService` for branch records and runtime branch events.
- Strict edge binding: compiled workflows now carry `edge_binding_plan.json`.
  `NativeWorkflowRunner` consumes this plan directly and defaults to explicit
  `source_node_id/source_port_id -> target_node_id/target_port_id` binding.
  The old contract/name-based implicit input injection is disabled by default
  and exists only behind `FLIGHTENV_ALLOW_IMPLICIT_CONTRACT_PORT_BINDING=1` for
  migration diagnostics. This prevents values with the same DTO contract but
  different roles or times from being guessed incorrectly.
- Runtime time core: common loop-tick types, node clock state, and the event
  queue live under `FlightEnvPlatformRuntime/time`. The hot path is now driven
  by the queue rather than by polling all nodes on every public tick. Non-integer
  node cadence is represented by `RuntimeTimePoint` / `RuntimeDuration` values
  backed by integer nanoseconds inside the event queue and scheduler; JSON
  evidence still emits `*_s` fields for readability and also records `*_ns`
  fields where timing provenance matters. Branch creation is now represented by
  a neutral `branch_triggered` event with a separate cause event, so future
  branch handoff is auditable without encoding object semantics in the runtime.
  Tensor alignment can select nearest refs or lazy operation refs for later
  specialized interpolation backends.
- Runtime input alignment skeleton: `RuntimePortSampleBuffer` records generic
  node and port samples, while `RuntimeInputAlignment` resolves declared
  `input_alignment` policies into hold-last, nearest, scalar linear, or scalar
  window-integration evidence. The runner only fills missing upstream inputs
  from this platform buffer; it does not override direct graph connections.
- Runtime tensor alignment and materialization: scalar alignment still computes
  numeric values in-process, while tensor-like values go through
  `RuntimeTensorInterpolator`. The first production method is `nearest`, which
  returns the closest existing tensor/artifact ref and records alignment
  evidence; `lazy_ref` remains available for later reducer/interpolator
  backends. Data-plane public entries are emitted through
  `RuntimeMaterialization`, while public tick loop frames and held outputs are
  assembled through `RuntimePublicFrameBuilder` with shared
  `RuntimePublicFramePolicy`. Held outputs now include port-level carried
  value summaries that prefer typed-buffer, tensor, artifact, or typed-payload
  refs instead of duplicating payloads. Node-level packets remain for
  compatibility, while `RuntimePortPacketWriter` also writes per-output-port
  packets into `ThreadSafePortStore`; `RuntimePortStoreView` prefers those
  port packets and falls back to the legacy node packet when needed. UI/replay
  timeline entries use `RuntimeTimelineMaterializer` so runtime evidence and
  run indexes share the same basic public-time fields.
- Branch/time scoped PortStore: port packets carry `branch_id` and
  `timeline_id` tags and are indexed by
  `branch_id + timeline_id + node_id + port_id + time`. The runtime can query
  the latest scoped packet or the nearest packet at-or-before a target logical
  timestamp. `runtime_packets.json`, `runtime_evidence.json`, and ready-queue
  checked-port evidence expose the scoped packet path for audit.
- Typed DTO hot-path gate: operator ports that declare
  `typed_io_contract.json_operator_io_forbidden` or
  `json_hot_path_forbidden` are not allowed to remain inline-only JSON on the
  runtime hot path. `RuntimeZeroCopyPolicy` is the single runtime gate for this
  rule: typed-only outputs must carry `typed_buffer_ref`, `tensor_ref`, or
  `artifact_ref`, otherwise execution fails fast. Large field/tensor values
  continue to move by reference. `RuntimeTypedBufferStore` writes short-path
  `tb/*.bin` shadow artifacts for replay/debug evidence and emits
  `runtime://typed-buffer/<id>` refs with schema, DTO, layout, dtype, shape,
  byte-size, lifecycle/refcount, ownership/access, and zero-copy eligibility
  metadata. Upstream typed buffers are retained when passed into downstream
  typed ABI calls, so future parallel branch sharing can be audited. Shadow
  artifacts are written for replay/debug evidence outside the adapter hot path;
  set `FLIGHTENV_TYPED_BUFFER_SHADOW_SYNC=1` only when a debugging session needs
  synchronous shadow writes. The old `json_typed_payload.v1` bridge is disabled
  by default and can only be re-enabled for migration diagnostics with
  `FLIGHTENV_ALLOW_INLINE_JSON_TYPED_PAYLOAD_BRIDGE=1`; it is not a production
  operator-facing hot path contract.
- Adapter typed ABI v2 foundation: the PDK adapter ABI now defines typed buffer
  views, a runtime-owned allocator, typed execute requests/results, and typed
  result release hooks. Existing adapters can keep using v1 JSON lifecycle
  calls; the runtime probes v2 exports and records availability in session
  evidence. The ballistic adapter is the first real `execute_typed_v2` path: it
  reads upstream generated DTO typed buffers and writes `state.next` directly
  into runtime-owned typed output buffers. Set
  `FLIGHTENV_TYPED_BUFFER_TRACE=1` when low-level typed buffer allocation traces
  are needed. Set `FLIGHTENV_DISABLE_TYPED_ADAPTER_ABI_V2=1` only for A/B
  debugging or performance comparison against the v1 JSON adapter ABI.

The old PDK compiled-workflow process runner is deprecated and disabled by
default. It remains available only as an explicit compatibility backend for
historical replay or migration diagnostics:

```powershell
--execution-backend compiled_workflow_process_backend --allow-legacy-process-backend
```

Normal platform runs should use the default:

```powershell
--execution-backend native_adapter_sessions
```

The hot path no longer launches a separate PDK workflow process per frame or
branch. The PDK CLI is still used after a run to build UI/evidence indexes.

Runtime evidence proves adapter lifecycle reuse through:

- `adapter_lifecycle_log.json`
- `adapter_session_summary.json`
- `health_ledger.json` / `health_ledger_summary.json`
- `native_runner_trace.log`
- `runtime_host_evidence.json` / `online_native_session_summary`
- `runtime_events.json` / `posterior_frame_committed ->
  prediction_branch_queued/completed/failed`
- per-workflow `runtime_evidence.json` / `runtime_packets.json`, where the R1
  bridge records `RuntimePacket`, `ThreadSafePortStore`, `ReadyQueueScheduler`,
  and `ThreadPoolExecutorDescriptor` usage from the PDK runtime core.

CLI/Python JSON-file adapters are runnable but pay process startup cost per
lifecycle event. ROS2/ONNX/DB protocols are currently declaration/evidence
contracts until their concrete sessions are implemented. For zero
process-boundary overhead in the main reentry chain, object operators should
use the DLL ABI.

## Build

```powershell
.\flightenv-platform-runtime\tools\build_platform_runtime.ps1 -Configuration Release -Platform x64
```

The executable is emitted under the shared workspace output root:

```text
_deps\workspace\x64\Release\FlightEnvPlatformRuntimeHost.exe
```

## Smoke

```powershell
.\flightenv-platform-runtime\tools\run_cpp_runtime_host_smoke.ps1 `
  -Configuration Release `
  -Platform x64 `
  -OnlineFrames 2 `
  -PredictionEveryFrames 2 `
  -FutureMaxIterations 2 `
  -ExecutionBackend native_adapter_sessions
```

The smoke script cleans the generated run package for its `-RunIdPrefix` by
default so timing assertions never read stale branch artifacts. Use
`-KeepPreviousArtifacts` only when intentionally comparing with a previous run.

Round2 multirate dispatch audit:

```powershell
.\flightenv-platform-runtime\tools\audit_multirate_runtime_round2.ps1 `
  -Configuration Release `
  -Platform x64
```

Runtime scheduler Gate A-G acceptance:

```powershell
.\flightenv-platform-runtime\tools\verify_runtime_scheduler_gates.ps1 `
  -Configuration Release `
  -Platform x64
```

From the workspace root, the same gate is available as:

```powershell
.\tools\verify_platform_runtime_scheduler_gates.ps1 `
  -Configuration Release `
  -Platform x64
```

Use `-StaticOnly` for a fast Gate A/G architecture and documentation check.
Use `-SkipSlow` when only build, negative binding, and synthetic multirate
checks are needed. Use `-RequireTypedZeroCopy` before release when typed ABI v2
zero-copy behavior must be enforced by the same gate.

Scheduler 100% staged acceptance from the workspace root:

```powershell
.\tools\verify_platform_scheduler_100_acceptance.ps1 `
  -Configuration Release `
  -Platform x64 `
  -SkipBuild `
  -SkipSlow
```

The staged acceptance wraps the existing scheduler gate, records the current
clock/scheduler boundary audit, runs the ReadyQueue behavior matrix, optionally
runs the Phase 4 checkpoint/replay determinism audit, optionally runs the Phase
5 stress/fault/backpressure audit, and writes
`_local_artifacts/platform-runtime/scheduler-acceptance/scheduler_100_acceptance_report.json`.
Use `-StaticOnly -SkipBuild` for a text-only architecture/boundary scan. Use
`-RunReplayDeterminism` when Phase 4 should be part of the current gate. Use
`-RunStress` when Phase 5 should be part of the current gate. Use `-StrictFinal`
only when closing the final 100% target; any deferred item then becomes a hard
failure.

Runtime time-boundary audit for Phase 2:

```powershell
.\flightenv-platform-runtime\tools\audit_runtime_time_boundary.ps1
```

This audit allows `*_s` fields at JSON/evidence/CLI boundaries, but blocks known
hot-path regressions where scheduler, ReadyQueue, sample alignment, or tensor
nearest logic compares logical time with floating-point seconds instead of
runtime nanoseconds.

ReadyQueue behavior audit for Phase 3:

```powershell
.\flightenv-platform-runtime\tools\audit_ready_queue_behavior.ps1 `
  -Configuration Release `
  -Platform x64
```

This audit builds `ReadyQueueBehaviorAudit.exe` and verifies blocked
dependencies, missing explicit ports, scoped port-store readiness, resource
locks, max parallelism, capacity-group saturation, exclusive dispatch, and
deadline policy behavior using the production `RuntimeReadyQueueExecutor`.

Checkpoint/replay determinism audit for Phase 4:

```powershell
.\flightenv-platform-runtime\tools\audit_checkpoint_replay_determinism.ps1 `
  -Configuration Release `
  -Platform x64 `
  -RunIdPrefix gate_phase4_smoke `
  -OnlineFrames 2 `
  -PredictionEveryFrames 1 `
  -FutureMaxIterations 1 `
  -BranchChunkIterations 1 `
  -SkipBuild
```

This audit runs the same RuntimeHost scenario twice and compares normalized
semantic projections for public timeline, branch index, runtime packets,
checkpoint refs, and key output refs. It proves repeated-run determinism for
the scheduler evidence surface; executable adapter/session restore from a saved
checkpoint remains a later production feature.

Stress/fault/backpressure audit for Phase 5:

```powershell
.\flightenv-platform-runtime\tools\audit_scheduler_stress_fault_backpressure.ps1 `
  -Configuration Release `
  -Platform x64 `
  -EventCount 10000 `
  -BranchCount 64
```

This audit builds `SchedulerStressFaultAudit.exe` and checks mixed-event queue
pressure, ReadyQueue max-parallelism and capacity-group backpressure, exclusive
resource-lock storms, branch/time scoped port-store reads, deadline fail versus
mark-stale policy, and typed zero-copy fail-fast behavior. Adapter lifecycle
retry/cancel/restart and long-run memory thresholds are still deeper production
gates, not replaced by this synthetic audit.

Typed ABI v2 A/B comparison:

```powershell
.\flightenv-platform-runtime\tools\compare_typed_abi_perf.ps1 `
  -Configuration Release `
  -Platform x64 `
  -RunIdPrefix typed_abi_full_acceptance `
  -OnlineFrames 3 `
  -PredictionEveryFrames 1 `
  -FutureMaxIterations 2
```

The script runs the same runtime smoke twice: first with
`FLIGHTENV_DISABLE_TYPED_ADAPTER_ABI_V2=1`, then with typed ABI v2 enabled. It
writes the comparison report under
`_local_artifacts/platform-runtime/perf-reports`.
