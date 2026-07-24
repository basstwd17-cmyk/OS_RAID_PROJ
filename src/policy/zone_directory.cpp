#include "zone_directory.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace RAID_Policy {

ZoneDirectory::ZoneDirectory()
	: initialized(false),
	  ssd_count(0),
	  stripe_unit_lba(0),
	  zone_size_lba(0),
	  block_unit_lba(0),
	  stripes_per_zone(0),
	  total_logical_lha_count(0),
	  per_ssd_logical_lha_count(0),
	  total_stripe_count(0),
	  zones_per_ssd(0),
	  zone_count(0)
{
}

void ZoneDirectory::Initialize(unsigned int ssd_count,
	unsigned int stripe_unit_lba,
	uint64_t zone_size_lba,
	unsigned int block_unit_lba,
	LHA_type total_logical_lha_count)
{
	if (ssd_count == 0 || stripe_unit_lba == 0 || zone_size_lba == 0 || block_unit_lba == 0) {
		throw std::invalid_argument("ZoneDirectory::Initialize invalid geometry");
	}
	if (zone_size_lba % stripe_unit_lba != 0) {
		throw std::invalid_argument("ZoneDirectory::Initialize zone size must be a multiple of stripe size");
	}
	const uint64_t stripes_per_zone_64 = zone_size_lba / stripe_unit_lba;
	if (stripes_per_zone_64 == 0 || stripes_per_zone_64 > std::numeric_limits<unsigned int>::max()) {
		throw std::invalid_argument("ZoneDirectory::Initialize invalid stripes per zone");
	}

	this->ssd_count = ssd_count;
	this->stripe_unit_lba = stripe_unit_lba;
	this->zone_size_lba = zone_size_lba;
	this->block_unit_lba = block_unit_lba;
	this->stripes_per_zone = static_cast<unsigned int>(stripes_per_zone_64);
	this->total_logical_lha_count = total_logical_lha_count;

	if (total_logical_lha_count == 0) {
		per_ssd_logical_lha_count = 0;
		total_stripe_count = 0;
		zones_per_ssd = 0;
		zone_count = 0;
		zones.clear();
		initialized = true;
		return;
	}

	total_stripe_count = total_logical_lha_count / this->stripe_unit_lba;
	if (total_logical_lha_count % this->stripe_unit_lba != 0) {
		total_stripe_count++;
	}

	per_ssd_logical_lha_count = total_logical_lha_count / ssd_count
		+ (total_logical_lha_count % ssd_count == 0 ? 0 : 1);
	zones_per_ssd = per_ssd_logical_lha_count / this->zone_size_lba;
	if (per_ssd_logical_lha_count % this->zone_size_lba != 0) {
		zones_per_ssd++;
	}
	if (zones_per_ssd > std::numeric_limits<uint64_t>::max() / ssd_count) {
		throw std::overflow_error("ZoneDirectory::Initialize zone count overflow");
	}
	zone_count = zones_per_ssd * ssd_count;

	zones.clear();
	zones.resize(static_cast<size_t>(zone_count));
	for (uint64_t zone_id = 0; zone_id < zone_count; zone_id++) {
		ZoneEntry& entry = zones[static_cast<size_t>(zone_id)];
		entry.Physical_ssd = zone_id % ssd_count;
		entry.Physical_zone = zone_id / ssd_count;
		entry.Number_of_writes = 0;
		entry.Empty = true;
		entry.Migrating = false;
		entry.Written_blocks_by_stream.clear();
	}

	initialized = true;
}

uint64_t ZoneDirectory::Zone_size_lba() const
{
	return zone_size_lba;
}

void ZoneDirectory::Validate_zone_id(uint64_t zone_id) const
{
	if (!initialized) {
		throw std::logic_error("ZoneDirectory is not initialized");
	}
	if (zone_id >= zone_count) {
		throw std::out_of_range("ZoneDirectory zone_id out of range");
	}
}

uint64_t ZoneDirectory::Zone_id_of_lba(LHA_type lba) const
{
	if (stripe_unit_lba == 0 || zone_size_lba == 0 || ssd_count == 0) {
		return 0;
	}
	const uint64_t stripe_index = lba / stripe_unit_lba;
	const unsigned int home_ssd = static_cast<unsigned int>(stripe_index % ssd_count);
	const uint64_t stripe_row = stripe_index / ssd_count;
	const uint64_t local_lba = stripe_row * stripe_unit_lba + lba % stripe_unit_lba;
	const uint64_t local_zone = local_lba / zone_size_lba;
	return local_zone * ssd_count + home_ssd;
}

