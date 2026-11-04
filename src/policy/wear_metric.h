#ifndef RAID_WEAR_METRIC_H
#define RAID_WEAR_METRIC_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace RAID_Policy {

// Metric-independent contract: one lifetime-consumed fraction per SSD.
// 0 = new, 1 = EOL. An empty/invalid snapshot disables wear-driven migration.
// TEC uses TEC / Emax on this scale, independently of the remaining-OP EOL rule.
struct WearSnapshot {
	std::vector<double> Values;

	static double Maximum() { return 1.0; }
	bool Is_valid(unsigned int ssd_count) const {
		if (ssd_count == 0 || Values.size() != ssd_count) return false;
		for (double value : Values)
			if (!std::isfinite(value) || value < 0.0 || value > Maximum()) return false;
		return true;
	}
	double Average() const {
		double sum = 0.0;
		for (double value : Values) sum += value;
		return Values.empty() ? 0.0 : sum / Values.size();
	}
	double Migration_threshold() const { return (Average() + Maximum()) / 2.0; }
};

// Match the existing EOL rule: (initial OP blocks - bad blocks) / N <= threshold.
// Use the block manager's already-rounded initial OP budget, not N * nominal OP.
inline double Bad_block_wear(uint64_t bad_blocks, uint64_t total_blocks,
	uint64_t initial_op_blocks, double eol_remaining_op_ratio)
{
	const double budget = static_cast<double>(initial_op_blocks)
		- static_cast<double>(total_blocks) * eol_remaining_op_ratio;
	if (total_blocks == 0 || initial_op_blocks > total_blocks
		|| !std::isfinite(eol_remaining_op_ratio) || eol_remaining_op_ratio < 0.0
		|| !std::isfinite(budget) || budget <= 0.0)
		return std::numeric_limits<double>::quiet_NaN();
	return std::min(1.0, static_cast<double>(bad_blocks) / budget);
}

} // namespace RAID_Policy
#endif
