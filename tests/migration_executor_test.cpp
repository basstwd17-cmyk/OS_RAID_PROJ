#include <cassert>
#include <string>
#include <vector>

#include "../src/policy/migration_executor.h"

int main()
{
	RAID_Policy::ZoneDirectory directory;
	directory.Initialize(2, 512, 1024, 512, 4ULL * 1024ULL);
	directory.Observe_write(0, 0, 0, 512);
	directory.Observe_write(0, 0, 512, 512);

	RAID_Policy::MigrationTask task;
	task.Op.Hot_zone = 0;
	task.Op.Cold_zone = 1;
	task.Op.Hot_ssd = 0;
	task.Op.Cold_ssd = 1;
	task.Moved_write_count = directory.Zone_write_count(0);
	for (unsigned int block = 0; block < 2; block++) {
		RAID_Policy::StripeCopyPlan copy;
		copy.Stream_id = 0;
		copy.Source_disk_id = 0;
		copy.Source_lba = block * 512;
		copy.Destination_disk_id = 1;
		copy.Destination_lba = block * 512;
		copy.Lba_count = 512;
		copy.Stripe_offset = block;
		task.Copies.push_back(copy);
	}

	RAID_Policy::MigrationExecutor executor(64);
	executor.Start(std::vector<RAID_Policy::MigrationTask>(1, task), directory);
	assert(executor.Has_inflight());
	assert(directory.Is_migrating(0));
	assert(directory.Is_migrating(1));

	std::vector<bool> submitted_writes;
	unsigned int next_id = 1;
	unsigned int discard_count = 0;
	unsigned int completion_count = 0;
	uint64_t completed_write_count = 0;
	auto submit = [&](const RAID_Policy::StripeCopyPlan&, bool is_write, uint64_t) {
		submitted_writes.push_back(is_write);
		return std::string("io-") + std::to_string(next_id++);
	};
	auto discard = [&](const RAID_Policy::StripeCopyPlan&, uint64_t) {
		discard_count++;
		return true;
	};
	auto complete = [&](const RAID_Policy::MigrationTask&, uint64_t moved_write_count) {
		completion_count++;
		completed_write_count = moved_write_count;
	};

	executor.Poll(directory, submit, discard, complete);
	assert((submitted_writes == std::vector<bool>{false}));
	executor.Poll(directory, submit, discard, complete);
	assert(submitted_writes.size() == 1);
	assert(!executor.Notify_request_completed("wrong-id", directory));
	assert(executor.Notify_request_completed("io-1", directory));
	assert(!executor.Notify_request_completed("io-1", directory));

	executor.Poll(directory, submit, discard, complete);
	assert((submitted_writes == std::vector<bool>{false, false}));
	assert(executor.Notify_request_completed("io-2", directory));

	// The same Poll performs GRABBING -> SWAPPING -> RESTORING and submits write 1.
	executor.Poll(directory, submit, discard, complete);
	assert((submitted_writes == std::vector<bool>{false, false, true}));
	assert(directory.Owner_ssd(0) == 1);
	executor.Poll(directory, submit, discard, complete);
	assert(submitted_writes.size() == 3);
	assert(executor.Notify_request_completed("io-3", directory));

	executor.Poll(directory, submit, discard, complete);
	assert((submitted_writes == std::vector<bool>{false, false, true, true}));
	assert(executor.Notify_request_completed("io-4", directory));

	// The same Poll performs RESTORING -> DRAINING_QUEUE -> DONE.
	executor.Poll(directory, submit, discard, complete);
	assert(!executor.Has_inflight());
	assert(completion_count == 1);
	assert(completed_write_count == 2);
	assert(discard_count == 2);
	assert(executor.Grabbed_blocks() == 2);
	assert(executor.Restored_blocks() == 2);
	assert(executor.Discarded_source_blocks() == 2);
	assert(!directory.Is_migrating(0));
	assert(!directory.Is_migrating(1));
	assert(directory.Owner_ssd(0) == 1);
	assert(directory.Owner_ssd(1) == 0);

	return 0;
}
