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
	zone_bytes = directory.Zone_size_lba() * SECTOR_SIZE_IN_BYTE;
	block_bytes = static_cast<uint64_t>(directory.Block_unit_lba()) * SECTOR_SIZE_IN_BYTE;
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
			ensure_block_bitmap(item.Discarded_source_blocks, stream_id, static_cast<size_t>(block_count));
		}
		item.Buffer.Valid = true;
		inflight.push_back(item);
		directory.Mark_migrating(tasks[i].Op.Hot_zone, true);
		directory.Mark_migrating(tasks[i].Op.Cold_zone, true);
	}
}

bool MigrationExecutor::Has_active_copy() const
{
	for (size_t i = 0; i < inflight.size(); i++) {
		if (!inflight[i].Active_request_id.empty()) {
			return true;
		}
	}
	return false;
}

bool MigrationExecutor::Intersects_inflight_zones(const std::vector<uint64_t>& request_zone_ids) const
{
	for (size_t i = 0; i < inflight.size(); i++) {
		if (Is_intercept_target(inflight[i], request_zone_ids)) {
			return true;
		}
	}
	return false;
}

MigrationExecutor::AbortSummary MigrationExecutor::Abort_all(ZoneDirectory& directory)
{
	AbortSummary summary;
	for (size_t i = 0; i < inflight.size(); i++) {
		InflightTask& task = inflight[i];
		summary.Task_count++;
		summary.Deferred_request_count += task.Deferred_requests.size();
		directory.Mark_migrating(task.Task.Op.Hot_zone, false);
		directory.Mark_migrating(task.Task.Op.Cold_zone, false);
		if (Task_finished) Task_finished(task.Task.Op.Hot_zone, true);
	}
	summary.Deferred_request_count += pending_reads.size();
	pending_reads.clear();
	inflight.clear();
	return summary;
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

MigrationExecutor::InterceptResult MigrationExecutor::Maybe_intercept(SSD_Components::User_Request* request,
	const std::vector<uint64_t>& request_zone_ids, sim_time_type enqueue_time)
{
	if (request == nullptr || !Intersects_inflight_zones(request_zone_ids)) {
		return InterceptResult::SUBMITTED;
	}

	DeferredRequest deferred;
	deferred.Request = request;
	deferred.Enqueue_time = enqueue_time;
	if (request->Type == SSD_Components::UserRequestType::READ) {
		// Reads have an independent queue: a full write cache must not prevent
		// a read from pausing the migration. Multi-zone reads pause all owners.
		for (auto& task : inflight) {
			if (Is_intercept_target(task, request_zone_ids)) {
				task.Priority_reads.insert(request->ID);
			}
		}
		pending_reads.push_back(deferred);
		Update_peaks();
		return InterceptResult::PRIORITY_READ;
	}

	for (auto& task : inflight) {
		if (!Is_intercept_target(task, request_zone_ids)) continue;
		if (task.Deferred_requests.size() >= buffer_limit_per_task) {
			backpressure_events++;
			return InterceptResult::BACKPRESSURE;
		}
		// Cache the original write without changing migration data or host
		// accounting. It will be submitted against the final mapping later.
		task.Deferred_requests.push_back(deferred);
		max_queue_depth = std::max<uint64_t>(max_queue_depth, task.Deferred_requests.size());
		Update_peaks();
		return InterceptResult::BUFFERED;
	}
	return InterceptResult::SUBMITTED;
}

std::vector<MigrationExecutor::DeferredRequest> MigrationExecutor::Drain_ready_reads()
{
	std::vector<DeferredRequest> ready;
	for (auto it = pending_reads.begin(); it != pending_reads.end();) {
		bool active_copy = false;
		for (const auto& task : inflight) {
			if (task.Priority_reads.count(it->Request->ID) && !task.Active_request_id.empty()) {
				active_copy = true;
				break;
			}
		}
		if (active_copy) { ++it; continue; }
		ready.push_back(*it);
		it = pending_reads.erase(it);
	}
	return ready;
}

bool MigrationExecutor::Is_priority_read(const io_request_id_type& request_id) const
{
	for (const auto& task : inflight) {
		if (task.Priority_reads.count(request_id)) return true;
	}
	return false;
}

bool MigrationExecutor::Has_priority_reads() const
{
	for (const auto& task : inflight) {
		if (!task.Priority_reads.empty()) return true;
	}
	return false;
}

void MigrationExecutor::Notify_read_completed(const io_request_id_type& request_id)
{
	for (auto& task : inflight) task.Priority_reads.erase(request_id);
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
			task.Next_restore++;
			restored_blocks++;
		}
		Update_peaks();
		return true;
	}
	return false;
}

void MigrationExecutor::Drain_task(InflightTask& task, ZoneDirectory& directory)
{
	directory.Complete_migration(task.Task.Op.Hot_zone, task.Task.Op.Cold_zone, task.Buffer.Dirty_blocks);

	while (!task.Deferred_requests.empty()) {
		replay_queue.push_back(task.Deferred_requests.front());
		task.Deferred_requests.pop_front();
	}
}

