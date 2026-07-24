#include <cassert>
#include <cmath>

#include "../src/policy/wear_leveling_policy.h"

int main()
{
	RAID_Policy::WearLevelingPolicy policy;
	policy.Initialize(4, 5.0, 15.0, 1);

	assert(!policy.Has_epoch_writes());
	policy.Observe_host_write(0, 1);
	assert(policy.Has_epoch_writes());

	const double expected_mu = std::sqrt(1875.0);
	RAID_Policy::ZoneDirectory directory;
	directory.Initialize(4, 512, 16384, 1024, 4ULL * 16384ULL);
	directory.Observe_write(0, 0, 0, 1);
	const RAID_Policy::PolicyDecision decision = policy.Evaluate(directory);
	assert(std::fabs(decision.Mu - expected_mu) < 1e-9);
	assert(decision.State == RAID_Policy::PolicyState::MIGRATION);
	assert(decision.Migrations.size() == 1);

	return 0;
}
