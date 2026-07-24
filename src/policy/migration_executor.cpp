#include "migration_executor.h"

#include <algorithm>
#include <limits>

namespace RAID_Policy {

namespace {
	std::vector<bool>& ensure_block_bitmap(std::map<stream_id_type, std::vector<bool>>& by_stream,
		stream_id_type stream_id, size_t block_count)
	{
		std::vector<bool>& blocks = by_stream[stream_id];
		if (blocks.size() < block_count) {
			blocks.resize(block_count, false);
		}
		return blocks;
	}
}

	MigrationExecutor::MigrationExecutor()
		: buffer_limit_per_task(64),
		  backpressure_events(0),
		  grabbed_blocks(0),
		  restored_blocks(0),
		  discarded_source_blocks(0),
		  discarded_source_sectors(0),
		  dirty_blocks(0),
		  max_queue_depth(0)
{
}

	MigrationExecutor::MigrationExecutor(unsigned int buffer_limit_per_task)
		: buffer_limit_per_task(buffer_limit_per_task == 0 ? 1 : buffer_limit_per_task),
		  backpressure_events(0),
		  grabbed_blocks(0),
		  restored_blocks(0),
		  discarded_source_blocks(0),
		  discarded_source_sectors(0),
		  dirty_blocks(0),
		  max_queue_depth(0)
{
}

void MigrationExecutor::Configure(unsigned int buffer_limit_per_task)
{
	this->buffer_limit_per_task = buffer_limit_per_task == 0 ? 1 : buffer_limit_per_task;
}

void MigrationExecutor::Start(const std::vector<MigrationTask>& tasks, ZoneDirectory& directory)
{
	if (!inflight.empty()) {
		return;
	}
	for (size_t i = 0; i < tasks.size(); i++) {
		InflightTask item;
		item.Task = tasks[i];
		item.Next_grab = 0;
		item.Next_restore = 0;
		item.State = TaskState::GRABBING;
		item.Active_request_id.clear();
		item.Active_block_offset = 0;
		item.Active_stream_id = 0;
		item.Buffer.Logical_zone = tasks[i].Op.Hot_zone;
		item.Buffer.Source_ssd = tasks[i].Op.Hot_ssd;
		item.Buffer.Target_ssd = tasks[i].Op.Cold_ssd;
		const uint64_t zone_size_lba = directory.Zone_size_lba();
		const unsigned int block_unit_lba = directory.Block_unit_lba();
		const uint64_t block_count = block_unit_lba == 0 ? 0 : (zone_size_lba + block_unit_lba - 1) / block_unit_lba;
		for (size_t copy_index = 0; copy_index < tasks[i].Copies.size(); copy_index++) {
			const stream_id_type stream_id = tasks[i].Copies[copy_index].Stream_id;
			ensure_block_bitmap(item.Buffer.Grabbed_blocks, stream_id, static_cast<size_t>(block_count));
			ensure_block_bitmap(item.Buffer.Dirty_blocks, stream_id, static_cast<size_t>(block_count));
			item.Restore_block_states[stream_id].assign(static_cast<size_t>(block_count), RestoreBlockState::NOT_SCHEDULED);
			item.Restore_after_active_write[stream_id].assign(static_cast<size_t>(block_count), false);
		}
		item.Buffer.Valid = true;
		inflight.push_back(item);
		directory.Mark_migrating(tasks[i].Op.Hot_zone, true);
		directory.Mark_migrating(tasks[i].Op.Cold_zone, true);
	}
}

bool MigrationExecutor::Is_intercept_target(const InflightTask& task, const std::vector<uint64_t>& request_zone_ids) const
{
	for (size_t i = 0; i < request_zone_ids.size(); i++) {
		if (request_zone_ids[i] == task.Task.Op.Hot_zone || request_zone_ids[i] == task.Task.Op.Cold_zone) {
			return true;
		}
	}
	return false;
}

bool MigrationExecutor::Hits_zone(uint64_t zone_id, const std::vector<uint64_t>& request_zone_ids) const
{
	return std::find(request_zone_ids.begin(), request_zone_ids.end(), zone_id) != request_zone_ids.end();
}

bool MigrationExecutor::Hits_only_zone(uint64_t zone_id, const std::vector<uint64_t>& request_zone_ids) const
{
	if (request_zone_ids.empty()) {
		return false;
	}
	for (size_t i = 0; i < request_zone_ids.size(); i++) {
		if (request_zone_ids[i] != zone_id) {
			return false;
		}
	}
	return true;
}

void MigrationExecutor::Mark_dirty_blocks(InflightTask& task,
	const SSD_Components::User_Request* request,
	ZoneDirectory& directory)
{
	if (request == nullptr || request->SizeInSectors == 0 || !task.Buffer.Valid) {
		return;
	}
	const uint64_t zone_size_lba = directory.Zone_size_lba();
	const unsigned int block_unit_lba = directory.Block_unit_lba();
	if (zone_size_lba == 0 || block_unit_lba == 0) {
		return;
	}

	const size_t block_count = static_cast<size_t>((zone_size_lba + block_unit_lba - 1) / block_unit_lba);
	std::vector<bool>& dirty_bitmap = ensure_block_bitmap(task.Buffer.Dirty_blocks, request->Stream_id, block_count);
	if (task.Restore_block_states[request->Stream_id].size() < block_count) {
		task.Restore_block_states[request->Stream_id].resize(block_count, RestoreBlockState::NOT_SCHEDULED);
	}
	if (task.Restore_after_active_write[request->Stream_id].size() < block_count) {
		task.Restore_after_active_write[request->Stream_id].resize(block_count, false);
	}

	LHA_type current_lba = request->Start_LBA;
	unsigned int remaining = request->SizeInSectors;
	while (remaining > 0) {
		ZoneResolveResult resolved;
		directory.Resolve(current_lba, resolved);
		const uint64_t stripe_remaining = directory.Stripe_unit_lba() > resolved.In_stripe_offset
			? directory.Stripe_unit_lba() - resolved.In_stripe_offset : 1;
		const uint64_t zone_remaining = zone_size_lba > resolved.Zone_lba_offset
			? zone_size_lba - resolved.Zone_lba_offset : 1;
		const unsigned int chunk = static_cast<unsigned int>(std::min<uint64_t>(remaining,
			std::min<uint64_t>(stripe_remaining, zone_remaining)));

		if (resolved.Zone_id == task.Task.Op.Hot_zone && chunk > 0) {
			const uint64_t start_block = resolved.Zone_lba_offset / block_unit_lba;
			const uint64_t end_block = (resolved.Zone_lba_offset + chunk - 1) / block_unit_lba;
			for (uint64_t block = start_block; block <= end_block && block < dirty_bitmap.size(); block++) {
				if (!dirty_bitmap[static_cast<size_t>(block)]) {
					dirty_bitmap[static_cast<size_t>(block)] = true;
					dirty_blocks++;
				}
				if (task.State == TaskState::RESTORING) {
					Request_restore_after_dirty(task, request->Stream_id, static_cast<unsigned int>(block), directory);
				}
			}
		}

		const unsigned int advance = chunk == 0 ? 1 : chunk;
		current_lba += advance;
		remaining -= std::min(remaining, advance);
	}
}

MigrationExecutor::InterceptResult MigrationExecutor::Maybe_intercept(SSD_Components::User_Request* request,
	const std::vector<uint64_t>& request_zone_ids,
	ZoneDirectory& directory,
	const BufferedWriteObserveFunction& observe_buffered_write)
{
	if (request == nullptr) {
		return InterceptResult::SUBMITTED;
	}

	for (size_t i = 0; i < inflight.size(); i++) {
		InflightTask& task = inflight[i];
		if (!Is_intercept_target(task, request_zone_ids)) {
			continue;
		}
		if (task.Deferred_requests.size() >= buffer_limit_per_task) {
			backpressure_events++;
			return InterceptResult::BACKPRESSURE;
		}
		DeferredRequest deferred;
		deferred.Request = request;
		deferred.Complete_without_dispatch = false;
		if (request->Type == SSD_Components::UserRequestType::WRITE && Hits_zone(task.Task.Op.Hot_zone, request_zone_ids)) {
			Mark_dirty_blocks(task, request, directory);
			deferred.Complete_without_dispatch = Hits_only_zone(task.Task.Op.Hot_zone, request_zone_ids);
			if (deferred.Complete_without_dispatch && observe_buffered_write) {
				observe_buffered_write(task.Task, request);
			}
		}
		task.Deferred_requests.push_back(deferred);
		max_queue_depth = std::max<uint64_t>(max_queue_depth, task.Deferred_requests.size());
		return InterceptResult::BUFFERED;
	}
	return InterceptResult::SUBMITTED;
}

bool MigrationExecutor::Notify_request_completed(io_request_id_type request_id, ZoneDirectory& directory)
{
	for (size_t i = 0; i < inflight.size(); i++) {
		InflightTask& task = inflight[i];
		if (task.Active_request_id != request_id) {
			continue;
		}

		task.Active_request_id.clear();
		if (task.State == TaskState::GRABBING) {
			std::vector<bool>& grabbed_blocks_for_stream = task.Buffer.Grabbed_blocks[task.Active_stream_id];
			if (task.Active_block_offset < grabbed_blocks_for_stream.size()
				&& !grabbed_blocks_for_stream[task.Active_block_offset]) {
				grabbed_blocks_for_stream[task.Active_block_offset] = true;
				grabbed_blocks++;
			}
			task.Next_grab++;
		} else if (task.State == TaskState::RESTORING) {
			std::vector<RestoreBlockState>& restore_states = task.Restore_block_states[task.Active_stream_id];
			std::vector<bool>& restore_after_active_write = task.Restore_after_active_write[task.Active_stream_id];
			if (task.Active_block_offset < restore_states.size()) {
				if (task.Active_block_offset < restore_after_active_write.size()
					&& restore_after_active_write[task.Active_block_offset]) {
					restore_after_active_write[task.Active_block_offset] = false;
					Append_restore_copy(task, task.Active_stream_id, task.Active_block_offset, directory);
				} else {
					restore_states[task.Active_block_offset] = RestoreBlockState::COMPLETED;
				}
			}
			task.Next_restore++;
			restored_blocks++;
		}
		return true;
	}
	return false;
}

void MigrationExecutor::Append_restore_copy(InflightTask& task, stream_id_type stream_id, unsigned int block_offset, const ZoneDirectory& directory)
{
	const uint64_t zone_size_lba = directory.Zone_size_lba();
	const unsigned int block_unit_lba = directory.Block_unit_lba();
	if (zone_size_lba == 0 || block_unit_lba == 0) {
		return;
	}
	const size_t block_count = static_cast<size_t>((zone_size_lba + block_unit_lba - 1) / block_unit_lba);
	if (block_offset >= block_count) {
		return;
	}
	if (task.Restore_block_states[stream_id].size() < block_count) {
		task.Restore_block_states[stream_id].resize(block_count, RestoreBlockState::NOT_SCHEDULED);
	}
	const uint64_t zone_lba_offset = static_cast<uint64_t>(block_offset) * block_unit_lba;
	if (zone_lba_offset >= zone_size_lba) {
		return;
	}
	const uint64_t copy_lba_count_64 = std::min<uint64_t>(block_unit_lba, zone_size_lba - zone_lba_offset);
	if (copy_lba_count_64 == 0 || copy_lba_count_64 > std::numeric_limits<unsigned int>::max()) {
		return;
	}

	StripeCopyPlan copy;
	copy.Stream_id = stream_id;
	copy.Stripe_offset = block_offset;
	copy.Lba_count = static_cast<unsigned int>(copy_lba_count_64);
	directory.Resolve_zone_lba(task.Task.Op.Hot_zone, zone_lba_offset, copy.Destination_disk_id, copy.Destination_lba);
	task.Restore_copies.push_back(copy);
	task.Restore_block_states[stream_id][block_offset] = RestoreBlockState::SCHEDULED;
}

void MigrationExecutor::Request_restore_after_dirty(InflightTask& task, stream_id_type stream_id, unsigned int block_offset, const ZoneDirectory& directory)
{
	const uint64_t zone_size_lba = directory.Zone_size_lba();
	const unsigned int block_unit_lba = directory.Block_unit_lba();
	const size_t block_count = block_unit_lba == 0 ? 0 : static_cast<size_t>((zone_size_lba + block_unit_lba - 1) / block_unit_lba);
	if (block_offset >= block_count) {
		return;
	}
	if (task.Restore_block_states[stream_id].size() < block_count) {
		task.Restore_block_states[stream_id].resize(block_count, RestoreBlockState::NOT_SCHEDULED);
	}
	if (task.Restore_after_active_write[stream_id].size() < block_count) {
		task.Restore_after_active_write[stream_id].resize(block_count, false);
	}
	switch (task.Restore_block_states[stream_id][block_offset]) {
		case RestoreBlockState::NOT_SCHEDULED:
		case RestoreBlockState::COMPLETED:
			Append_restore_copy(task, stream_id, block_offset, directory);
			break;
		case RestoreBlockState::SCHEDULED:
			break;
		case RestoreBlockState::IN_FLIGHT:
			task.Restore_after_active_write[stream_id][block_offset] = true;
			break;
	}
}

void MigrationExecutor::Build_restore_copies(InflightTask& task, ZoneDirectory& directory)
{
	task.Restore_copies.clear();
	const uint64_t zone_size_lba = directory.Zone_size_lba();
	const unsigned int block_unit_lba = directory.Block_unit_lba();
	if (zone_size_lba == 0 || block_unit_lba == 0) {
		return;
	}
	const size_t zone_block_count = static_cast<size_t>((zone_size_lba + block_unit_lba - 1) / block_unit_lba);
	task.Restore_copies.reserve(task.Task.Copies.size());
	task.Restore_block_states.clear();
	task.Restore_after_active_write.clear();
	std::map<stream_id_type, std::vector<bool>> merged_by_stream = task.Buffer.Grabbed_blocks;
	for (std::map<stream_id_type, std::vector<bool>>::const_iterator dirty_it = task.Buffer.Dirty_blocks.begin();
		dirty_it != task.Buffer.Dirty_blocks.end(); ++dirty_it) {
		std::vector<bool>& merged = merged_by_stream[dirty_it->first];
		if (merged.size() < dirty_it->second.size()) {
			merged.resize(dirty_it->second.size(), false);
		}
		for (size_t block = 0; block < dirty_it->second.size(); block++) {
			merged[block] = merged[block] || dirty_it->second[block];
		}
	}
	for (std::map<stream_id_type, std::vector<bool>>::const_iterator stream_it = merged_by_stream.begin();
		stream_it != merged_by_stream.end(); ++stream_it) {
		const stream_id_type stream_id = stream_it->first;
		task.Restore_block_states[stream_id].assign(zone_block_count, RestoreBlockState::NOT_SCHEDULED);
		task.Restore_after_active_write[stream_id].assign(zone_block_count, false);
		for (size_t block = 0; block < stream_it->second.size(); block++) {
			if (!stream_it->second[block]) {
				continue;
			}
			const uint64_t zone_lba_offset = static_cast<uint64_t>(block) * block_unit_lba;
			if (zone_lba_offset >= zone_size_lba) {
				continue;
			}
			Append_restore_copy(task, stream_id, static_cast<unsigned int>(block), directory);
		}
	}
}

void MigrationExecutor::Drain_task(InflightTask& task, ZoneDirectory& directory)
{
	directory.Complete_migration(task.Task.Op.Hot_zone, task.Task.Op.Cold_zone, task.Buffer.Dirty_blocks);

	while (!task.Deferred_requests.empty()) {
		replay_queue.push_back(task.Deferred_requests.front());
		task.Deferred_requests.pop_front();
	}
}

void MigrationExecutor::Discard_source_copies(InflightTask& task, const DiscardFunction& discard_source, uint64_t task_index)
{
	if (!discard_source) {
		return;
	}
	for (size_t i = 0; i < task.Task.Copies.size(); i++) {
		const StripeCopyPlan& copy = task.Task.Copies[i];
		if (copy.Lba_count == 0) {
			continue;
		}
		if (discard_source(copy, task_index)) {
			discarded_source_blocks++;
			discarded_source_sectors += copy.Lba_count;
		}
	}
}

void MigrationExecutor::Poll(ZoneDirectory& directory,
	const SubmitCopyFunction& submit_copy,
	const DiscardFunction& discard_source,
	const CompletionFunction& complete_migration)
{
	for (size_t i = 0; i < inflight.size(); i++) {
		InflightTask& task = inflight[i];
		bool advance_without_io = true;
		while (advance_without_io) {
			advance_without_io = false;
			switch (task.State) {
				case TaskState::IDLE:
					task.State = TaskState::GRABBING;
					advance_without_io = true;
					break;
				case TaskState::GRABBING:
					if (task.Next_grab >= task.Task.Copies.size()) {
						task.State = TaskState::SWAPPING;
						advance_without_io = true;
						break;
					}
					if (task.Active_request_id.empty()) {
						const StripeCopyPlan& copy = task.Task.Copies[task.Next_grab];
						task.Active_request_id = submit_copy(copy, false, i);
						if (!task.Active_request_id.empty()) {
							task.Active_block_offset = copy.Stripe_offset;
							task.Active_stream_id = copy.Stream_id;
						}
					}
					break;
				case TaskState::SWAPPING:
					directory.Swap_placement(task.Task.Op.Hot_zone, task.Task.Op.Cold_zone);
					Build_restore_copies(task, directory);
					task.Next_restore = 0;
					task.State = TaskState::RESTORING;
					advance_without_io = true;
					break;
				case TaskState::RESTORING:
					if (task.Next_restore >= task.Restore_copies.size()) {
						task.State = TaskState::DRAINING_QUEUE;
						advance_without_io = true;
						break;
					}
					if (task.Active_request_id.empty()) {
						const StripeCopyPlan& copy = task.Restore_copies[task.Next_restore];
						task.Active_request_id = submit_copy(copy, true, i);
						if (!task.Active_request_id.empty()) {
							task.Active_block_offset = copy.Stripe_offset;
							task.Active_stream_id = copy.Stream_id;
							std::vector<RestoreBlockState>& restore_states = task.Restore_block_states[copy.Stream_id];
							if (task.Active_block_offset < restore_states.size()) {
								restore_states[task.Active_block_offset] = RestoreBlockState::IN_FLIGHT;
							}
						}
					}
					break;
				case TaskState::DRAINING_QUEUE:
				{
					const uint64_t moved_write_count = task.Task.Moved_write_count;
					Discard_source_copies(task, discard_source, i);
					Drain_task(task, directory);
					if (complete_migration) {
						complete_migration(task.Task, moved_write_count);
					}
					task.State = TaskState::DONE;
					break;
				}
				case TaskState::DONE:
					break;
			}
		}
	}

	inflight.erase(std::remove_if(inflight.begin(), inflight.end(),
		[](const InflightTask& task) { return task.State == TaskState::DONE; }),
		inflight.end());
}

std::vector<MigrationExecutor::DeferredRequest> MigrationExecutor::Drain_replay_requests()
{
	std::vector<DeferredRequest> out;
	out.swap(replay_queue);
	return out;
}

uint64_t MigrationExecutor::Buffered_count() const
{
	uint64_t count = 0;
	for (size_t i = 0; i < inflight.size(); i++) {
		count += inflight[i].Deferred_requests.size();
	}
	return count;
}

} // namespace RAID_Policy
