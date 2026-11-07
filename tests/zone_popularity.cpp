#include <cassert>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include "../src/policy/zone_directory.h"

using namespace RAID_Policy;

int main()
{
	// Controller-owned per-zone history: A/8 + B/4 + C/2 + D.
	ZoneDirectory history;
	history.Initialize(2, 8, 32, 8, 256);
	history.Configure_popularity("HALF_DECAY", 0);
	history.Observe_write(0, 0, 8, true); // A
	history.On_migration_completed();
	history.Observe_write(0, 0, 12, true); // B
	history.On_migration_completed();
	history.Observe_write(0, 0, 16, true); // C
	history.On_migration_completed();
	history.Observe_write(0, 0, 20, true); // D
	assert(history.Zone_popularity(0) == 8.0/8 + 12.0/4 + 16.0/2 + 20);
	history.Advance_popularity(400000000000ULL);
	assert(history.Zone_popularity(0) == 32);
	assert(history.Zone_written_sectors(0) == 56);
	assert(history.Zone_write_count(0) == 4);
	ZoneDirectory d;
	d.Initialize(2, 8, 32, 8, 256);
	d.Configure_popularity("HALF_DECAY", 100);
	d.Observe_write(0, 0, 8, true);
	d.Observe_write(0, 0, 8, true); // overwrites count for heat, not valid footprint
	d.Observe_write(2, 0, 24, true);
	assert(d.Zone_valid_sectors(0) == 8);
	assert(d.Zone_valid_ratio(0) == .25);
	assert(d.Zone_copy_sectors(0) == 8);
	d.Advance_popularity(100);
	assert(d.Zone_popularity(0) == 16 && d.Zone_popularity(2) == 24);
	assert(d.Find_hottest_used_zone_on_ssd(0, {}) == 2);
	d.Observe_write(0, 0, 8, true);
	d.Advance_popularity(200);
	assert(d.Zone_popularity(0) == 24 && d.Zone_popularity(2) == 24);
	d.On_migration_completed();
	assert(d.Zone_popularity(0) == 12 && d.Zone_popularity(2) == 12);
	d.Observe_write(0, 0, 1, true);
	assert(d.Zone_popularity(0) == 13); // new writes count immediately at full weight
	d.Advance_popularity(500);
	assert(d.Zone_popularity(0) == 13 && d.Zone_popularity(2) == 12);
	d.Advance_popularity(500);
	assert(d.Zone_popularity(0) == 13); // elapsed time never ages HALF_DECAY
	d.On_migration_completed();
	assert(d.Zone_popularity(0) == 6.5 && d.Zone_popularity(2) == 6);
	assert(d.Zone_written_sectors(0) == 25 && d.Zone_write_count(0) == 4);
	assert(d.Popularity_period() == 0);
	// Copy cost follows touched blocks: sparse sectors in different blocks cost more.
	d.Configure_popularity("LAST_EPOCH", 100);
	d.Observe_write(4, 0, 1, true);
	d.Observe_write(4, 8, 1, true);
	d.Observe_write(6, 0, 2, true);
	d.Advance_popularity(100);
	assert(d.Zone_popularity(4) == d.Zone_popularity(6));
	d.On_migration_completed();
	assert(d.Zone_popularity(4) == 2); // explicit LAST_EPOCH mode stays time-based
	assert(d.Zone_valid_sectors(4) == d.Zone_valid_sectors(6));
	assert(d.Zone_copy_sectors(4) == 16 && d.Zone_copy_sectors(6) == 8);
	assert(d.Find_hottest_used_zone_on_ssd(0, {0, 2}) == 6);
	assert(d.Find_coldest_used_zone_on_ssd(0, {0, 2}) == 6);
	d.Advance_popularity(300);
	assert(d.Zone_popularity(4) == 0 && d.Zone_popularity(6) == 0);
	// Read-initialized data participates in cost/occupancy without becoming write-hot.
	d.Observe_data(0, 1, 0, 3);
	d.Observe_data(0, 1, 2, 3);
	assert(d.Zone_valid_sectors(1) == 5);
	assert(!d.Is_empty(1));
	d.Advance_popularity(400);
	assert(d.Zone_popularity(1) == 0 && d.Zone_written_sectors(1) == 0);
	d.Swap_placement(4, 5);
	assert(d.Owner_ssd(4) == 1 && d.Zone_copy_sectors(4) == 16);
	d.Reset_logical_zone(4);
	assert(d.Is_empty(4) && d.Zone_valid_sectors(4) == 0 && d.Zone_copy_sectors(4) == 0);
	bool rejected = false;
	try { d.Configure_popularity("UNKNOWN", 100); } catch (const std::invalid_argument&) { rejected = true; }
	assert(rejected);
	std::cout << "zone popularity tests passed\n";
}
