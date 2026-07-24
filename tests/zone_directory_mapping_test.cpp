#include <cassert>
#include <stdexcept>
#include <vector>

#include "../src/policy/zone_directory.h"

namespace {
	const unsigned int SSD_COUNT = 4;
	const unsigned int STRIPE_LBA = 512;
	const uint64_t ZONE_LBA = 16384;
	const unsigned int BLOCK_LBA = 4096;

	void assert_resolve(const RAID_Policy::ZoneDirectory& directory,
		LHA_type global_lba,
		unsigned int expected_disk,
		LHA_type expected_local_lba,
		uint64_t expected_zone,
		uint64_t expected_zone_offset)
	{
		RAID_Policy::ZoneResolveResult result;
		directory.Resolve(global_lba, result);
		assert(result.Disk_id == expected_disk);
		assert(result.Local_lba == expected_local_lba);
		assert(result.Zone_id == expected_zone);
		assert(result.Zone_lba_offset == expected_zone_offset);
	}
}

int main()
{
	RAID_Policy::ZoneDirectory directory;
	directory.Initialize(SSD_COUNT, STRIPE_LBA, ZONE_LBA, BLOCK_LBA, 4ULL * 2ULL * ZONE_LBA);

	assert(directory.Stripe_unit_lba() == STRIPE_LBA);
	assert(directory.Stripes_per_zone() == ZONE_LBA / STRIPE_LBA);
	assert(directory.Zone_count() == 8);
	directory.Observe_write(0, 0, 0, 8);
	assert(directory.Zone_write_count(0) == 1);
	directory.Observe_write(0, 0, 8, 16);
	assert(directory.Zone_write_count(0) == 2);

	assert_resolve(directory, 0, 0, 0, 0, 0);
	assert_resolve(directory, 512, 1, 0, 1, 0);
	assert_resolve(directory, 2048, 0, 512, 0, 512);
	assert_resolve(directory, 65536, 0, 16384, 4, 0);
	for (LHA_type global_lba = 0; global_lba < 4ULL * 2ULL * ZONE_LBA; global_lba += 137) {
		const LHA_type stripe = global_lba / STRIPE_LBA;
		const unsigned int expected_disk = static_cast<unsigned int>(stripe % SSD_COUNT);
		const LHA_type expected_local_lba = (stripe / SSD_COUNT) * STRIPE_LBA
			+ global_lba % STRIPE_LBA;
		RAID_Policy::ZoneResolveResult result;
		directory.Resolve(global_lba, result);
		assert(result.Disk_id == expected_disk);
		assert(result.Local_lba == expected_local_lba);
	}

	RAID_Policy::ZoneResolveResult before_boundary;
	directory.Resolve(500, before_boundary);
	assert(before_boundary.Disk_id == 0);
	assert(before_boundary.In_stripe_offset == 500);
	RAID_Policy::ZoneResolveResult after_boundary;
	directory.Resolve(512, after_boundary);
	assert(after_boundary.Disk_id == 1);
	assert(after_boundary.In_stripe_offset == 0);

	unsigned int stripe_disk = 0;
	LHA_type stripe_local_lba = 0;
	directory.Resolve_zone_stripe(4, 3, 5, stripe_disk, stripe_local_lba);
	assert(stripe_disk == 0);
	assert(stripe_local_lba == 16384 + 3 * 512 + 5);

	directory.Swap_placement(0, 1);
	assert_resolve(directory, 0, 1, 0, 0, 0);
	assert_resolve(directory, 512, 0, 0, 1, 0);

	RAID_Policy::ZoneDirectory partial_directory;
	partial_directory.Initialize(SSD_COUNT, STRIPE_LBA, ZONE_LBA, BLOCK_LBA,
		4ULL * (ZONE_LBA + 100));
	std::vector<uint64_t> reserved;
	reserved.push_back(0);
	assert(partial_directory.Find_empty_zone_on_ssd(0, reserved) == RAID_Policy::INVALID_ZONE_ID);

	bool rejected_bad_geometry = false;
	try {
		RAID_Policy::ZoneDirectory invalid_directory;
		invalid_directory.Initialize(SSD_COUNT, STRIPE_LBA, ZONE_LBA + 1, BLOCK_LBA, 4ULL * ZONE_LBA);
	} catch (const std::invalid_argument&) {
		rejected_bad_geometry = true;
	}
	assert(rejected_bad_geometry);

	return 0;
}