void MigrationExecutor::Discard_source_block(InflightTask& task, stream_id_type stream_id, unsigned int block_offset,
	const DiscardFunction& discard_source, uint64_t task_index)
{
	if (!discard_source) {
		return;
	}
	std::vector<bool>& discarded = task.Discarded_source_blocks[stream_id];
	if (block_offset >= discarded.size()) {
		discarded.resize(static_cast<size_t>(block_offset) + 1, false);
	}
	if (discarded[block_offset]) {
		return;
	}
	for (size_t i = 0; i < task.Task.Copies.size(); i++) {
		const StripeCopyPlan& copy = task.Task.Copies[i];
		if (copy.Stream_id != stream_id || copy.Stripe_offset != block_offset || copy.Lba_count == 0) {
			continue;
		}
		if (discard_source(copy, task_index)) {
			discarded[block_offset] = true;
			discarded_source_blocks++;
			discarded_source_sectors += copy.Lba_count;
		}
		return;
	}
}

void MigrationExecutor::Discard_source_copies(InflightTask& task, const DiscardFunction& discard_source, uint64_t task_index)
{
	if (!discard_source) {
		return;
	}
	for (size_t i = 0; i < task.Task.Copies.size(); i++) {
		const StripeCopyPlan& copy = task.Task.Copies[i];
		Discard_source_block(task, copy.Stream_id, copy.Stripe_offset, discard_source, task_index);
	}
}

void MigrationExecutor::Poll(ZoneDirectory& directory,
	const SubmitCopyFunction& submit_copy,
	const DiscardFunction& discard_source)
{
	for (size_t i = 0; i < inflight.size(); i++) {
		InflightTask& task = inflight[i];
		if (!task.Priority_reads.empty()) continue;
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
						task.State = TaskState::RESTORING;
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
				case TaskState::RESTORING:
					if (task.Next_restore >= task.Task.Copies.size()) {
						task.State = TaskState::DRAINING_QUEUE;
						advance_without_io = true;
						break;
					}
					if (task.Active_request_id.empty()) {
						const StripeCopyPlan& copy = task.Task.Copies[task.Next_restore];
						task.Active_request_id = submit_copy(copy, true, i);
						if (!task.Active_request_id.empty()) {
							task.Active_block_offset = copy.Stripe_offset;
							task.Active_stream_id = copy.Stream_id;
						}
					}
					break;
				case TaskState::DRAINING_QUEUE:
				{
					// Keep the source and its public mapping until all destination
					// writes finish. Paused reads always use this stable source.
					directory.Swap_placement(task.Task.Op.Hot_zone, task.Task.Op.Cold_zone);
					Discard_source_copies(task, discard_source, i);
					Drain_task(task, directory);
					task.State = TaskState::DONE;
					if (Task_finished) Task_finished(task.Task.Op.Hot_zone, false);
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
	uint64_t count = pending_reads.size();
	for (size_t i = 0; i < inflight.size(); i++) {
		count += inflight[i].Deferred_requests.size();
	}
	return count;
}

uint64_t MigrationExecutor::Queued_request_bytes() const
{
	uint64_t bytes = 0;
	for (const auto& task : inflight)
		for (const auto& queued : task.Deferred_requests)
			if (queued.Request) bytes += static_cast<uint64_t>(queued.Request->SizeInSectors) * SECTOR_SIZE_IN_BYTE;
	for (const auto& queued : pending_reads)
		bytes += static_cast<uint64_t>(queued.Request->SizeInSectors) * SECTOR_SIZE_IN_BYTE;
	return bytes;
}

uint64_t MigrationExecutor::Copy_buffer_bytes() const
{
	// Logical payload represented by bitmaps, not C++ allocation size.
	uint64_t bytes = 0;
	for (const auto& task : inflight) {
		if (task.State == TaskState::DONE) continue;
		std::map<stream_id_type, std::vector<bool>> occupied = task.Buffer.Grabbed_blocks;
		for (const auto& stream : task.Buffer.Dirty_blocks) {
			auto& bits = occupied[stream.first];
			bits.resize(std::max(bits.size(), stream.second.size()), false);
			for (size_t i = 0; i < stream.second.size(); ++i) bits[i] = bits[i] || stream.second[i];
		}
		for (const auto& stream : occupied)
			for (size_t i = 0; i < stream.second.size(); ++i)
				if (stream.second[i] && i * block_bytes < zone_bytes)
					bytes += std::min(block_bytes, zone_bytes - i * block_bytes);
	}
	return bytes;
}

void MigrationExecutor::Update_peaks()
{
	peak_queued_request_bytes = std::max(peak_queued_request_bytes, Queued_request_bytes());
	peak_copy_buffer_bytes = std::max(peak_copy_buffer_bytes, Copy_buffer_bytes());
	peak_total_queue_depth = std::max(peak_total_queue_depth, Buffered_count());
}

} // namespace RAID_Policy