void ZoneDirectory::Resolve(LHA_type lba, ZoneResolveResult& result) const
{
	if (!initialized || stripe_unit_lba == 0 || zone_size_lba == 0 || ssd_count == 0 || zone_count == 0) {
		result.Disk_id = 0;
		result.Local_lba = lba;
		result.Zone_id = 0;
		result.Zone_lba_offset = 0;
		result.Stripe_offset = 0;
		result.In_stripe_offset = 0;
		return;
	}

	const uint64_t stripe_index = lba / stripe_unit_lba;
	const uint64_t stripe_row = stripe_index / ssd_count;
	const uint64_t in_stripe_offset = lba % stripe_unit_lba;
	const uint64_t base_local_lba = stripe_row * stripe_unit_lba + in_stripe_offset;
	const uint64_t zone_id = Zone_id_of_lba(lba);
	const uint64_t zone_lba_offset = base_local_lba % zone_size_lba;

	Validate_zone_id(zone_id);
	const ZoneEntry& zone = zones[static_cast<size_t>(zone_id)];
	const unsigned int disk_id = static_cast<unsigned int>(zone.Physical_ssd);
	const LHA_type local_lba = zone.Physical_zone * zone_size_lba + zone_lba_offset;

	result.Disk_id = disk_id;
	result.Local_lba = local_lba;
	result.Zone_id = zone_id;
	result.Zone_lba_offset = zone_lba_offset;
	result.Stripe_offset = static_cast<unsigned int>(zone_lba_offset / stripe_unit_lba);
	result.In_stripe_offset = static_cast<unsigned int>(in_stripe_offset);
}

void ZoneDirectory::Resolve_zone_lba(uint64_t zone_id, uint64_t zone_lba_offset,
	unsigned int& disk_id, LHA_type& local_lba) const
{
	Validate_zone_id(zone_id);
	if (zone_lba_offset >= zone_size_lba) {
		throw std::out_of_range("ZoneDirectory zone_lba_offset out of range");
	}
	const ZoneEntry& zone = zones[static_cast<size_t>(zone_id)];
	disk_id = static_cast<unsigned int>(zone.Physical_ssd);
	local_lba = zone.Physical_zone * zone_size_lba + zone_lba_offset;
}

void ZoneDirectory::Resolve_zone_stripe(uint64_t zone_id, unsigned int stripe_offset, unsigned int in_stripe_offset,
	unsigned int& disk_id, LHA_type& local_lba) const
{
	Validate_zone_id(zone_id);
	if (stripe_offset >= stripes_per_zone) {
		throw std::out_of_range("ZoneDirectory stripe_offset out of range");
	}
	if (in_stripe_offset >= stripe_unit_lba) {
		throw std::out_of_range("ZoneDirectory in_stripe_offset out of range");
	}

	const ZoneEntry& zone = zones[static_cast<size_t>(zone_id)];
	const uint64_t zone_lba_offset = static_cast<uint64_t>(stripe_offset) * stripe_unit_lba + in_stripe_offset;
	if (zone_lba_offset >= zone_size_lba) {
		throw std::out_of_range("ZoneDirectory stripe address out of zone range");
	}
	disk_id = static_cast<unsigned int>(zone.Physical_ssd);
	local_lba = zone.Physical_zone * zone_size_lba + zone_lba_offset;
}

unsigned int ZoneDirectory::Owner_ssd(uint64_t zone_id) const
{
	Validate_zone_id(zone_id);
	const ZoneEntry& zone = zones[static_cast<size_t>(zone_id)];
	return static_cast<unsigned int>(zone.Physical_ssd);
}

bool ZoneDirectory::Is_empty(uint64_t zone_id) const
{
	Validate_zone_id(zone_id);
	return zones[static_cast<size_t>(zone_id)].Empty;
}

bool ZoneDirectory::Is_migrating(uint64_t zone_id) const
{
	Validate_zone_id(zone_id);
	return zones[static_cast<size_t>(zone_id)].Migrating;
}

