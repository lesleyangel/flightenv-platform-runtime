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
- Event-driven multirate dispatch: `RuntimeTimeScheduler` seeds a single
  `RuntimeEventQueue` with `node_due`, `public_tick`, `input_arrived`, and
  `checkpoint_due` event kinds. Nodes execute on `node_due` events at their
  declared cadence, while `public_tick` events materialize loop summaries and
  carry forward held outputs for downstream consumers. The runner records
  `runtime_event_kind`, `runtime_event_time_s`, `effective_delta_t_s`,
  `output_period_s`, and held-output evidence in `scheduler_timeline.json` and
  `runtime_loop_summary.json`.
- Runtime time scheduler: node cadence, public output time, held/carry-forward
  decisions, and `runtime_time` evidence are centralized in
  `RuntimeTimeScheduler`. `NativeWorkflowRunner` consumes these dispatch
  decisions and owns adapter execution; `PlatformRuntimeHost` owns online
  mainline and prediction-branch evidence aggregation.
- Runtime time core: common loop-tick types, node clock state, and the event
  queue live under `FlightEnvPlatformRuntime/time`. The hot path is now driven
  by the queue rather than by polling all nodes on every public tick. Non-integer
  node cadence is represented as exact `node_due` event times within the current
  double-precision clock model; tensor alignment can select nearest refs or lazy
  operation refs for later specialized interpolation backends.
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
  `RuntimeMaterialization` instead of being assembled directly in the runner.
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
