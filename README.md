# MQSim RAID0 + SWANS

This repository is an MQSim-based storage simulator with a host-side RAID0
controller and an optional SWANS placement and migration policy. The default
branch, `main`, supports both modes from the same source tree.

## Modes

Choose the mode in the SSD configuration XML.

### RAID0 only

```xml
<SSD_Count>4</SSD_Count>
<Stripe_Unit_LBA>512</Stripe_Unit_LBA>
<SWANS_Enabled>false</SWANS_Enabled>
```

With SWANS disabled, requests use the regular RAID0 stripe mapping path. Zone
mapping, policy evaluation, redirect, and migration are not initialized.

### RAID0 + SWANS

```xml
<SSD_Count>4</SSD_Count>
<Stripe_Unit_LBA>512</Stripe_Unit_LBA>
<SWANS_Enabled>true</SWANS_Enabled>
```

When enabled, SWANS tracks logical zones and per-stream block writes, measures
write imbalance across SSDs, and can redirect new writes or migrate data between
SSDs. Configure its zone size, epoch durations, thresholds, and migration
limits with the accompanying `SWANS_*` XML parameters.

Use the boolean strings `true` and `false` for `SWANS_Enabled`.

## Repository branches

- `main`: the active implementation; supports RAID0-only and RAID0 + SWANS
  through XML configuration.
- `archive/raid0-only`: preserved RAID0-only implementation from before the
  SWANS integration.
- `baseline/mqsim-output`: original MQSim behavior with comparable output
  metrics, for baseline comparisons.

Each branch is a complete buildable source tree.

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
- RAID0 request splitting, completion aggregation, and per-SSD statistics

Matched RAID0/SWANS paper-like inputs are under `experiments/paper_like/`.
Their raw trace files must be supplied separately.

## Current research limitations

- `PAGE_LEVEL` is the supported mapping mode. The upstream `HYBRID` mapping
  class is incomplete.
- Policy tests cover zone mapping, wear decisions, and the single-migration
  state machine; they are not a full-system correctness proof.
- Keep `SWANS_Max_Concurrent_Migrations` at `1`. Cross-zone replay with
  multiple simultaneous migrations has not been fully validated.
- Source-discard support is implemented for page-level mapping. Other mapping
  modes must not be used for SWANS migration.

## Trace workloads

Raw traces are not stored in this repository. Put local trace files in an
ignored `traces/` directory and reference them from a workload XML using a
relative path. This keeps the source repository small and portable while making
the trace dependency explicit.

## Upstream

This project is derived from MQSim. See `LICENSE` and `fast18/README.md` for
the upstream license and FAST'18 example notes.
