#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include "../src/policy/wear_leveling_policy.h"

using namespace RAID_Policy;

static ZoneDirectory directory(unsigned int count = 4)
{
	ZoneDirectory d;
	d.Initialize(count, 8, 8, 8, count * 8 * 4);
	for (unsigned int i = 0; i < count; ++i) d.Observe_write(i, 0, 8, true);
	return d;
}

static WearSnapshot snapshot(std::initializer_list<double> values)
{
	WearSnapshot result;
	result.Values = values;
	return result;
}

int main()
{
	// Normalize against the actual EOL OP budget, including heterogeneous capacity.
	assert(Bad_block_wear(0, 1000, 200, 0.05) == 0);
	assert(Bad_block_wear(75, 1000, 200, 0.05) == 0.5);
	assert(Bad_block_wear(150, 2000, 400, 0.05) == 0.5);
	assert(Bad_block_wear(50, 1000, 150, 0.05) == 0.5);
	assert(Bad_block_wear(150, 1000, 200, 0.05) == 1);
	assert(Bad_block_wear(151, 1000, 200, 0.05) == 1);
	assert(std::abs(Bad_block_wear(10, 103, 20, 0.05) - 10.0 / 14.85) < 1e-12);
	assert(std::isnan(Bad_block_wear(0, 0, 0, 0.05)));
	assert(std::isnan(Bad_block_wear(0, 1000, 50, 0.05)));
	assert(std::isnan(Bad_block_wear(0, 1000, 200, -1)));

	auto d = directory();
	WearLevelingPolicy policy;
	policy.Initialize(4, 8, 32, 2, 1);
	assert(policy.Evaluate(d, snapshot({0, 0, 0, 0})).State == PolicyState::NORMAL);
	assert(policy.Evaluate(d, snapshot({1, 1, 1, 1})).Migrations.empty());
	// Exactly representable boundary: mean .5, threshold .75, source .75.
	assert(policy.Evaluate(d, snapshot({.75, .25, .5, .5})).Migrations.empty());
	assert(!policy.Evaluate(d, snapshot({.750001, .25, .5, .5})).Migrations.empty());
	assert(policy.Evaluate(d, snapshot({.749999, .25, .5, .5})).Migrations.empty());
	// Same wear gap, older cluster: the fixed-limit formula becomes more aggressive.
	assert(policy.Evaluate(d, snapshot({.4, .1, .1, .1})).Migrations.empty());
	assert(!policy.Evaluate(d, snapshot({.9, .6, .6, .6})).Migrations.empty());
	// No writes in the policy counters and below both legacy thresholds: still migrate.
	auto decision = policy.Evaluate(d, snapshot({.2, .2, .8, .2}));
	assert(decision.State == PolicyState::MIGRATION && decision.Mu == 0);
	assert(decision.Migrations[0].Hot_ssd == 2 && decision.Migrations[0].Cold_ssd == 0);
	assert(std::abs(decision.Wear_threshold - .675) < 1e-12);
	// Migration source follows physical wear even if another SSD has more logical writes.
	policy.Observe_host_write(1, 1000);
	policy.Observe_completed_migration_write(1, 1000);
	decision = policy.Evaluate(d, snapshot({.2, .2, .8, .2}));
	assert(decision.Migrations[0].Hot_ssd == 2);
	decision = policy.Evaluate(d, snapshot({0, 0, 0, 0}));
	assert(decision.State == PolicyState::REDIRECT && decision.Migrations.empty());
	assert(decision.Redirect.Hot_ssd == 1); // exceeding legacy critical never migrates
	// Invalid snapshots cannot initiate migration; placement can still operate.
	for (const auto& bad : {snapshot({}), snapshot({.9}), snapshot({.9, -.1, 0, 0}),
		snapshot({.9, 1.1, 0, 0}), snapshot({.9, std::numeric_limits<double>::quiet_NaN(), 0, 0}),
		snapshot({.9, std::numeric_limits<double>::infinity(), 0, 0})}) {
		decision = policy.Evaluate(d, bad);
		assert(!decision.Wear_valid && decision.Migrations.empty());
	}
	// Multiple operations must not reuse a source or destination zone.
	d.Observe_write(6, 0, 8, true); // second used zone on SSD 2
	decision = policy.Evaluate(d, snapshot({.2, .2, .8, .2}));
	assert(decision.Migrations.size() == 2);
	assert(decision.Migrations[0].Hot_zone != decision.Migrations[1].Hot_zone);
	assert(decision.Migrations[0].Cold_zone != decision.Migrations[1].Cold_zone);
	for (uint64_t z = 0; z < d.Zone_count(); z += 4) d.Observe_write(z, 0, 8, true);
	decision = policy.Evaluate(d, snapshot({.2, .2, .8, .2}));
	assert(decision.Migrations.empty()); // no empty zone on destination
	assert(decision.State == PolicyState::REDIRECT);
	ZoneDirectory empty;
	assert(policy.Evaluate(empty, snapshot({.9, 0, 0, 0})).Migrations.empty());
	WearLevelingPolicy single;
	single.Initialize(1, 0, 0, 1, 1);
	assert(single.Evaluate(directory(1), snapshot({1})).Migrations.empty());
	std::cout << "wear trigger tests passed\n";
}
