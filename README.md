# MQSim RAID0 + SWANS

**A research simulator for studying write distribution, data migration, and SSD wear across a RAID0 array.**

This project extends [MQSim](https://github.com/CMU-SAFARI/MQSim), the FAST 2018 SSD simulator, with a host-side RAID0 controller and an optional SWANS placement/migration policy. Compare ordinary RAID0 with policies that redistribute writes and move logical zones between SSDs, while observing the underlying NAND activity.

This is simulation software, not a RAID driver or firmware for physical SSDs. The extensions are under research evaluation; functional tests do not establish a real-device lifetime improvement.

[Build](#build) · [First run](#first-run-without-an-external-trace) · [Trace workloads](#run-your-own-trace) · [Settings](#key-settings) · [Outputs](#understand-the-results) · [Reference](docs/configuration-reference.md)

## What changes from MQSim?

MQSim supplies NVMe/SATA host interfaces, the flash translation layer (FTL), NAND timing, caching, garbage collection (GC), and device-level wear-leveling models. This project builds an array-level research layer around those components.

| Area | Additions on `main` |
|---|---|
| Multi-SSD array | RAID0 striping, request splitting, completion aggregation, and per-SSD accounting |
| Placement and migration | Logical zone directory, write-imbalance tracking, redirect decisions, and completion-driven inter-SSD migration |
| Migration interaction | Deferred writes, backpressure, source discard, and coordination with FTL GC barriers |
| Endurance | Bad-block retirement, remaining-overprovisioning end-of-life (EOL), and a snapshot at the first EOL event |
| Observability | Host bytes, NAND programs/erases, bad blocks, migration records, and CSV telemetry |
| Correctness fixes | Initial-read allocation/accounting, GC page metadata initialization, and GC retry after outstanding I/O completes |

Previously in this codebase, an unmapped read used a write-allocation path and incremented an outstanding-program counter without an actual NAND program to complete it. The fix separates initial-read allocation from write accounting. This was internal GC bookkeeping, not simply an extra completed host write in the statistics. These fixes describe this project's history, not an audit of the latest upstream MQSim.

```text
Synthetic workload or trace
            |
     RAID0 controller
     + optional SWANS zone placement / migration
            |
     SSD 0   SSD 1   ...   SSD N-1
       |       |              |
     MQSim FTL, GC, scheduling and NAND models
            |
     XML reports + CSV telemetry
```

## Choose a branch

Status checked on **2026-11-06**. Record the commit SHA used for every experiment.

| Branch | Purpose | Reference commit |
|---|---|---|
| [`main`](https://github.com/basstwd17-cmyk/OS_RAID_PROJ/tree/main) | Merged RAID0/SWANS baseline, including initial-read and GC progress fixes | `dd867a9` |
| [`experiment/tec-swans-src-only`](https://github.com/basstwd17-cmyk/OS_RAID_PROJ/tree/experiment/tec-swans-src-only) | Additional policies from PR #3, with Makefile header dependency tracking | `aba1b9d` |

The experiment branch adds `BAD_BLOCK_COUNT` / `TEC` / `WRITE_BYTES` wear metrics, configurable migration triggers and zone selection, two-stage OP-zone exchange, trace-pool replay, and OP-ratio/TEC EOL modes. It also accepts an optional `-s` SWANS configuration file. These extensions are **not yet merged into `main`**.

The experiment branch retains the `main` XML files: selecting the branch does not automatically enable TEC or OP-zone exchange. Its added sources match PR #3 at `5e9e2fe`. See that snapshot's [configuration examples](https://github.com/basstwd17-cmyk/OS_RAID_PROJ/tree/5e9e2fe6d42831c23d6720bd3169735b5fbc628b/examples/advanced-swans) and [policy documentation](https://github.com/basstwd17-cmyk/OS_RAID_PROJ/tree/5e9e2fe6d42831c23d6720bd3169735b5fbc628b/docs); they are not bundled in the source-only branch.

## Build

Requirements: Git (or download a ZIP), GNU Make, and a C++11-capable compiler. Python 3 is only needed for the first-run example below. Raw traces and prebuilt executables are not bundled.

```sh
git clone --branch main --single-branch https://github.com/basstwd17-cmyk/OS_RAID_PROJ.git
cd OS_RAID_PROJ
git rev-parse HEAD
```

For the experiment branch, replace `--branch main` with `--branch experiment/tec-swans-src-only`. Use separate checkout folders to compare versions.

### Windows / PowerShell

Install a MinGW-compatible C++ toolchain and put its `bin` directory on `PATH`. Both `g++` and `mingw32-make` must be available in the same terminal.

```powershell
g++ --version
mingw32-make --version
mingw32-make -j4
```

The result is `MQSim.exe`. Keep runtime DLLs required by your toolchain available through `PATH`. This workflow was checked with LLVM-MinGW. The Visual Studio `.sln` files described by upstream are not included here.

### Linux

With GNU Make and `g++` installed:

```sh
make -j4
```

The result is `MQSim`. Linux build rules are present; the validation reported below was performed on Windows.

On `main`, run `make clean` before rebuilding after header changes or switching source versions (`mingw32-make clean` on Windows). The experiment branch additionally tracks header dependencies through `.d` files. Neither of these two branches defines `make test`.

## First run without an external trace

The root `workload.xml` includes multiple scenarios, one of which references the **unbundled** `traces/src1_1.trace`. Start with a small independent input instead of running that file unchanged.

Save this as `quickstart.py` in the repository root, then run `python quickstart.py` (`python3` where appropriate). It creates `demo/` without modifying the root XML files.

```python
from pathlib import Path
from copy import deepcopy
import xml.etree.ElementTree as ET

out = Path("demo")
out.mkdir(exist_ok=True)
ssd = ET.parse("ssdconfig.xml")
settings = {
    "SSD_Count": 4, "Enabled_Preconditioning": "false",
    "Flash_Channel_Count": 1, "Chip_No_Per_Channel": 1,
    "Die_No_Per_Chip": 1, "Plane_No_Per_Die": 1,
    "Block_No_Per_Plane": 128, "Page_No_Per_Block": 32,
    "Page_Capacity": 4096, "IO_Queue_Depth": 64,
    "Queue_Fetch_Size": 16, "Data_Cache_Capacity": 0,
    "Stripe_Unit_LBA": 128, "SWANS_Zone_Size_LBA": 128,
    "SWANS_Enabled": "true", "RAID_Telemetry_Enabled": "true",
}
for tag, value in settings.items():
    ssd.find(".//" + tag).text = str(value)
ssd.write(out / "ssdconfig.xml", encoding="utf-8", xml_declaration=True)

flow = deepcopy(ET.parse("workload.xml").find(".//IO_Flow_Parameter_Set_Trace_Based"))
fields = {
    "Channel_IDs": "0", "Chip_IDs": "0", "Die_IDs": "0", "Plane_IDs": "0",
    "Initial_Occupancy_Percentage": "0", "Device_Level_Data_Caching_Mode": "TURNED_OFF",
    "File_Path": "demo/io.trace", "Percentage_To_Be_Executed": "100",
    "Relay_Count": "1", "Time_Unit": "NANOSECOND",
}
for tag, value in fields.items():
    flow.find(tag).text = value
workload = ET.Element("MQSim_IO_Scenarios")
ET.SubElement(workload, "IO_Scenario").append(flow)
ET.ElementTree(workload).write(out / "workload.xml", encoding="utf-8", xml_declaration=True)

with (out / "io.trace").open("w", encoding="ascii") as trace:
    for i in range(1000):
        operation = 1 if i % 4 == 0 else 0  # 1 = read; 0 = write
        trace.write(f"{(i + 1) * 1000000} 0 {i * 8} 8 {operation}\n")
print("Created demo/ssdconfig.xml, demo/workload.xml and demo/io.trace")
```

Run from the repository root:

```powershell
# Windows
.\MQSim.exe -i demo/ssdconfig.xml -w demo/workload.xml
```

```sh
# Linux
./MQSim -i demo/ssdconfig.xml -w demo/workload.xml
```

The program waits for Enter after reporting completion. For automated runs, provide a newline or close standard input.

Expected: **1,000 completed host requests, including 750 writes**, in `demo/workload_scenario_1.xml`, plus `demo/workload_scenario_1.telemetry.csv`. This example checks startup and reporting; it is not designed to reach EOL or establish a migration benefit.

## Run your own trace

Use `demo/workload.xml` as a **single-flow template**, save it as a separate file such as `workload_trace.xml`, and change `File_Path`. Relative trace paths resolve from the process working directory, so use an absolute path or always launch from the repository root.

Match `Channel_IDs`, `Chip_IDs`, `Die_IDs`, and `Plane_IDs` to the SSD configuration. To use all resources in the root `ssdconfig.xml`, these lists are respectively `0,1`, `0`, `0,1,2,3`, and `0,1,2,3`.

Input is five space-separated fields, with no CSV header:

```text
arrival_time device_id start_LBA sector_count operation
```

For example, a 4 KiB write followed by a read of the same address:

```text
1000000 0 0 8 0
2000000 0 0 8 1
```

LBAs and request sizes use **512-byte sectors**; `0` means write and `1` means read. Use nondecreasing timestamps and a matching `Time_Unit` (nanoseconds above). Raw MSRC CSV files must be converted first. A developer's local `analysis/` conversion scripts are not part of a fresh clone.

Set `Relay_Count=1` for one pass. `Relay_Count=0` requests replay until EOL; with `main`'s OP-based EOL, bad-block retirement must be enabled. Confirm finite runs complete before endurance experiments.

```powershell
.\MQSim.exe -i ssdconfig.xml -w workload_trace.xml
```

On Linux, use `./MQSim` with the same arguments. Create `workload_trace.xml` first; it is not bundled. Give each run a separate workload path/directory to avoid overwriting reports.

## Key settings

The root `ssdconfig.xml` is an experiment configuration, not a calibrated commercial SSD model.

| Setting | Root value | Meaning |
|---|---|---|
| `SSD_Count` | `4` | SSD backends |
| `Stripe_Unit_LBA` / `SWANS_Zone_Size_LBA` | `32768` / `32768` | 16 MiB stripe and zone |
| `SWANS_Enabled` | `true` | Set `false` for ordinary RAID0 |
| `SWANS_Epoch_Placement` / `SWANS_Epoch_Migration` | `80000000000` / `120000000000` | 80 s / 120 s of simulated time |
| `SWANS_Balance_Unit_Bytes` | `1048576` | One write unit = cumulative bytes / 1 MiB |
| `SWANS_Max_Concurrent_Migrations` | `1` | Keep at one for the evaluated path |
| `SWANS_Migration_Working_Queue_Limit` | `64` | Waiting requests per migration task, not a 64-byte buffer |
| `Block_PE_Cycles_Limit` | `25` | Accelerated endurance setting |
| `Overprovisioning_Ratio` / `End_of_Life_Threshold` | `0.20` / `0.05` | Initial NAND OP / remaining-OP EOL threshold |
| `GC_Exec_Threshold` / `GC_Hard_Threshold` | `0.10` / `0.005` | Converted to block counts using initial blocks per plane |
| `RAID_Telemetry_Period` | `40000000000` | 40 s sampling, plus event/final samples |

The root geometry models 32 GiB raw NAND per SSD before OP, with TLC, 4 KiB pages, and no device data cache. GC thresholds are not recalculated to exclude retired blocks. Raising thresholds and changing the denominator have different effects; neither alone proves a GC progress issue is resolved.

For RAID0/SWANS comparisons, hold trace, seed, geometry, timing, and other settings constant and change only `SWANS_Enabled`. Cross-SSD SWANS policies are distinct from the within-SSD dynamic/static wear-leveling options. See the [configuration reference](docs/configuration-reference.md) for inherited parameters and output fields.

## Understand the results

For `workload_trace.xml`, scenario 1 produces:

| Artifact | Contents |
|---|---|
| `workload_trace_scenario_1.xml` | Host completion/performance, RAID policy and migration records, per-SSD wear, EOL/final snapshots |
| `workload_trace_scenario_1.telemetry.csv` | Periodic and event samples, when RAID telemetry is enabled |

Host writes, migration writes, NAND page programs, and block erases measure different things. RAID-wide completion values repeated on per-SSD CSV rows must not be summed across those rows.

The first-EOL snapshot and final snapshot can differ because issued work may continue after the EOL boundary. Use the same snapshot definition for lifetime comparisons. Record the commit, trace/version, seed, input XML files, completion status, and termination reason with each result.

## Validation and limitations

- The merged initial-read/GC retry fixes passed six small regressions: first read, starvation, burst writes, spaced writes, overlapping reads/writes, and partial overwrites. See [PR #2](https://github.com/basstwd17-cmyk/OS_RAID_PROJ/pull/2).
- Experiment snapshot `aba1b9d` was built with its Makefile on Windows/LLVM-MinGW. The same six regressions and four policy smoke suites (wear trigger, advanced SWANS, OP exchange, EOL modes) passed. The policy suites are in [PR #3's snapshot](https://github.com/basstwd17-cmyk/OS_RAID_PROJ/tree/5e9e2fe6d42831c23d6720bd3169735b5fbc628b/tests); they are not bundled in the source-only branch. The earlier six-case harness is local to the project workspace.
- These are bounded functional checks, not a large-trace performance study, byte-level payload-integrity proof, or validation of physical SSD lifetime.
- Use a flash backend with `PAGE_LEVEL` mapping and migration concurrency one for the evaluated SWANS path. Experimental OP-zone exchange additionally requires one I/O flow and device caching off. Its reserved logical zone is separate from NAND OP.
- Upstream NVMe/SATA support does not establish that every combination has been validated with these extensions. Large-trace and Linux validation remain separate work.

## Repository layout

```text
src/main.cpp       Command-line entry point
src/exec/          Configuration, host/device assembly, RAID and telemetry
src/policy/        Zone mapping, placement and migration policy
src/host/          Workload generation and trace replay
src/ssd/           FTL, GC, caches, scheduling and block management
src/nvm_chip/      NAND models
src/sim/           Discrete-event simulation engine
src/utils/         Utilities and bundled XML parser
fast18/            Inherited FAST 2018 examples; use their matching configurations
Makefile           Build definition
ssdconfig.xml      Root SSD/RAID configuration
workload.xml       Example scenarios; external trace must be supplied
```

Generated binaries, build products, large traces, logs, and results are excluded from version control. Report bugs through [Issues](https://github.com/basstwd17-cmyk/OS_RAID_PROJ/issues) with the commit, compiler/platform, configuration, and a small reproducible input where possible.

## Upstream, citation and license

This project derives from [CMU-SAFARI/MQSim](https://github.com/CMU-SAFARI/MQSim). Cite the original simulator when using this work and identify this repository and the exact commit for the RAID0/SWANS extensions. No separate publication is claimed here for these extensions.

Arash Tavakkol, Juan Gomez-Luna, Mohammad Sadrosadati, Saugata Ghose, and Onur Mutlu. **MQSim: A Framework for Enabling Realistic Studies of Modern Multi-Queue SSD Devices.** FAST 2018. [Paper](https://people.inf.ethz.ch/omutlu/pub/MQSim-SSD-simulation-framework_fast18.pdf).

```bibtex
@inproceedings{tavakkol2018mqsim,
  title={MQSim: A Framework for Enabling Realistic Studies of Modern Multi-Queue SSD Devices},
  author={Tavakkol, Arash and Gomez-Luna, Juan and Sadrosadati, Mohammad and Ghose, Saugata and Mutlu, Onur},
  booktitle={Proceedings of the 16th USENIX Conference on File and Storage Technologies (FAST)},
  year={2018}
}
```

Distributed under the [MIT license](LICENSE). Preserve upstream copyright and license notices.
