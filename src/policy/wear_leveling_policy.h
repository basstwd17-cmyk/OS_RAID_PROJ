#ifndef RAID_SWANS_WEAR_LEVELING_POLICY_H
#define RAID_SWANS_WEAR_LEVELING_POLICY_H

#include <vector>
#include "swans_policy_types.h"
#include "zone_directory.h"

namespace RAID_Policy {

class WearLevelingPolicy
{
public:
	WearLevelingPolicy();

	void Initialize(unsigned int ssd_count,
		double th_precautionary_write_units,
		double th_critical_write_units,
		unsigned int max_concurrent_migrations,
		uint64_t balance_unit_bytes);

	void Observe_host_write(unsigned int ssd_id, uint64_t write_bytes);
	// Records a destination-LBA migration write only after its background request
	// completes. Historical writes are never reassigned between SSDs.
	void Observe_completed_migration_write(unsigned int ssd_id, uint64_t write_bytes);
	PolicyDecision Evaluate(const ZoneDirectory& directory);
	bool Has_epoch_writes() const;

	double Last_mu() const { return last_mu; }
	double Current_mu() const { return Compute_stddev(); }
	PolicyState Current_state() const { return current_state; }
	unsigned int Current_hottest_ssd() const { return Pick_hottest_ssd(); }
	unsigned int Current_coldest_ssd() const { return Pick_coldest_ssd(); }
	double Precautionary_threshold() const { return th_precautionary_write_units; }
	double Critical_threshold() const { return th_critical_write_units; }
	uint64_t Balance_unit_bytes() const { return balance_unit_bytes; }
	uint64_t Actual_write_bytes(unsigned int ssd_id) const;
	uint64_t Observed_host_write_bytes(unsigned int ssd_id) const;
	uint64_t Observed_migration_write_bytes(unsigned int ssd_id) const;
	uint64_t Total_actual_write_bytes() const;
	uint64_t Total_observed_host_write_bytes() const;
	uint64_t Total_observed_migration_write_bytes() const;

private:
	unsigned int Pick_hottest_ssd() const;
	unsigned int Pick_coldest_ssd() const;
	double Compute_stddev() const;
	void Reset_epoch();

	bool initialized;
	unsigned int ssd_count;
	double th_precautionary_write_units;
	double th_critical_write_units;
	uint64_t balance_unit_bytes;
	unsigned int max_concurrent_migrations;
	std::vector<uint64_t> epoch_write_bytes;	// completed logical writes since the last policy evaluation
	std::vector<uint64_t> actual_write_bytes;	// completed host + migration destination LBA bytes; never decremented
	std::vector<uint64_t> observed_host_write_bytes;
	std::vector<uint64_t> observed_migration_write_bytes;
	double last_mu;
	PolicyState current_state;
};

} // namespace RAID_Policy

#endif // RAID_SWANS_WEAR_LEVELING_POLICY_H
