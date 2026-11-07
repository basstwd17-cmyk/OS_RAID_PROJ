#include "../src/policy/migration_executor.h"
#include "../src/policy/wear_leveling_policy.h"
#include <cassert>
#include <iostream>
#include <map>
#include <stdexcept>
#include <tuple>
using namespace RAID_Policy;
using namespace SSD_Components;

using Location = std::pair<unsigned int, LHA_type>;
Location location(const ZoneDirectory& d, uint64_t zone, uint64_t offset = 0) {
	unsigned int disk; LHA_type lba; d.Resolve_zone_lba(zone, offset, disk, lba);
	return {disk, lba};
}
MigrationTask plan(const ZoneDirectory& d, const MigrationOp& op) {
	MigrationTask task; task.Op = op;
	auto copies = [&](uint64_t source, uint64_t target, std::vector<StripeCopyPlan>& out) {
		for (const auto& stream : d.Written_block_offsets_by_stream(source)) for (auto block : stream.second) {
			StripeCopyPlan copy; copy.Stream_id = stream.first; copy.Stripe_offset = block; copy.Lba_count = 8;
			d.Resolve_zone_lba(source, block * 8, copy.Source_disk_id, copy.Source_lba);
			d.Resolve_zone_lba(target, block * 8, copy.Destination_disk_id, copy.Destination_lba);
			out.push_back(copy);
		}
	};
	copies(op.Hot_zone, op.Cold_zone, task.Copies);
	if (op.Reserve_zone != INVALID_ZONE_ID) copies(op.Cold_zone, op.Reserve_zone, task.Cold_copies);
	return task;
}