uint64_t ZoneDirectory::Zone_write_count(uint64_t zone_id) const
{
	Validate_zone_id(zone_id);
	return zones[static_cast<size_t>(zone_id)].Number_of_writes;
}

void ZoneDirectory::Observe_write(stream_id_type stream_id, uint64_t zone_id, uint64_t zone_lba_offset, unsigned int write_sectors)
{
	Validate_zone_id(zone_id);
	if (write_sectors == 0) {
		return;
	}
	if (zone_lba_offset >= zone_size_lba) {
		throw std::out_of_range("ZoneDirectory observe zone_lba_offset out of range");
	}
	const uint64_t end_lba_offset = zone_lba_offset + write_sectors - 1;
	if (end_lba_offset >= zone_size_lba) {
		throw std::out_of_range("ZoneDirectory observe write range out of zone bounds");
	}
	ZoneEntry& zone = zones[static_cast<size_t>(zone_id)];
	zone.Number_of_writes++;
	zone.Empty = false;

	const uint64_t start_block = zone_lba_offset / block_unit_lba;
	const uint64_t end_block = end_lba_offset / block_unit_lba;
	const uint64_t block_count = (zone_size_lba + block_unit_lba - 1) / block_unit_lba;
	std::vector<bool>& written_blocks = zone.Written_blocks_by_stream[stream_id];
	if (written_blocks.size() < block_count) {
		written_blocks.resize(static_cast<size_t>(block_count), false);
	}
	for (uint64_t block = start_block; block <= end_block; block++) {
		written_blocks[static_cast<size_t>(block)] = true;
	}
}

void ZoneDirectory::Observe_write(uint64_t zone_id, uint64_t zone_lba_offset, unsigned int write_sectors)
{
	Observe_write(0, zone_id, zone_lba_offset, write_sectors);
}

std::map<stream_id_type, std::vector<unsigned int>> ZoneDirectory::Written_block_offsets_by_stream(uint64_t zone_id) const
{
	Validate_zone_id(zone_id);
	std::map<stream_id_type, std::vector<unsigned int>> out;
	const ZoneEntry& zone = zones[static_cast<size_t>(zone_id)];
	for (std::map<stream_id_type, std::vector<bool>>::const_iterator stream_it = zone.Written_blocks_by_stream.begin();
		stream_it != zone.Written_blocks_by_stream.end(); ++stream_it) {
		std::vector<unsigned int>& blocks = out[stream_it->first];
		for (unsigned int block = 0; block < stream_it->second.size(); block++) {
			if (stream_it->second[static_cast<size_t>(block)]) {
				blocks.push_back(block);
			}
		}
		if (blocks.empty()) {
			out.erase(stream_it->first);
		}
	}
	return out;
}

std::vector<unsigned int> ZoneDirectory::Written_block_offsets(uint64_t zone_id) const
{
	Validate_zone_id(zone_id);
	std::vector<unsigned int> out;
	const ZoneEntry& zone = zones[static_cast<size_t>(zone_id)];
	std::vector<bool> merged;
	for (std::map<stream_id_type, std::vector<bool>>::const_iterator stream_it = zone.Written_blocks_by_stream.begin();
		stream_it != zone.Written_blocks_by_stream.end(); ++stream_it) {
		if (merged.size() < stream_it->second.size()) {
			merged.resize(stream_it->second.size(), false);
		}
		for (size_t block = 0; block < stream_it->second.size(); block++) {
			merged[block] = merged[block] || stream_it->second[block];
		}
	}
	for (unsigned int block = 0; block < merged.size(); block++) {
		if (merged[static_cast<size_t>(block)]) {
			out.push_back(block);
		}
	}
	return out;
}

void ZoneDirectory::Merge_written_blocks(stream_id_type stream_id, uint64_t zone_id, const std::vector<bool>& written_blocks)
{
	Validate_zone_id(zone_id);
	ZoneEntry& zone = zones[static_cast<size_t>(zone_id)];
	const uint64_t block_count = (zone_size_lba + block_unit_lba - 1) / block_unit_lba;
	std::vector<bool>& target_blocks = zone.Written_blocks_by_stream[stream_id];
	if (target_blocks.size() < block_count) {
		target_blocks.resize(static_cast<size_t>(block_count), false);
	}
	const size_t count = std::min(target_blocks.size(), written_blocks.size());
	for (size_t i = 0; i < count; i++) {
		if (written_blocks[i]) {
			target_blocks[i] = true;
			zone.Empty = false;
		}
	}
}

