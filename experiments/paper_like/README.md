# SWANS paper-like experiment

This directory keeps the paper-like experiment inputs separate from the
working `RAID/ssdconfig.xml` and `RAID/workload_trace.xml` files.

## Configuration represented here

- Four SSDs, 64 GiB raw capacity per SSD (256 GiB raw array capacity)
- 10% MQSim overprovisioning (about 57.6 GiB host-visible per SSD)
- 256 KiB RAID stripe (`512` sectors)
- 16 MiB SWANS zone (`32768` sectors)
- 40 s policy epochs and SWANS thresholds `5` / `15`
- One migration thread and a 64-entry migration working queue
- 4 KiB page, 512 KiB block, 4096 blocks/plane, 4 planes/die,
  4 dies/package, and 2 packages/SSD
- Flash read/program/erase latency of 25 us / 200 us / 1.5 ms

MQSim models packages as `channels * chips per channel`.  The paper gives two
packages per SSD but does not state their channel layout, so this experiment
uses two channels with one chip on each channel.

The paper uses a FAST hybrid/log-block FTL.  MQSim's hybrid mapping unit in
this repository is incomplete, so both RAID0 and SWANS use the same
`PAGE_LEVEL` FTL.  Results should therefore be interpreted as a controlled
RAID0-versus-SWANS comparison in MQSim, not as an exact reproduction of the
paper's absolute FAST-FTL results.

The paper does not specify MQSim's overprovisioning, SSD-side cache, channel
transfer rate, host queue depth, GC policy, flash technology, or PE-cycle
limit.  These fields retain the repository's existing values and are kept
identical between the two configurations.

Because the per-plane logical sector calculation truncates after applying
10% OP, MQSim exposes 120,795,936 sectors per SSD.  The largest symmetric
stripe-aligned range is 120,795,648 sectors per SSD, or 483,182,592 sectors
for the four-SSD array.  Trace normalization should use the stripe-aligned
values.

## Build and policy tests

The repository Makefile tracks C++ header dependencies and provides the policy
unit tests:

```sh
make -j4
make test
```

## Trace files

Raw traces are intentionally not versioned. Place the required files under the
ignored repository-local paths referenced by each workload:

```text
traces/alibaba_device_aware.trace
traces/alibaba_sample.trace
traces/paper_like/24.hour.BuildServer.11-28-2007.07-55-PM.trace
```

Run a matched configuration/workload pair from the repository root, for
example:

```sh
./MQSim \
  -i experiments/paper_like/ssdconfig_paper_like_swans.xml \
  -w experiments/paper_like/workload_smoke_swans.xml
```

Generated scenario XML files and analysis outputs belong in ignored `results/`
or `runs/` directories, not in source control.
