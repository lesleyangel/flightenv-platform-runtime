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

## Current MVP

`FlightEnvPlatformRuntimeHost.exe` implements the B2/B3/B4 runtime path:

- B2: C++ host project, initialization/preflight, branch registry, runtime
  cursor, timeline index, series manifest.
- B3: external observation stream input, one online frame per platform tick,
  recurrent seed handoff between frames.
- B4: prediction branches forked from online posterior outputs and executed in
  background workers without blocking the online mainline.

The operator execution backend still reuses the existing PDK compiled-workflow
runner while the C++ host owns scheduling, branching, progress, and evidence
aggregation. Native adapter sessions can be moved behind the same host boundary
in later phases.

## Build

```powershell
.\flightenv-platform-runtime\tools\build_platform_runtime.ps1 -Configuration Release -Platform x64
```

The executable is emitted under the shared workspace output root:

```text
_deps\workspace\x64\Release\FlightEnvPlatformRuntimeHost.exe
```