void ZoneDirectory::Merge_written_blocks(uint64_t zone_id, const std::vector<bool>& written_blocks)
{
	Merge_written_blocks(0, zone_id, written_blocks);
}

bool ZoneDirectory::Is_reserved(uint64_t zone_id, const std::vector<uint64_t>& reserved) const
{
	return std::find(reserved.begin(), reserved.end(), zone_id) != reserved.end();
}

bool ZoneDirectory::Is_full_physical_zone(const ZoneEntry& zone) const
{
	if (zone_size_lba == 0 || per_ssd_logical_lha_count < zone_size_lba) {
		return false;
	}
	return zone.Physical_zone <= (per_ssd_logical_lha_count - zone_size_lba) / zone_size_lba;
}

uint64_t ZoneDirectory::Find_empty_zone_on_ssd(unsigned int ssd_id, const std::vector<uint64_t>& reserved) const
{
	if (!initialized) {
		return INVALID_ZONE_ID;
	}
	for (uint64_t zone_id = 0; zone_id < zone_count; zone_id++) {
		const ZoneEntry& zone = zones[static_cast<size_t>(zone_id)];
		if (zone.Empty && !zone.Migrating && Is_full_physical_zone(zone)
			&& !Is_reserved(zone_id, reserved) && Owner_ssd(zone_id) == ssd_id) {
			return zone_id;
		}
	}
	return INVALID_ZONE_ID;
}

uint64_t ZoneDirectory::Find_hottest_used_zone_on_ssd(unsigned int ssd_id, const std::vector<uint64_t>& reserved) const
{
	if (!initialized) {
		return INVALID_ZONE_ID;
	}
	uint64_t selected = INVALID_ZONE_ID;
	uint64_t best = 0;
	for (uint64_t zone_id = 0; zone_id < zone_count; zone_id++) {
		const ZoneEntry& zone = zones[static_cast<size_t>(zone_id)];
		if (zone.Empty || zone.Migrating || Is_reserved(zone_id, reserved) || Owner_ssd(zone_id) != ssd_id) {
			continue;
		}
		if (selected == INVALID_ZONE_ID || zone.Number_of_writes > best) {
			selected = zone_id;
			best = zone.Number_of_writes;
		}
	}
	return selected;
}

void ZoneDirectory::Mark_migrating(uint64_t zone_id, bool migrating)
{
	Validate_zone_id(zone_id);
	zones[static_cast<size_t>(zone_id)].Migrating = migrating;
}

void ZoneDirectory::Swap_placement(uint64_t logical_zone_a, uint64_t logical_zone_b)
{
	Validate_zone_id(logical_zone_a);
	Validate_zone_id(logical_zone_b);
	std::swap(zones[static_cast<size_t>(logical_zone_a)].Physical_ssd, zones[static_cast<size_t>(logical_zone_b)].Physical_ssd);
	std::swap(zones[static_cast<size_t>(logical_zone_a)].Physical_zone, zones[static_cast<size_t>(logical_zone_b)].Physical_zone);
}

void ZoneDirectory::Complete_migration(uint64_t hot_zone, uint64_t cold_zone,
	const std::map<stream_id_type, std::vector<bool>>& dirty_blocks_by_stream)
{
	Validate_zone_id(hot_zone);
	Validate_zone_id(cold_zone);
	for (std::map<stream_id_type, std::vector<bool>>::const_iterator dirty_it = dirty_blocks_by_stream.begin();
		dirty_it != dirty_blocks_by_stream.end(); ++dirty_it) {
		Merge_written_blocks(dirty_it->first, hot_zone, dirty_it->second);
	}
	Reset_logical_zone(cold_zone);
	Mark_migrating(hot_zone, false);
}

void ZoneDirectory::Reset_logical_zone(uint64_t logical_zone)
{
	Validate_zone_id(logical_zone);
	ZoneEntry& zone = zones[static_cast<size_t>(logical_zone)];
	zone.Number_of_writes = 0;
	zone.Empty = true;
	zone.Migrating = false;
	zone.Written_blocks_by_stream.clear();
}

} // namespace RAID_Policy