void exchange(unsigned mode, bool half_decay) {
	ZoneDirectory d; d.Initialize(4, 16, 16, 8, 128, true, 48);
	d.Configure_popularity(half_decay ? "HALF_DECAY" : "LAST_EPOCH", 10);
	std::map<Location, int> storage;
	for (uint64_t z = 0; z < d.User_zone_count(); ++z) {
		d.Observe_write(z, 0, 16, true);
		for (unsigned b = 0; b < 2; ++b) storage[location(d, z, b * 8)] = 100 + z * 2 + b;
	}
	d.Observe_write(0, 0, 8, true); d.Advance_popularity(10);
	WearLevelingPolicy policy; policy.Initialize(4, 1e12, 0, 4, 1024);
	WearSnapshot wear; wear.Values = {.9, .5, .3, .1};
	auto decision = policy.Evaluate(d, wear);
	assert(decision.Migrations.size() == 1); // exactly one spare on old SSD
	auto op = decision.Migrations.front();
	assert(op.Hot_zone == 0 && op.Cold_zone == 3 && op.Reserve_zone == 8);
	assert(d.Find_empty_zone_on_ssd(3, {}) == INVALID_ZONE_ID);
	bool rejected = false; try { ZoneResolveResult r; d.Resolve(128, r); } catch (const std::out_of_range&) { rejected = true; }
	assert(rejected);
	for (unsigned round = 0; round < 2; ++round) {
		const double hot_heat = d.Zone_popularity(op.Hot_zone);
		const double cold_heat = d.Zone_popularity(op.Cold_zone);
		const double other_heat = d.Zone_popularity(1);
		const auto hot_writes = d.Zone_written_sectors(op.Hot_zone);
		auto task = plan(d, op);
		auto hot = location(d, op.Hot_zone), cold = location(d, op.Cold_zone), spare = location(d, op.Reserve_zone);
		auto young_spare = location(d, d.Find_reserve_zone_on_ssd(3));
		int h = storage.at(hot), c = storage.at(cold);
		MigrationExecutor executor(2); MigrationLimits limits;
		limits.Pause_duration = 5;
		if (mode == 1) limits.Slice_sectors = 8;
		if (mode == 2) limits.Slice_time = 1;
		if (mode == 3) limits.Pause_on_read = true;
		executor.Configure_limits(limits);
		unsigned finished = 0;
		executor.Task_finished = [&](uint64_t zone, bool aborted) { assert(zone == op.Hot_zone && !aborted); ++finished; };
		executor.Start({task}, d, 10);
		assert(d.Migrating_zone_count() == 3);
		User_Request write, read, unrelated, overflow;
		write.Type = UserRequestType::WRITE; write.Start_LBA = op.Hot_zone * 16; write.SizeInSectors = 8; write.Stream_id = 0;
		read.Type = UserRequestType::READ; read.Start_LBA = op.Cold_zone * 16; read.SizeInSectors = 8; read.Stream_id = 0;
		unrelated.Type = UserRequestType::READ; unrelated.Start_LBA = 16; unrelated.SizeInSectors = 8; unrelated.Stream_id = 0;
		overflow.Type = UserRequestType::WRITE; overflow.Start_LBA = op.Cold_zone * 16; overflow.SizeInSectors = 8; overflow.Stream_id = 0;
		unsigned observed = 0;
		auto observe = [&](const MigrationTask&, const User_Request*) { ++observed; };
		assert(executor.Maybe_intercept(&write, {op.Hot_zone}, d, observe, 10) == MigrationExecutor::InterceptResult::BUFFERED);
		assert(executor.Maybe_intercept(&read, {op.Cold_zone}, d, observe, 10) == MigrationExecutor::InterceptResult::BUFFERED);
		assert(executor.Maybe_intercept(&overflow, {op.Cold_zone}, d, observe, 10) == MigrationExecutor::InterceptResult::BACKPRESSURE);
		assert(observed == 0);
		std::map<unsigned int, int> buffer;
		io_request_id_type active;
		unsigned io = 0, discarded = 0, failed_discards = 0;
		auto submit = [&](const StripeCopyPlan& copy, bool is_write, uint64_t) {
			assert(active.empty());
			if (!is_write) buffer[copy.Stripe_offset] = storage.at({copy.Source_disk_id, copy.Source_lba});
			else storage[{copy.Destination_disk_id, copy.Destination_lba}] = buffer.at(copy.Stripe_offset);
			active = std::to_string(++io); return active;
		};
		auto discard = [&](const StripeCopyPlan& copy, uint64_t) {
			// Force one invalidation retry: the phase may not finish before it succeeds.
			if (!failed_discards++) return false;
			assert(storage.at({copy.Source_disk_id, copy.Source_lba}) == storage.at({copy.Destination_disk_id, copy.Destination_lba}));
			storage.erase({copy.Source_disk_id, copy.Source_lba}); ++discarded; return true;
		};
		for (sim_time_type now = 10; executor.Has_inflight() && now < 500; ++now) {
			if (!active.empty()) { auto id = active; active.clear(); assert(executor.Notify_request_completed(id, d)); }
			assert(executor.Maybe_intercept(&unrelated, {1}, d, observe, now) == MigrationExecutor::InterceptResult::SUBMITTED);
			executor.Poll(d, submit, discard, now);
			if (executor.Has_inflight()) { assert(executor.Drain_replay_requests().empty()); assert(d.Migrating_zone_count() == 3); }
			if (executor.Has_inflight()) assert(d.Zone_popularity(1) == other_heat);
		}
		const double factor = half_decay ? 0.5 : 1.0;
		assert(d.Zone_popularity(op.Hot_zone) == hot_heat * factor);
		assert(d.Zone_popularity(op.Cold_zone) == cold_heat * factor);
		assert(d.Zone_popularity(1) == other_heat * factor);
		assert(d.Zone_written_sectors(op.Hot_zone) == hot_writes);
		assert(!executor.Has_inflight() && finished == 1 && io == 8 && discarded == 4);
		assert(executor.Cold_stages_completed() == 1);
		assert(storage.at(location(d, op.Hot_zone)) == h && storage.at(location(d, op.Cold_zone)) == c);
		assert(location(d, op.Hot_zone) == cold && location(d, op.Cold_zone) == spare);
		assert(location(d, op.Reserve_zone) == hot && !storage.count(hot));
		assert(location(d, d.Find_reserve_zone_on_ssd(3)) == young_spare);
		assert(d.Find_reserve_zone_on_ssd(0) == op.Reserve_zone);
		assert(d.Duplicate_physical_location_count() == 0 && d.Migrating_zone_count() == 0);
		auto replay = executor.Drain_replay_requests();
		assert(replay.size() == 2 && replay[0].Request == &write && replay[1].Request == &read);
		assert(!replay[0].Complete_without_dispatch && !replay[1].Complete_without_dispatch);
		if (mode == 1) assert(executor.Sector_pauses() > 0);
		if (mode == 2) assert(executor.Time_pauses() > 0);
		if (mode == 3) assert(executor.Read_pauses() > 0);
		// Exchange another old zone with the now-young hot zone, reusing the rotated reserve.
		op.Cold_zone = op.Hot_zone; op.Hot_zone = 4;
	}
}

void policy_selection() {
	WearSnapshot wear; wear.Values = {.9, .8, .3, .1};
	for (const auto& metric : {"TEC", "BAD_BLOCK_COUNT", "WRITE_BYTES"}) {
		for (const auto& trigger : {"DYNAMIC_WEAR", "IMBALANCE"}) {
			for (const auto& selection : {"CUMULATIVE", "SSD_POPULARITY", "GLOBAL_POPULARITY"}) {
				ZoneDirectory d; d.Initialize(4, 16, 16, 8, 128, true, 48);
				d.Configure_popularity("LAST_EPOCH", 10);
				for (uint64_t z = 0; z < d.User_zone_count(); ++z) d.Observe_write(z, 0, 8, true);
				d.Observe_write(0, 0, 16, true);
				d.Advance_popularity(10);
				WearLevelingPolicy p; p.Initialize(4, 1, 2, 4, 1024);
				p.Configure_metric(metric, trigger, selection, 1, 2);
				p.Observe_host_write(0, 100 * 1024);
				auto decision = p.Evaluate(d, wear);
				assert(!decision.Migrations.empty());
				std::vector<uint64_t> used;
				for (const auto& op : decision.Migrations) {
					assert(op.Reserve_zone != INVALID_ZONE_ID);
					assert(d.Owner_ssd(op.Reserve_zone) == op.Hot_ssd);
					for (auto z : {op.Hot_zone, op.Cold_zone, op.Reserve_zone}) {
						assert(std::find(used.begin(), used.end(), z) == used.end());
						used.push_back(z);
					}
				}
				// Busy source spare must not be reused. Global selection can
				// continue to another eligible source with its own spare.
				d.Mark_migrating(8, true);
				decision = p.Evaluate(d, wear);
				if (std::string(selection) != "GLOBAL_POPULARITY") assert(decision.Migrations.empty());
				else {
					assert(!decision.Migrations.empty());
					for (const auto& op : decision.Migrations) assert(op.Hot_ssd != 0 && op.Reserve_zone != 8);
				}
			}
		}
	}
	ZoneDirectory d; d.Initialize(4, 16, 16, 8, 128);
	for (uint64_t z = 0; z < d.User_zone_count(); ++z) d.Observe_write(z, 0, 8, true);
	WearLevelingPolicy p; p.Initialize(4, 1e12, 0, 4, 1024);
	assert(p.Evaluate(d, wear).Migrations.empty()); // No reserve configured.
}

