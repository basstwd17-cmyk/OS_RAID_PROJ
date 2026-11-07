# Zone selection

Selection used by both empty-destination migration and reserve exchange:

- `SWANS_Zone_Popularity_Mode=HALF_DECAY` (default): each host write immediately
  adds its sector count to P. After each successfully completed migration task,
  every zone's P is multiplied by 0.5. Time passing alone never changes P.
  A two-stage reserve exchange counts once, after both stages and source discard
  complete. Pauses, failed discard retries and EOL aborts do not halve P.
  Concurrent tasks each halve P when they finish; two completions apply 0.5 twice.
  Deferred host writes replayed after completion count at full weight when observed.
  Lifetime write counters and cumulative zone selection are not halved.
  The RAID controller retains this state in its ZoneDirectory. For successive
  per-zone write amounts A, B, C, D separated by three completed migrations,
  the current score is `A/8 + B/4 + C/2 + D`. It is never replaced with only
  the latest interval; D accumulates on top of the retained older history.
- `LAST_EPOCH`: `P_new = host_write_sectors_in_window`.
- `SWANS_Zone_Popularity_Period` applies only to LAST_EPOCH, in nanoseconds;
  zero uses `SWANS_Epoch_Default`. HALF_DECAY ignores legacy period settings,
  including 40 seconds. Runtime statistics report its effective period as 0 and
  update rule as `LIVE_WRITES_HALF_ALL_ZONES_PER_COMPLETED_MIGRATION`.
- HALF_DECAY selection uses live accumulated heat. LAST_EPOCH selection uses the
  latest completed time window, including empty idle windows.
- Host overwrites count again for popularity; migration copies do not.
- Source: maximum popularity on the oldest SSD. Equal-popularity ties prefer
  fewer copy sectors, then lowest logical zone ID. A cold-zone selector uses the
  minimum popularity, with the same cost tie break.
- Logical valid ratio counts distinct observed sectors divided by zone size.
  This is logical data coverage, not physical NAND valid-page ratio. Read-initialized
  ranges are recorded without contributing to write popularity. With reserve mode,
  supported preconditioning data is registered from the actual FTL mappings before
  host I/O; mappings outside the exported capacity are discarded. Legacy mode
  still tracks host-observed data only.
- Copy cost counts touched migration units per stream. Reserve mode uses flash
  pages, so reading a copy never initializes a wholly unwritten page. Legacy mode
  retains its erase-block copy units. Partially valid pages are copied as full pages;
  the sector-level ratio is not an estimate of physical NAND valid-page ratio.

This implements the requested halving/last-window alternatives, not a verified
reproduction of a particular EDM paper. No weighted cost-versus-popularity score
is assumed: hottest/coldest ordering remains primary.

The youngest SSD's empty user zone is preferred. If none exists, select its coldest
used zone and the oldest SSD's hidden spare. Only one exchange can use that spare
at a time. See [reserve-exchange.md](reserve-exchange.md) for the two phases and
pause semantics. Selection is popularity-first; copy cost only breaks popularity
ties. A weighted popularity/cost objective has not been assumed.
