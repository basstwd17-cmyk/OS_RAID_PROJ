# Migration I/O policy

Based on OS_RAID_PROJ `9057bbd6b8ac52de88a85c74367eef860fe79fa7`.

- Reads touching a migrating hot/cold zone pause every migration task they touch. Already submitted copy I/O finishes; the NAND command itself is not suspended. The controller then submits the read using the existing mapping and resumes each task after all subrequests of every read holding that task finish. Unrelated tasks and host I/O can continue.
- Source mappings and source blocks remain intact through grabbing and restoring. Restore writes use the destination addresses captured in the copy plan. After all restore writes finish and no priority read remains, placement is swapped, the source is discarded, and cached writes are released.
- Writes touching migrating zones stay in the RAID controller's request cache. They do not modify the migration buffer or dirty bitmap. They are submitted through the normal SSD path after migration, using the final mapping, and acknowledged on backend completion. A request spanning multiple migrations waits until all those zones are stable.
- `SWANS_Migration_Working_Queue_Limit` still limits cached requests per task. Excess writes enter the existing backpressure queue. Reads use a separate priority queue so a full write cache cannot prevent a read from pausing migration.
- This cache follows MQSim's request-level simulation: request objects and logical byte accounting, without a new DRAM capacity/latency or byte-payload model. Reads do not forward unacknowledged cached writes; they use the committed source. Sustained reads can postpone migration until its read queue drains.
- Configuration remains `SWANS_Buffered_Write_Completion_Mode=DEFERRED`. Result XML identifies `SWANS_Write_During_Migration_Mode=CACHE_AND_REPLAY` and `SWANS_Read_During_Migration_Mode=PAUSE_AT_IO_BOUNDARY`. `SWANS_Buffered_Write_Completions` now counts actual backend completion of cached writes. The legacy dirty-block metric stays zero.

Validation: `scripts/build-and-test.ps1` on MSVC, or `make test` with GCC. Tests cover independent/multiple tasks, cache saturation, source lifetime, abort cleanup, and real controller/engine events with delayed backend callbacks before copying, during grabbing/restoring, and after the final restore before commit.

After building the simulator, `python tests/nand_smoke.py` exercises the real NAND path with 15 requests: one completed migration, three priority reads during migration, two cached writes and one backpressure write. It checks all requests complete and all migration queues drain; generated inputs/results stay under `build/nand-smoke/`.
