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
		double th_precautionary,
		double th_critical,
		unsigned int max_concurrent_migrations);

	void Observe_host_write(unsigned int ssd_id, unsigned int write_count);
	void Observe_migration_write(unsigned int ssd_id, unsigned int write_sectors);
	void Transfer_writes(unsigned int from_ssd, unsigned int to_ssd, uint64_t write_count);
	PolicyDecision Evaluate(const ZoneDirectory& directory);
	bool Has_epoch_writes() const;

	double Last_mu() const { return last_mu; }
	PolicyState Current_state() const { return current_state; }

private:
	unsigned int Pick_hottest_ssd() const;
	unsigned int Pick_coldest_ssd() const;
	double Compute_stddev() const;
	void Reset_epoch();

	bool initialized;
	unsigned int ssd_count;
	double th_precautionary;
	double th_critical;
	unsigned int max_concurrent_migrations;
	std::vector<uint64_t> epoch_writes;	// recent host write activity for event scheduling
	std::vector<uint64_t> placement_writes;	// SIT-style SSD write counts for hot/cold selection
	std::vector<uint64_t> observed_host_writes;	// host writes observed by WAM; never decremented
	std::vector<uint64_t> observed_migration_writes;	// migration restore writes; never decremented
	double last_mu;
	PolicyState current_state;
};

} // namespace RAID_Policy

#endif // RAID_SWANS_WEAR_LEVELING_POLICY_H
