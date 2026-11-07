# Migration wear trigger

Migration uses the strict comparison `(Wear_avg(t) + Wear_max) / 2 < Wear_i(t)`.
`Wear_max = 1` is the fixed end-of-life limit, not the maximum observed wear.
`Wear_avg` is the unweighted mean of the lifetime-consumed fractions of all SSDs.
At each existing policy epoch, select the most worn SSD as source and the least
worn SSD as destination. Equality does not trigger migration. Existing zone
selection, concurrent migration limits, and immediate EOL stop remain in force.

## Current metric

`<SWANS_Wear_Metric>BAD_BLOCK_COUNT</SWANS_Wear_Metric>` is the default, including
when the setting is absent. For each SSD:

```
N = physical block count
Bop = floor(N * Overprovisioning_Ratio)  # block manager's actual initial OP budget
Blimit = Bop - N * End_of_Life_Threshold
Wear_i = min(1, bad_block_count / Blimit)
```

This normalizes the existing remaining-OP EOL condition; it does not change SSD
retirement or EOL detection. Different capacities and OP budgets are normalized
separately. A disabled retirement policy, missing SSD observation, nonpositive
budget, or invalid fraction disables wear-triggered migration for that snapshot.
Wear remains zero until blocks retire; this metric cannot anticipate retirement.

Example: 1,000 blocks, OP 20%, EOL remaining OP 5% gives a budget of 150 bad blocks.
Bad counts `[120, 30, 30, 30]` give wear `[0.8, 0.2, 0.2, 0.2]`, mean `0.35`,
and threshold `0.675`; SSD 0 qualifies.

The trigger is independent of cumulative write-byte standard deviation.
`SWANS_TH_Critical` is accepted for compatibility but no longer triggers migration.
If wear does not qualify, or no migration zone pair is available, the existing
write-byte-based redirect threshold and live write-hot/write-cold selection apply.
Mu and logical write counters retain their original meanings.

## Future metric selection

`RAID_Controller::Set_wear_metric_provider(name, callback)` supplies a
`RAID_Policy::WearSnapshot` of per-SSD fractions in `[0, 1]`. Register an alternative
provider after `Set_backend_ssds`, and extend `SWANS_Wear_Metric` validation and
selection in `RAID_Device` when its definition is established. The migration
formula and policy need no changes. `TAIL_ERASE_COUNT` is deliberately rejected
until implemented; it never silently falls back to another metric.

Policy XML reports the metric, fixed maximum, last evaluation's validity, mean,
threshold, and per-SSD values. These are evaluation-time values, not final EOL
observations. Migration history records the wear values used at each start.

## Verification

Run `make test` for boundary, aging, metric normalization, migration source,
redirect, unavailable metric, and full simulator smoke checks.
