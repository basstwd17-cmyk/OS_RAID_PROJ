# MQSim RAID0

This is the stable RAID0 branch. It keeps the original `MQSim-master` directory
layout and adds a host-side RAID controller that splits requests across
independent MQSim SSD backends. SWANS policy, migration, and zone-remapping code
is intentionally absent from this branch.

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
- documentation and licenses

Build products, IDE state, raw traces, generated `*_scenario_*.xml` reports,
logs, archives, and experiment result data are ignored.

## Build

Requirements are GNU Make and a C++11 compiler.

```sh
make -j4
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

## RAID0 scope

- configurable SSD count and stripe-unit size
- stripe-aware request splitting and parent-request completion aggregation
- per-SSD request, sector, latency, and flash wear statistics
- RAID-level completion latency and completion-skew statistics
- output fields aligned with `baseline/mqsim-output`

The implementation starts from the last RAID0-only revision before SWANS and
selectively includes the later fixes that are independent of SWANS:

- trace replay arrival times no longer double-count the replay offset
- flash validity bitmaps use a modulo-64 bit index
- `Ideal_Mapping_Table` is serialized from the correct setting
- RAID request latency starts at the host request initiation time

This preserves the simple RAID0 path while retaining the general correctness
improvements found during later SWANS work.

## Configuration

`ssdconfig.xml` contains the RAID settings:

```xml
<SSD_Count>8</SSD_Count>
<Stripe_Unit_LBA>4</Stripe_Unit_LBA>
```

The included `workload.xml` is synthetic and uses only valid channels and chips
for the included SSD configuration, so it can be run without external data.

## Trace workloads

Raw traces are not stored in this repository. Put local trace files in an
ignored `traces/` directory and reference them from a workload XML using a
relative path. This keeps the source repository small and portable while making
the trace dependency explicit.

Use `PAGE_LEVEL` address mapping. The upstream `HYBRID` mapping class is retained
for source compatibility but is not implemented as a complete runnable FTL.

## Upstream

This project is derived from MQSim. See `LICENSE` and `fast18/README.md` for the
upstream license and FAST'18 example notes.
