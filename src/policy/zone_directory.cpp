#include "zone_directory.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
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
	LHA_type total_logical_lha_count, bool reserve_zone, LHA_type backend_lha_count)
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
	popularity_epoch = 0;

	if (total_logical_lha_count == 0) {
		per_ssd_logical_lha_count = 0;
		total_stripe_count = 0;
		zones_per_ssd = 0;
		zone_count = 0;
		user_zone_count = 0;
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
	per_ssd_physical_lha_count = reserve_zone ? backend_lha_count : per_ssd_logical_lha_count;
	zones_per_ssd = per_ssd_logical_lha_count / this->zone_size_lba;
	if (per_ssd_logical_lha_count % this->zone_size_lba != 0) {
		zones_per_ssd++;
	}
	if (zones_per_ssd > std::numeric_limits<uint64_t>::max() / ssd_count) {
		throw std::overflow_error("ZoneDirectory::Initialize zone count overflow");
	}
	zone_count = zones_per_ssd * ssd_count;
	user_zone_count = zone_count;
	if (reserve_zone) {
		if (total_logical_lha_count % ssd_count != 0 || per_ssd_logical_lha_count % zone_size_lba != 0
			|| per_ssd_physical_lha_count / zone_size_lba <= zones_per_ssd)
			throw std::invalid_argument("Reserve requires full exported zones and one extra backend zone per SSD");
		zone_count += ssd_count;
	}

	zones.clear();
	zones.resize(static_cast<size_t>(zone_count));
	for (uint64_t zone_id = 0; zone_id < zone_count; zone_id++) {
		ZoneEntry& entry = zones[static_cast<size_t>(zone_id)];
		entry.Physical_ssd = zone_id % ssd_count;
		entry.Physical_zone = zone_id / ssd_count;
		entry.Number_of_writes = 0;
		entry.Empty = true;
		entry.Migrating = false;
		entry.Reserve = zone_id >= user_zone_count;
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
	if (initialized && lba >= total_logical_lha_count)
		throw std::out_of_range("Host LBA is outside exported capacity (reserved zones are hidden)");
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

uint64_t ZoneDirectory::Total_zone_write_count() const
{
	uint64_t total = 0;
	for (const ZoneEntry& zone : zones) {
		total += zone.Number_of_writes;
	}
	return total;
}

uint64_t ZoneDirectory::Zone_written_sectors(uint64_t zone_id) const
{
	Validate_zone_id(zone_id);
	return zones[static_cast<size_t>(zone_id)].Written_sectors;
}

uint64_t ZoneDirectory::Duplicate_physical_location_count() const
{
	if (!initialized) {
		return 0;
	}
	std::set<std::pair<uint64_t, uint64_t>> locations;
	uint64_t duplicates = 0;
	for (const ZoneEntry& zone : zones) {
		if (!locations.insert(std::make_pair(zone.Physical_ssd, zone.Physical_zone)).second) {
			duplicates++;
		}
	}
	return duplicates;
}

uint64_t ZoneDirectory::Migrating_zone_count() const
{
	uint64_t count = 0;
	for (const ZoneEntry& zone : zones) {
		if (zone.Migrating) {
			count++;
		}
	}
	return count;
}

void ZoneDirectory::Observe_write(stream_id_type stream_id, uint64_t zone_id, uint64_t zone_lba_offset,
	unsigned int write_sectors, bool count_host_request_zone_touch)
{
	Observe_data(stream_id, zone_id, zone_lba_offset, write_sectors);
	if (write_sectors == 0) return;
	ZoneEntry& zone = zones[static_cast<size_t>(zone_id)];
	if (count_host_request_zone_touch) zone.Number_of_writes++;
	zone.Written_sectors += write_sectors;
	if (popularity_mode == "HALF_DECAY") zone.Popularity += write_sectors;
	else zone.Epoch_write_sectors += write_sectors;
}

void ZoneDirectory::Observe_data(stream_id_type stream_id, uint64_t zone_id, uint64_t zone_lba_offset,
	unsigned int write_sectors)
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
	zone.Empty = false;
	auto& valid = zone.Valid_sectors_by_stream[stream_id];
	valid.resize(static_cast<size_t>(zone_size_lba), false);
	for (uint64_t sector = zone_lba_offset; sector <= end_lba_offset; ++sector) valid[sector] = true;

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

void ZoneDirectory::Observe_write(uint64_t zone_id, uint64_t zone_lba_offset, unsigned int write_sectors,
	bool count_host_request_zone_touch)
{
	Observe_write(0, zone_id, zone_lba_offset, write_sectors, count_host_request_zone_touch);
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

void ZoneDirectory::Configure_popularity(const std::string& mode, sim_time_type period)
{
	if ((mode != "HALF_DECAY" && mode != "LAST_EPOCH") || (mode == "LAST_EPOCH" && period == 0))
		throw std::invalid_argument("Popularity requires HALF_DECAY or LAST_EPOCH; LAST_EPOCH requires a positive period");
	popularity_mode = mode;
	popularity_period = period;
	popularity_epoch = 0;
	for (auto& zone : zones) { zone.Popularity = 0; zone.Epoch_write_sectors = 0; }
}

void ZoneDirectory::Advance_popularity(sim_time_type now)
{
	if (popularity_mode == "HALF_DECAY") return;
	const uint64_t epoch = now / popularity_period;
	if (epoch <= popularity_epoch) return;
	const uint64_t elapsed = epoch - popularity_epoch;
	for (auto& zone : zones) {
		zone.Popularity = elapsed == 1 ? static_cast<double>(zone.Epoch_write_sectors) : 0.0;
		zone.Epoch_write_sectors = 0;
	}
	popularity_epoch = epoch;
}

void ZoneDirectory::On_migration_completed()
{
	if (popularity_mode != "HALF_DECAY") return;
	for (auto& zone : zones) zone.Popularity *= 0.5;
}

double ZoneDirectory::Zone_popularity(uint64_t zone_id) const
{
	Validate_zone_id(zone_id);
	return zones[zone_id].Popularity;
}

uint64_t ZoneDirectory::Zone_valid_sectors(uint64_t zone_id) const
{
	Validate_zone_id(zone_id);
	std::vector<bool> valid(static_cast<size_t>(zone_size_lba), false);
	for (const auto& stream : zones[zone_id].Valid_sectors_by_stream)
		for (size_t i = 0; i < stream.second.size(); ++i) valid[i] = valid[i] || stream.second[i];
	return static_cast<uint64_t>(std::count(valid.begin(), valid.end(), true));
}

double ZoneDirectory::Zone_valid_ratio(uint64_t zone_id) const
{
	return static_cast<double>(Zone_valid_sectors(zone_id)) / zone_size_lba;
}

uint64_t ZoneDirectory::Zone_copy_sectors(uint64_t zone_id) const
{
	Validate_zone_id(zone_id);
	uint64_t sectors = 0;
	for (const auto& stream : zones[zone_id].Written_blocks_by_stream)
		for (size_t block = 0; block < stream.second.size(); ++block)
			if (stream.second[block]) sectors += std::min<uint64_t>(block_unit_lba, zone_size_lba - block * block_unit_lba);
	return sectors;
}

bool ZoneDirectory::Is_full_physical_zone(const ZoneEntry& zone) const
{
	if (zone_size_lba == 0 || per_ssd_physical_lha_count < zone_size_lba) {
		return false;
	}
	return zone.Physical_zone <= (per_ssd_physical_lha_count - zone_size_lba) / zone_size_lba;
}

uint64_t ZoneDirectory::Find_empty_zone_on_ssd(unsigned int ssd_id, const std::vector<uint64_t>& reserved) const
{
	if (!initialized) {
		return INVALID_ZONE_ID;
	}
	for (uint64_t zone_id = 0; zone_id < zone_count; zone_id++) {
		const ZoneEntry& zone = zones[static_cast<size_t>(zone_id)];
		if (!zone.Reserve && zone.Empty && !zone.Migrating && Is_full_physical_zone(zone)
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
	double best = 0;
	uint64_t best_cost = 0;
	for (uint64_t zone_id = 0; zone_id < zone_count; zone_id++) {
		const ZoneEntry& zone = zones[static_cast<size_t>(zone_id)];
		if (zone.Reserve || zone.Empty || zone.Migrating || Is_reserved(zone_id, reserved) || Owner_ssd(zone_id) != ssd_id) {
			continue;
		}
		const uint64_t cost = Zone_copy_sectors(zone_id);
		if (selected == INVALID_ZONE_ID || zone.Popularity > best
			|| (zone.Popularity == best && cost < best_cost)) {
			selected = zone_id;
			best = zone.Popularity;
			best_cost = cost;
		}
	}
	return selected;
}

uint64_t ZoneDirectory::Find_coldest_used_zone_on_ssd(unsigned int ssd_id, const std::vector<uint64_t>& reserved) const
{
	uint64_t selected = INVALID_ZONE_ID;
	for (uint64_t z = 0; z < zone_count; ++z) {
		const auto& zone = zones[z];
		if (zone.Reserve || zone.Empty || zone.Migrating || Is_reserved(z, reserved) || zone.Physical_ssd != ssd_id) continue;
		if (selected == INVALID_ZONE_ID || zone.Popularity < zones[selected].Popularity
			|| (zone.Popularity == zones[selected].Popularity && Zone_copy_sectors(z) < Zone_copy_sectors(selected)))
			selected = z;
	}
	return selected;
}

uint64_t ZoneDirectory::Find_migration_zone(unsigned int ssd, bool global, bool cumulative,
	const std::vector<uint64_t>& reserved) const
{
	uint64_t selected = INVALID_ZONE_ID;
	for (uint64_t z = 0; z < zones.size(); ++z) {
		const auto& zone = zones[z];
		if (zone.Reserve || zone.Empty || zone.Migrating || Is_reserved(z, reserved)
			|| (!global && Owner_ssd(z) != ssd)) continue;
		if (selected == INVALID_ZONE_ID || (cumulative
			? zone.Written_sectors > zones[selected].Written_sectors
			: zone.Popularity > zones[selected].Popularity)) selected = z;
	}
	return selected; // equal scores: smallest logical zone ID
}

void ZoneDirectory::Mark_migrating(uint64_t zone_id, bool migrating)
{
	Validate_zone_id(zone_id);
	zones[static_cast<size_t>(zone_id)].Migrating = migrating;
}

bool ZoneDirectory::Is_reserve_zone(uint64_t zone_id) const
{
	Validate_zone_id(zone_id);
	return zones[zone_id].Reserve;
}

uint64_t ZoneDirectory::Find_reserve_zone_on_ssd(unsigned int ssd_id, const std::vector<uint64_t>& excluded) const
{
	for (uint64_t z = user_zone_count; z < zone_count; ++z)
		if (zones[z].Reserve && zones[z].Empty && !zones[z].Migrating && zones[z].Physical_ssd == ssd_id
			&& !Is_reserved(z, excluded)) return z;
	return INVALID_ZONE_ID;
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
	zone.Written_sectors = 0;
	zone.Epoch_write_sectors = 0;
	zone.Popularity = 0;
	zone.Valid_sectors_by_stream.clear();
	zone.Empty = true;
	zone.Migrating = false;
	zone.Written_blocks_by_stream.clear();
}

} // namespace RAID_Policy