void op_zone_migration_contract() {
	// Four SSDs with exactly 100 backend zones each: expose 99, hide the last.
	ZoneDirectory d; d.Initialize(4, 16, 16, 8, 4 * 99 * 16, true, 100 * 16);
	assert(d.User_zone_count() == 396 && d.Zone_count() == 400);
	assert(d.Exported_sectors() == 4 * 99 * 16);
	for (unsigned ssd = 0; ssd < 4; ++ssd) {
		auto op = d.Find_reserve_zone_on_ssd(ssd);
		assert(op != INVALID_ZONE_ID && d.Is_empty(op));
		assert(location(d, op) == Location(ssd, 99 * 16));
	}
	bool hidden = false;
	try { ZoneResolveResult r; d.Resolve(d.Exported_sectors(), r); }
	catch (const std::out_of_range&) { hidden = true; }
	assert(hidden);
	// Destination SSD 1 has one ordinary empty zone: prefer it to an exchange.
	for (uint64_t z = 0; z < d.User_zone_count(); ++z)
		if (z != 1) d.Observe_write(z, 0, 8, true);
	d.Observe_write(0, 0, 8, true);
	WearSnapshot wear; wear.Values = {.1, .9, .3, .2};
	WearLevelingPolicy writes; writes.Initialize(4, 5, 32, 1, 1);
	writes.Configure_metric("WRITE_BYTES", "IMBALANCE", "SSD_POPULARITY", 0, 0);
	// Migration writes count too: SSD 0 has the most total writes despite
	// SSD 2 having the most host-only writes. TEC ranks SSD 1 as oldest.
	writes.Observe_host_write(2, 200);
	writes.Observe_completed_migration_write(0, 1000);
	writes.Observe_host_write(3, 3);
	auto empty = writes.Evaluate(d, wear).Migrations.at(0);
	assert(empty.Hot_ssd == 0 && empty.Cold_ssd == 1);
	assert(empty.Cold_zone == 1 && empty.Reserve_zone == INVALID_ZONE_ID);
	d.Observe_write(1, 0, 8, true);
	auto exchange = writes.Evaluate(d, wear).Migrations.at(0);
	assert(exchange.Hot_zone == 0 && exchange.Cold_zone == 1);
	assert(exchange.Reserve_zone == d.Find_reserve_zone_on_ssd(0));
	WearLevelingPolicy tec; tec.Initialize(4, 5, 10, 1, 1);
	tec.Configure_metric("TEC", "DYNAMIC_WEAR", "SSD_POPULARITY", 5, 10);
	auto tec_exchange = tec.Evaluate(d, wear).Migrations.at(0);
	assert(tec_exchange.Hot_ssd == 1 && tec_exchange.Cold_ssd == 0);
	assert(tec_exchange.Hot_zone == 1 && tec_exchange.Cold_zone == 4);
	assert(tec_exchange.Reserve_zone == d.Find_reserve_zone_on_ssd(1));
}

int main() {
	op_zone_migration_contract();
	policy_selection();
	for (unsigned mode = 0; mode < 4; ++mode) { exchange(mode, false); exchange(mode, true); }
	ZoneDirectory d; d.Initialize(4, 16, 16, 8, 128, true, 48);
	d.Observe_write(0, 0, 8, true); d.Configure_popularity("LAST_EPOCH", 10); d.Advance_popularity(10);
	WearLevelingPolicy p; p.Initialize(4, 1e12, 0, 1, 1024); WearSnapshot w; w.Values = {.9, .5, .3, .1};
	auto op = p.Evaluate(d, w).Migrations.at(0); assert(op.Reserve_zone == INVALID_ZONE_ID);
	MigrationExecutor executor; executor.Start({plan(d, op)}, d);
	const auto heat_before_abort = d.Zone_popularity(0);
	auto abort = executor.Abort_all(d); assert(abort.Task_count == 1 && d.Migrating_zone_count() == 2);
	assert(d.Zone_popularity(0) == heat_before_abort);
	std::cout << "reserve exchange: two stages, rotating spares, data, pause/resume, replay, discard retries, abort passed\n";
}
