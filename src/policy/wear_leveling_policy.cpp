#include "wear_leveling_policy.h"

#include <algorithm>
#include <cmath>

namespace RAID_Policy {

WearLevelingPolicy::WearLevelingPolicy()
	: initialized(false),
	  ssd_count(0),
	  th_precautionary(0.0),
	  th_critical(0.0),
	  max_concurrent_migrations(1),
	  last_mu(0.0),
	  current_state(PolicyState::NORMAL)
{
}

void WearLevelingPolicy::Initialize(unsigned int ssd_count,
	double th_precautionary,
	double th_critical,
	unsigned int max_concurrent_migrations)
{
	this->ssd_count = ssd_count;
	this->th_precautionary = th_precautionary;
	this->th_critical = th_critical;
	this->max_concurrent_migrations = max_concurrent_migrations == 0 ? 1 : max_concurrent_migrations;
	epoch_writes.assign(ssd_count, 0);
	placement_writes.assign(ssd_count, 0);
	observed_host_writes.assign(ssd_count, 0);
	observed_migration_writes.assign(ssd_count, 0);
	last_mu = 0.0;
	current_state = PolicyState::NORMAL;
	initialized = true;
}

void WearLevelingPolicy::Observe_host_write(unsigned int ssd_id, unsigned int write_count)
{
	if (!initialized || ssd_id >= epoch_writes.size()) {
		return;
	}
	epoch_writes[ssd_id] += write_count;
	placement_writes[ssd_id] += write_count;
	observed_host_writes[ssd_id] += write_count;
}

void WearLevelingPolicy::Observe_migration_write(unsigned int ssd_id, unsigned int write_sectors)
{
	if (!initialized || ssd_id >= observed_migration_writes.size()) {
		return;
	}
	observed_migration_writes[ssd_id] += write_sectors;
}

void WearLevelingPolicy::Transfer_writes(unsigned int from_ssd, unsigned int to_ssd, uint64_t write_count)
{
	if (!initialized || from_ssd >= placement_writes.size() || to_ssd >= placement_writes.size() || write_count == 0) {
		return;
	}
	placement_writes[from_ssd] = placement_writes[from_ssd] > write_count
		? placement_writes[from_ssd] - write_count
		: 0;
	placement_writes[to_ssd] += write_count;
}

bool WearLevelingPolicy::Has_epoch_writes() const
{
	if (!initialized) {
		return false;
	}
	for (size_t i = 0; i < epoch_writes.size(); i++) {
		if (epoch_writes[i] > 0) {
			return true;
		}
	}
	return false;
}
// SIT-style cumulative write-request share imbalance. Unit: percentage points.
double WearLevelingPolicy::Compute_stddev() const
{
	if (!initialized || ssd_count == 0) {
		return 0.0;
	}
	uint64_t total = 0;
	for (unsigned int i = 0; i < ssd_count; i++) {
		total += placement_writes[i];
	}
	if (total == 0) {
		return 0.0;
	}

	const double mean_share = 100.0 / (double)ssd_count;
	double variance = 0.0;
	for (unsigned int i = 0; i < ssd_count; i++) {
		const double share = 100.0 * (double)placement_writes[i] / (double)total;
		const double diff = share - mean_share;
		variance += diff * diff;
	}
	variance /= (double)ssd_count;
	return std::sqrt(variance);
}

unsigned int WearLevelingPolicy::Pick_hottest_ssd() const
{
	unsigned int selected = 0;
	uint64_t best = 0;
	for (unsigned int i = 0; i < ssd_count; i++) {
		if (i == 0 || placement_writes[i] > best) {
			selected = i;
			best = placement_writes[i];
		}
	}
	return selected;
}

unsigned int WearLevelingPolicy::Pick_coldest_ssd() const
{
	unsigned int selected = 0;
	uint64_t best = 0;
	for (unsigned int i = 0; i < ssd_count; i++) {
		if (i == 0 || placement_writes[i] < best) {
			selected = i;
			best = placement_writes[i];
		}
	}
	return selected;
}

void WearLevelingPolicy::Reset_epoch()
{
	for (unsigned int i = 0; i < epoch_writes.size(); i++) {
		epoch_writes[i] = 0;
	}
}

PolicyDecision WearLevelingPolicy::Evaluate(const ZoneDirectory& directory)
{
	PolicyDecision decision;	// 바로 NORMAL 처리 (초기화x,ssd1개,zonedirectory 미초기화면)
	if (!initialized || ssd_count < 2 || !directory.Is_initialized()) {
		decision.State = PolicyState::NORMAL;
		decision.Mu = 0.0;
		current_state = decision.State;
		last_mu = decision.Mu;
		Reset_epoch();
		return decision;
	}

	decision.Mu = Compute_stddev();
	last_mu = decision.Mu;

	const unsigned int hot_ssd = Pick_hottest_ssd();
	const unsigned int cold_ssd = Pick_coldest_ssd();
	if (hot_ssd == cold_ssd) {	// normal로 바로 종료
		decision.State = PolicyState::NORMAL;
		current_state = decision.State;
		Reset_epoch();
		return decision;
	}

	if (decision.Mu < th_precautionary) {	// ->NORMAL
		decision.State = PolicyState::NORMAL;
		current_state = decision.State;
		Reset_epoch();
		return decision;
	}

	if (decision.Mu < th_critical) {	// -> REDIRECT + Redirect.Vaild=true + hot/cold SSD 세팅
		decision.State = PolicyState::REDIRECT;
		decision.Redirect.Valid = true;
		decision.Redirect.Hot_ssd = hot_ssd;
		decision.Redirect.Cold_ssd = cold_ssd;
		current_state = decision.State;
		Reset_epoch();
		return decision;
	}

	decision.State = PolicyState::MIGRATION;	// -> MIGRATION 시도
	decision.Redirect.Valid = false;
	std::vector<uint64_t> reserved;
	for (unsigned int i = 0; i < max_concurrent_migrations; i++) {	// 벡터로 같은 zone 중복 선택 방지
		uint64_t hot_zone = directory.Find_hottest_used_zone_on_ssd(hot_ssd, reserved);
		uint64_t cold_zone = directory.Find_empty_zone_on_ssd(cold_ssd, reserved);
		if (hot_zone == INVALID_ZONE_ID || cold_zone == INVALID_ZONE_ID || hot_zone == cold_zone) {
			break;
		}

		MigrationOp op;
		op.Hot_zone = hot_zone;
		op.Cold_zone = cold_zone;
		op.Hot_ssd = hot_ssd;
		op.Cold_ssd = cold_ssd;
		decision.Migrations.push_back(op);
		reserved.push_back(hot_zone);
		reserved.push_back(cold_zone);
	}
	if (decision.Migrations.empty()) {	// vaild 못만들면 REDIRECT 실행행
		decision.State = PolicyState::REDIRECT;
		decision.Redirect.Valid = true;
		decision.Redirect.Hot_ssd = hot_ssd;
		decision.Redirect.Cold_ssd = cold_ssd;
	}

	current_state = decision.State;
	Reset_epoch(); 
	return decision;
}

} // namespace RAID_Policy
