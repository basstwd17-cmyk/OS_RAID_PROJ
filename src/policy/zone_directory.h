#ifndef RAID_SWANS_ZONE_DIRECTORY_H
#define RAID_SWANS_ZONE_DIRECTORY_H

#include <map>
#include <vector>
#include "swans_policy_types.h"

namespace RAID_Policy {

struct ZoneResolveResult
{
	unsigned int Disk_id = 0;
	LHA_type Local_lba = 0;
	uint64_t Zone_id = 0;
	uint64_t Zone_lba_offset = 0;
	unsigned int Stripe_offset = 0;
	unsigned int In_stripe_offset = 0;
};

class ZoneDirectory
{
public:
	ZoneDirectory();

	void Initialize(unsigned int ssd_count,
		unsigned int stripe_unit_lba,
		uint64_t zone_size_lba,
		unsigned int block_unit_lba,
		LHA_type total_logical_lha_count);

	bool Is_initialized() const { return initialized; }
	uint64_t Zone_count() const { return zone_count; }
	unsigned int Stripe_unit_lba() const { return stripe_unit_lba; }
	unsigned int Block_unit_lba() const { return block_unit_lba; }
	unsigned int Stripes_per_zone() const { return stripes_per_zone; }
	uint64_t Zone_size_lba() const;

	uint64_t Zone_id_of_lba(LHA_type lba) const;
	void Resolve(LHA_type lba, ZoneResolveResult& result) const;
	void Resolve_zone_lba(uint64_t zone_id, uint64_t zone_lba_offset,
		unsigned int& disk_id, LHA_type& local_lba) const;
	void Resolve_zone_stripe(uint64_t zone_id, unsigned int stripe_offset, unsigned int in_stripe_offset,
		unsigned int& disk_id, LHA_type& local_lba) const;

	unsigned int Owner_ssd(uint64_t zone_id) const;
	bool Is_empty(uint64_t zone_id) const;
	bool Is_migrating(uint64_t zone_id) const;
	uint64_t Zone_write_count(uint64_t zone_id) const;
	uint64_t Duplicate_physical_location_count() const;
	uint64_t Migrating_zone_count() const;

	void Observe_write(stream_id_type stream_id, uint64_t zone_id, uint64_t zone_lba_offset, unsigned int write_sectors);
	void Observe_write(uint64_t zone_id, uint64_t zone_lba_offset, unsigned int write_sectors);
	std::map<stream_id_type, std::vector<unsigned int>> Written_block_offsets_by_stream(uint64_t zone_id) const;
	std::vector<unsigned int> Written_block_offsets(uint64_t zone_id) const;
	void Merge_written_blocks(stream_id_type stream_id, uint64_t zone_id, const std::vector<bool>& written_blocks);
	void Merge_written_blocks(uint64_t zone_id, const std::vector<bool>& written_blocks);

	uint64_t Find_empty_zone_on_ssd(unsigned int ssd_id, const std::vector<uint64_t>& reserved) const;
	uint64_t Find_hottest_used_zone_on_ssd(unsigned int ssd_id, const std::vector<uint64_t>& reserved) const;

	void Mark_migrating(uint64_t zone_id, bool migrating);
	void Swap_placement(uint64_t logical_zone_a, uint64_t logical_zone_b);
	void Complete_migration(uint64_t hot_zone, uint64_t cold_zone,
		const std::map<stream_id_type, std::vector<bool>>& dirty_blocks_by_stream);
	void Reset_logical_zone(uint64_t logical_zone);

private:
	struct ZoneEntry {
		uint64_t Physical_ssd = 0;     // physical SSD where this logical zone is currently placed
		uint64_t Physical_zone = 0;    // physical zone index inside that SSD
		uint64_t Number_of_writes = 0; // accumulated write subrequests for this logical zone
		bool Empty = true;
		bool Migrating = false;
		std::map<stream_id_type, std::vector<bool>> Written_blocks_by_stream; // BlockWriteMap: stream -> per-block written bitmap within this zone
	};

	bool Is_reserved(uint64_t zone_id, const std::vector<uint64_t>& reserved) const;
	bool Is_full_physical_zone(const ZoneEntry& zone) const;
	void Validate_zone_id(uint64_t zone_id) const;

	bool initialized;
	unsigned int ssd_count;
	unsigned int stripe_unit_lba;
	uint64_t zone_size_lba;
	unsigned int block_unit_lba;
	unsigned int stripes_per_zone;
	LHA_type total_logical_lha_count;
	uint64_t per_ssd_logical_lha_count;
	uint64_t total_stripe_count;
	uint64_t zones_per_ssd;
	uint64_t zone_count;
	std::vector<ZoneEntry> zones;
};

} // namespace RAID_Policy

#endif // RAID_SWANS_ZONE_DIRECTORY_H
