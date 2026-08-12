# Performance Notes

This is a template for repeatable SpaceLens benchmarks. Record measured results here only after the benchmark procedure and build configuration are documented.

## Methodology

- Build configuration:
- Compiler/toolchain:
- Commit or version:
- Warm-up procedure:
- Number of repetitions:
- Whether the filesystem cache was cold or warm:
- How background activity was controlled:
- How scan scope, exclusions, and error conditions were recorded:

Measure end-to-end scan time separately from UI rendering time where possible. Report incomplete scans and cancellation as outcomes, not successful throughput measurements.

## Hardware and OS

- CPU:
- Physical/logical cores:
- RAM:
- Storage device and filesystem:
- Windows version/build:
- Qt version:
- CMake version:

## Datasets

For each dataset, record the root path, file count, directory count, total logical bytes, approximate depth, file-size distribution, and whether reparse points or permission errors are present.

- Dataset A:
- Dataset B:
- Dataset C:

## Metrics

- Total wall-clock scan time
- Enumeration time
- Aggregation time
- Peak process working set
- Peak/steady worker count
- Progress update rate
- Cancellation response time
- Number of files/directories visited
- Number of skipped or inaccessible entries
- Top-K maintenance time, when measured separately
- UI responsiveness during an active scan

## Baseline table

| Dataset | Build | Workers | Files | Directories | Logical bytes | Scan time | Peak memory | Notes |
|---|---|---:|---:|---:|---:|---:|---:|---|
|  |  |  |  |  |  |  |  |  |

## Rules

1. Measure before optimizing.
2. Change one meaningful variable at a time.
3. Use the same dataset, scan policy, build configuration, and cache condition when comparing runs.
4. Include correctness checks and incomplete/error counts with performance results.
5. Prefer representative workloads over synthetic microbenchmarks alone.
6. Do not trade away cancellation, deterministic ownership, or UI responsiveness without a measured benefit and an explicit design decision.
