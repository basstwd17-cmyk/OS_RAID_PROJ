# op_zone_migration

`op_zone_migration` is the public name for the existing two-stage reserve-zone
exchange, with an explicit XML option and migration-record mode. It reuses the
existing mapping, copy, discard, pause/retry and deferred-I/O machinery rather
than introducing a second exchange executor.

## Capacity and movement

Enable `<op_zone_migration>true</op_zone_migration>` in the separate SWANS XML
(`-s`) or the SSD Device_Parameter_Set (`-i`). The older
`SWANS_Reserve_Zone_Enabled` option remains an alias; do not put both in the
same SWANS overlay (duplicates are rejected). The default without either is false.

For N complete backend logical zones per SSD, export N-1 user zones and hide
one empty OP zone. Initially the last full zone is hidden. Partial trailing
capacity is not exported. This is in addition to NAND overprovisioning: it
does not change the TEC user-block boundary or the NAND OP ratio.

Once the configured migration trigger fires:

1. Select source/destination using the existing policy. With SSD_POPULARITY,
   select the hottest eligible zone on the selected source SSD.
2. If the destination has an available ordinary empty zone, use normal migration.
   The hidden OP zone is never treated as an ordinary empty destination.
3. Otherwise select the coldest eligible used zone on the destination and an
   available OP zone on the source.
4. Copy destination cold data to the source OP zone and release its old location.
5. Copy source hot data to the released cold location and invalidate the old hot
   data. The old hot location becomes the source's new empty OP zone. The
   destination's OP zone is unchanged.

OP is a rotating role, not a permanently fixed physical address. During exchange
the source spare is locked/temporarily occupied; after successful completion
each SSD again has one empty OP zone. Busy/ineligible zones cannot be selected;
if the required zones cannot be obtained, the exchange is not started. Reads and
writes to locked zones are deferred. Invalidation must complete before reuse.
Popularity halves once after the entire two-stage task, not after each copy.

## Policy settings

TEC dynamic wear:

```xml
<op_zone_migration>true</op_zone_migration>
<SWANS_Wear_Metric>TEC</SWANS_Wear_Metric>
<SWANS_Migration_Trigger>DYNAMIC_WEAR</SWANS_Migration_Trigger>
<SWANS_Zone_Selection>SSD_POPULARITY</SWANS_Zone_Selection>
<SWANS_TEC_TH_Precautionary_PctPoint>5</SWANS_TEC_TH_Precautionary_PctPoint>
<SWANS_TEC_TH_Critical_PctPoint>10</SWANS_TEC_TH_Critical_PctPoint>
```

5/10 are example thresholds, not paper recommendations. Existing dynamic wear
trigger remains `(average wear + 1)/2 < source wear`.

For cumulative host **plus completed migration** writes, use
`SWANS_Wear_Metric=WRITE_BYTES` and `SWANS_Migration_Trigger=IMBALANCE`, with
the existing `SWANS_TH_Precautionary`, `SWANS_TH_Critical` and
`SWANS_Balance_Unit_Bytes`. Merely switching to WRITE_BYTES while retaining
DYNAMIC_WEAR does not make the dynamic wear snapshot a write-byte metric.
Other existing selection modes retain their semantics; SSD_POPULARITY matches
the most-worn-SSD / hottest-zone behavior described here.

This option does not change the chosen EOL criterion. The existing restrictions
remain: one I/O flow, flash backend, page-level mapping with discard, device
data cache off, at least two full zones per SSD, and page/stripe-aligned zones.

## Output and checks

Migration records use `Mode=op_zone_migration` for the exchange and retain
`Mode=EMPTY_DESTINATION` for ordinary migration. Existing Reserve_zone and
cold-stage/copy counters remain available. Consumers expecting RESERVE_EXCHANGE
must accept the new mode name.

`tests/reserve_exchange.cpp` checks 100->99 capacity, hidden address rejection,
ordinary-empty priority, opposing TEC/write rankings (including migration write
accounting), and the existing exchange data/mapping/rotating-spare/pause/abort
cases. `tests/reserve_exchange_smoke.py` exercises the named option on actual
NAND paths, the legacy alias, SSD XML placement, and invalid/duplicate options.
