# MQSim output-comparison baseline

This branch contains a clean MQSim source tree with the additional XML metrics
used to compare a single SSD against the RAID0 and SWANS variants. The directory
layout follows the original `MQSim-master` tree, while generated binaries,
traces, and simulation results are intentionally excluded.

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

## Comparison output added on this branch

- normalized 100-bin total/read/write IOPS per flow
- host write bytes and approximate flash write amplification
- flash page program and erase counts
- per-SSD and per-plane erase-count distribution and histogram

These additions change reporting only; the included baseline keeps the original
single-SSD execution path.

## Trace workloads

Raw traces are not stored in this repository. Put local trace files in an
ignored `traces/` directory and reference them from a workload XML using a
relative path. This keeps the source repository small and portable while making
the trace dependency explicit.

## Upstream

This project is derived from MQSim. See `LICENSE` and `fast18/README.md` for the
upstream license and FAST'18 example notes.
