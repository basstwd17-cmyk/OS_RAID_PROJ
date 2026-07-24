# MQSim RAID0 + SWANS

This is the active research branch. It keeps the original `MQSim-master`
directory layout, the RAID0 request path, and the current SWANS placement and
migration implementation. Use `main` when only the stable RAID0 path is needed.

## Repository branches

- `baseline/mqsim-output`: original MQSim behavior plus comparable output
  metrics.
- `main`: the stable RAID0 implementation.
- `develop/swans`: the current RAID0 + SWANS research implementation.

Each branch is a complete buildable source tree. Check out only the variant you
need; no source folder has to be copied out of another branch.

## What is tracked

- C++ source and bundled RapidXML headers
- GNU Make build definition
- SSD and synthetic workload configuration files
- FAST'18 example configuration files
- SWANS policy unit tests and paper-like input configurations
- documentation and licenses

Build products, IDE state, raw traces, generated `*_scenario_*.xml` reports,
logs, archives, and experiment result data are ignored.

## Build

Requirements are GNU Make and a C++11 compiler.

```sh
make -j4
make test
```

The executable is `MQSim` on Linux/macOS and `MQSim.exe` on Windows with
MinGW/MSYS2.

## Run the included synthetic workload

```sh
./MQSim -i ssdconfig.xml -w workload.xml
```

On PowerShell:

```powershell
.\MQSim.exe -i ssdconfig.xml -w workload.xml
```

The simulator writes a generated report beside the workload file, such as
`workload_scenario_1.xml`. These reports are deliberately not versioned.

The test target builds and runs:

- `zone_directory_mapping_test`
- `wear_leveling_policy_test`
- `migration_executor_test`

## SWANS scope

- logical zone directory and per-stream block-write tracking
- write-request-based device wear accounting
- normal, redirect, and migration policy states
- completion-driven migration state machine
- buffered writes, replay, source discard, and backpressure statistics
- the common RAID0 correctness fixes also retained on `main`

## Configuration

`ssdconfig.xml` contains the RAID settings:

```xml
<SSD_Count>4</SSD_Count>
<Stripe_Unit_LBA>512</Stripe_Unit_LBA>
<SWANS_Enabled>true</SWANS_Enabled>
```

The included `workload.xml` is synthetic and uses only valid channels and chips
for the included SSD configuration, so it can be run without external data.
Matched RAID0/SWANS paper-like inputs are under `experiments/paper_like/`;
their raw traces must be supplied separately.

## Current research limitations

- `PAGE_LEVEL` is the supported mapping mode. The upstream `HYBRID` mapping
  class is incomplete.
- Policy tests cover zone mapping, wear decisions, and the single-migration
  state machine; they are not a full-system correctness proof.
- Keep `SWANS_Max_Concurrent_Migrations` at `1`. Cross-zone replay with multiple
  simultaneous migrations has not been fully validated.
- Source-discard support is implemented for page-level mapping. Other mapping
  modes must not be used for SWANS migration.

## Trace workloads

Raw traces are not stored in this repository. Put local trace files in an
ignored `traces/` directory and reference them from a workload XML using a
relative path. This keeps the source repository small and portable while making
the trace dependency explicit.

## Upstream

This project is derived from MQSim. See `LICENSE` and `fast18/README.md` for the
upstream license and FAST'18 example notes.
