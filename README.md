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
