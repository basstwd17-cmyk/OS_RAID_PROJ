#include "wear_leveling_policy.h"

#include <algorithm>
#include <cmath>

namespace RAID_Policy {

WearLevelingPolicy::WearLevelingPolicy()
	: initialized(false),
	  ssd_count(0),
	  th_precautionary_write_units(0.0),
	  th_critical_write_units(0.0),
	  balance_unit_bytes(1024ULL * 1024ULL),
	  max_concurrent_migrations(1),
	  last_mu(0.0),
	  current_state(PolicyState::NORMAL)
{
}

void WearLevelingPolicy::Initialize(unsigned int ssd_count,
	double th_precautionary_write_units,
	double th_critical_write_units,
	unsigned int max_concurrent_migrations,
	uint64_t balance_unit_bytes)
{
	this->ssd_count = ssd_count;
	this->th_precautionary_write_units = th_precautionary_write_units;
	this->th_critical_write_units = th_critical_write_units;
	this->balance_unit_bytes = balance_unit_bytes == 0 ? 1024ULL * 1024ULL : balance_unit_bytes;
	this->max_concurrent_migrations = max_concurrent_migrations == 0 ? 1 : max_concurrent_migrations;
	epoch_write_bytes.assign(ssd_count, 0);
	actual_write_bytes.assign(ssd_count, 0);
	observed_host_write_bytes.assign(ssd_count, 0);
	observed_migration_write_bytes.assign(ssd_count, 0);
	last_mu = 0.0;
	current_state = PolicyState::NORMAL;
	initialized = true;
}

void WearLevelingPolicy::Observe_host_write(unsigned int ssd_id, uint64_t write_bytes)
{
	if (!initialized || ssd_id >= epoch_write_bytes.size() || write_bytes == 0) {
		return;
	}
	epoch_write_bytes[ssd_id] += write_bytes;
	actual_write_bytes[ssd_id] += write_bytes;
	observed_host_write_bytes[ssd_id] += write_bytes;
}

void WearLevelingPolicy::Observe_completed_migration_write(unsigned int ssd_id, uint64_t write_bytes)
{
	if (!initialized || ssd_id >= observed_migration_write_bytes.size() || write_bytes == 0) {
		return;
	}
	epoch_write_bytes[ssd_id] += write_bytes;
	actual_write_bytes[ssd_id] += write_bytes;
	observed_migration_write_bytes[ssd_id] += write_bytes;
}

uint64_t WearLevelingPolicy::Actual_write_bytes(unsigned int ssd_id) const
{
	return ssd_id < actual_write_bytes.size() ? actual_write_bytes[ssd_id] : 0;
}

uint64_t WearLevelingPolicy::Observed_host_write_bytes(unsigned int ssd_id) const
{
	return ssd_id < observed_host_write_bytes.size() ? observed_host_write_bytes[ssd_id] : 0;
}

uint64_t WearLevelingPolicy::Observed_migration_write_bytes(unsigned int ssd_id) const
{
	return ssd_id < observed_migration_write_bytes.size() ? observed_migration_write_bytes[ssd_id] : 0;
}

uint64_t WearLevelingPolicy::Total_actual_write_bytes() const
{
	uint64_t total = 0;
	for (uint64_t count : actual_write_bytes) {
		total += count;
	}
	return total;
}

uint64_t WearLevelingPolicy::Total_observed_host_write_bytes() const
{
	uint64_t total = 0;
	for (uint64_t count : observed_host_write_bytes) {
		total += count;
	}
	return total;
}

uint64_t WearLevelingPolicy::Total_observed_migration_write_bytes() const
{
	uint64_t total = 0;
	for (uint64_t bytes : observed_migration_write_bytes) {
		total += bytes;
	}
	return total;
}

bool WearLevelingPolicy::Has_epoch_writes() const
{
	if (!initialized) {
		return false;
	}
	for (size_t i = 0; i < epoch_write_bytes.size(); i++) {
		if (epoch_write_bytes[i] > 0) {
			return true;
		}
	}
	return false;
}
// Actual completed host and migration-destination LBA writes. Values are
// normalized to the configured unit before comparison with the thresholds.
double WearLevelingPolicy::Compute_stddev() const
{
	if (!initialized || ssd_count == 0) {
		return 0.0;
	}
	double mean = 0.0;
	for (unsigned int i = 0; i < ssd_count; i++) {
		mean += static_cast<double>(actual_write_bytes[i]) / static_cast<double>(balance_unit_bytes);
	}
	if (mean == 0.0) {
		return 0.0;
	}
	mean /= static_cast<double>(ssd_count);
	double variance = 0.0;
	for (unsigned int i = 0; i < ssd_count; i++) {
		const double diff = static_cast<double>(actual_write_bytes[i]) / static_cast<double>(balance_unit_bytes) - mean;
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
		if (i == 0 || actual_write_bytes[i] > best) {
			selected = i;
			best = actual_write_bytes[i];
		}
	}
	return selected;
}

unsigned int WearLevelingPolicy::Pick_coldest_ssd() const
{
	unsigned int selected = 0;
	uint64_t best = 0;
	for (unsigned int i = 0; i < ssd_count; i++) {
		if (i == 0 || actual_write_bytes[i] < best) {
			selected = i;
			best = actual_write_bytes[i];
		}
	}
	return selected;
}

void WearLevelingPolicy::Reset_epoch()
{
	for (unsigned int i = 0; i < epoch_write_bytes.size(); i++) {
		epoch_write_bytes[i] = 0;
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

	if (decision.Mu < th_precautionary_write_units) {	// ->NORMAL
		decision.State = PolicyState::NORMAL;
		current_state = decision.State;
		Reset_epoch();
		return decision;
	}

	if (decision.Mu < th_critical_write_units) {	// -> REDIRECT + Redirect.Valid=true + hot/cold SSD 설정
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
	// Actual migration writes can make an SSD the byte-hot member even when it
	// has no movable host-data zone. In that case, redirect future writes rather
	// than repeatedly scheduling an empty migration epoch.
	if (decision.Migrations.empty()) {
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
