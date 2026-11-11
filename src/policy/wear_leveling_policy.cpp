#include "wear_leveling_policy.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

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

void WearLevelingPolicy::Configure_metric(const std::string& m, const std::string& t,
	const std::string& s, double precautionary, double critical)
{
	if (m != "TEC" && m != "WRITE_BYTES" && m != "BAD_BLOCK_COUNT")
		throw std::invalid_argument("Unsupported SWANS_Wear_Metric: " + m);
	if (t != "DYNAMIC_WEAR" && t != "IMBALANCE") throw std::invalid_argument("Invalid SWANS_Migration_Trigger");
	if (s != "CUMULATIVE" && s != "SSD_POPULARITY" && s != "GLOBAL_POPULARITY") throw std::invalid_argument("Invalid SWANS_Zone_Selection");
	if (m == "TEC" && (!std::isfinite(precautionary) || !std::isfinite(critical)
		|| precautionary < 0 || critical <= precautionary))
		throw std::invalid_argument("TEC requires explicit 0 <= precautionary < critical thresholds in percent points");
	metric = m; trigger = t; selection = s;
	if (m == "TEC") { th_precautionary_write_units = precautionary; th_critical_write_units = critical; }
}

void WearLevelingPolicy::Update_wear(const WearSnapshot& w)
{
	if (metric == "TEC" && !w.Is_valid(ssd_count)) throw std::runtime_error("Invalid TEC wear snapshot");
	live_wear = w;
}

double WearLevelingPolicy::Metric_value(unsigned int i) const
{
	return metric == "TEC" ? live_wear.Values.at(i) * 100.0
		: static_cast<double>(actual_write_bytes[i]) / balance_unit_bytes;
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
		mean += Metric_value(i);
	}
	if (mean == 0.0) {
		return 0.0;
	}
	mean /= static_cast<double>(ssd_count);
	double variance = 0.0;
	for (unsigned int i = 0; i < ssd_count; i++) {
		const double diff = Metric_value(i) - mean;
		variance += diff * diff;
	}
	variance /= (double)ssd_count;
	return std::sqrt(variance);
}

unsigned int WearLevelingPolicy::Pick_hottest_ssd() const
{
	unsigned int selected = 0;
	double best = 0;
	for (unsigned int i = 0; i < ssd_count; i++) {
		if (i == 0 || Metric_value(i) > best) {
			selected = i;
			best = Metric_value(i);
		}
	}
	return selected;
}

unsigned int WearLevelingPolicy::Pick_coldest_ssd() const
{
	unsigned int selected = 0;
	double best = 0;
	for (unsigned int i = 0; i < ssd_count; i++) {
		if (i == 0 || Metric_value(i) < best) {
			selected = i;
			best = Metric_value(i);
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

PolicyDecision WearLevelingPolicy::Evaluate(const ZoneDirectory& directory, const WearSnapshot& wear)
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

	Update_wear(wear);
	decision.Mu = Compute_stddev();
	last_mu = decision.Mu;

	// Migration is independent of write-byte imbalance and its legacy thresholds.
	// Wear_max is the fixed lifetime limit (1), not the largest observed SSD wear.
	decision.Wear_valid = wear.Is_valid(ssd_count);
	unsigned int hot_ssd = 0, cold_ssd = 0;
	bool migrate = false;
	if (decision.Wear_valid) {
		decision.Wear_values = wear.Values;
		decision.Wear_average = wear.Average();
		decision.Wear_threshold = wear.Migration_threshold();
		hot_ssd = static_cast<unsigned int>(std::max_element(wear.Values.begin(), wear.Values.end()) - wear.Values.begin());
		cold_ssd = static_cast<unsigned int>(std::min_element(wear.Values.begin(), wear.Values.end()) - wear.Values.begin());
		migrate = hot_ssd != cold_ssd && decision.Wear_threshold < wear.Values[hot_ssd];
	}
	if (trigger == "IMBALANCE") {
		hot_ssd = Pick_hottest_ssd(); cold_ssd = Pick_coldest_ssd();
		migrate = hot_ssd != cold_ssd && decision.Mu >= th_critical_write_units;
	}
	// Placement uses the selected metric, including live TEC in redirect.
	auto apply_placement = [&]() {
		const unsigned int write_hot = Pick_hottest_ssd();
		const unsigned int write_cold = Pick_coldest_ssd();
		decision.State = PolicyState::NORMAL;
		decision.Redirect.Valid = false;
		if (write_hot != write_cold && decision.Mu >= th_precautionary_write_units) {
			decision.State = PolicyState::REDIRECT;
			decision.Redirect.Valid = true;
			decision.Redirect.Hot_ssd = write_hot;
			decision.Redirect.Cold_ssd = write_cold;
		}
	};
	if (!migrate) {
		apply_placement();
		current_state = decision.State;
		Reset_epoch();
		return decision;
	}

	decision.State = PolicyState::MIGRATION;	// -> MIGRATION 시도
	decision.Redirect.Valid = false;
	std::vector<uint64_t> reserved;
	while (decision.Migrations.size() < max_concurrent_migrations) {
		uint64_t hot_zone = selection == "GLOBAL_POPULARITY"
			? directory.Find_migration_zone(0, true, false, reserved)
			: directory.Find_migration_zone(hot_ssd, false, selection == "CUMULATIVE", reserved);
		if (hot_zone == INVALID_ZONE_ID) break;
		if (selection == "GLOBAL_POPULARITY") {
			hot_ssd = directory.Owner_ssd(hot_zone);
			// Global heat chooses the source; the configured wear trigger still gates it.
			if (hot_ssd == cold_ssd || (trigger == "DYNAMIC_WEAR"
				&& (!decision.Wear_valid || wear.Values[hot_ssd] <= decision.Wear_threshold))) {
				reserved.push_back(hot_zone); continue;
			}
		}
		uint64_t cold_zone = directory.Find_empty_zone_on_ssd(cold_ssd, reserved);
		uint64_t reserve_zone = INVALID_ZONE_ID;
		if (cold_zone == INVALID_ZONE_ID) {
			// op_zone_migration: only when the selected destination has no
			// available ordinary empty zone. Cold -> source OP, then hot ->
			// freed cold location; the former hot location becomes source OP.
			reserve_zone = directory.Find_reserve_zone_on_ssd(hot_ssd, reserved);
			if (reserve_zone != INVALID_ZONE_ID)
				cold_zone = directory.Find_coldest_used_zone_on_ssd(cold_ssd, reserved);
		}
		if (hot_zone == INVALID_ZONE_ID || cold_zone == INVALID_ZONE_ID || hot_zone == cold_zone) {
			// A global candidate on another SSD may have its own free spare.
			if (selection == "GLOBAL_POPULARITY") {
				reserved.push_back(hot_zone);
				continue;
			}
			break;
		}

		MigrationOp op;
		op.Hot_zone = hot_zone;
		op.Cold_zone = cold_zone;
		op.Hot_ssd = hot_ssd;
		op.Cold_ssd = cold_ssd;
		op.Reserve_zone = reserve_zone;
		decision.Migrations.push_back(op);
		reserved.push_back(hot_zone);
		reserved.push_back(cold_zone);
		if (reserve_zone != INVALID_ZONE_ID) reserved.push_back(reserve_zone);
	}
	// No movable source or free destination: retain write-based placement.
	if (decision.Migrations.empty()) {
		apply_placement();
	}
	current_state = decision.State;
	Reset_epoch(); 
	return decision;
}

} // namespace RAID_Policy
