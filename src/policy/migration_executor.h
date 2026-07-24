#ifndef RAID_SWANS_MIGRATION_EXECUTOR_H
#define RAID_SWANS_MIGRATION_EXECUTOR_H

#include <deque>
#include <functional>
#include <map>
#include <vector>
#include "swans_policy_types.h"
#include "zone_directory.h"
#include "../ssd/User_Request.h"

namespace RAID_Policy {

class MigrationExecutor
{
public:
	enum class InterceptResult {
		SUBMITTED,
		BUFFERED,
		BACKPRESSURE
	};

	struct DeferredRequest {
		SSD_Components::User_Request* Request = nullptr;
		bool Complete_without_dispatch = false;
	};

	typedef std::function<io_request_id_type(const StripeCopyPlan&, bool is_write, uint64_t task_index)> SubmitCopyFunction;
	typedef std::function<bool(const StripeCopyPlan&, uint64_t task_index)> DiscardFunction;
	typedef std::function<void(const MigrationTask&, uint64_t moved_write_count)> CompletionFunction;
	typedef std::function<void(const MigrationTask&, const SSD_Components::User_Request*)> BufferedWriteObserveFunction;

	MigrationExecutor();
	explicit MigrationExecutor(unsigned int buffer_limit_per_task);

	void Configure(unsigned int buffer_limit_per_task);
	void Start(const std::vector<MigrationTask>& tasks, ZoneDirectory& directory);
	bool Has_inflight() const { return !inflight.empty(); }

	InterceptResult Maybe_intercept(SSD_Components::User_Request* request,
		const std::vector<uint64_t>& request_zone_ids,
		ZoneDirectory& directory,
		const BufferedWriteObserveFunction& observe_buffered_write = BufferedWriteObserveFunction());
	bool Notify_request_completed(io_request_id_type request_id, ZoneDirectory& directory);
	void Poll(ZoneDirectory& directory,
		const SubmitCopyFunction& submit_copy,
		const DiscardFunction& discard_source,
		const CompletionFunction& complete_migration);

	std::vector<DeferredRequest> Drain_replay_requests();
	uint64_t Buffered_count() const;
	uint64_t Backpressure_events() const { return backpressure_events; }
	uint64_t Grabbed_blocks() const { return grabbed_blocks; }
	uint64_t Restored_blocks() const { return restored_blocks; }
	uint64_t Discarded_source_blocks() const { return discarded_source_blocks; }
	uint64_t Discarded_source_sectors() const { return discarded_source_sectors; }
	uint64_t Dirty_blocks() const { return dirty_blocks; }
	uint64_t Max_queue_depth() const { return max_queue_depth; }

private:
	enum class TaskState {
		IDLE,
		GRABBING,
		SWAPPING,
		RESTORING,
		DRAINING_QUEUE,
		DONE
	};

	enum class RestoreBlockState {
		NOT_SCHEDULED,
		SCHEDULED,
		IN_FLIGHT,
		COMPLETED
	};

	struct InflightTask {
		MigrationTask Task;	// hot/cold 쌍 + 복사할 stripe 목록
		MigrationBuffer Buffer;
		std::vector<StripeCopyPlan> Restore_copies;
		std::map<stream_id_type, std::vector<RestoreBlockState>> Restore_block_states;
		std::map<stream_id_type, std::vector<bool>> Restore_after_active_write;
		size_t Next_grab = 0;
		size_t Next_restore = 0;
		TaskState State = TaskState::IDLE;
		io_request_id_type Active_request_id;
		unsigned int Active_block_offset = 0;
		stream_id_type Active_stream_id = 0;
		std::deque<DeferredRequest> Deferred_requests;
	};		

	bool Is_intercept_target(const InflightTask& task, const std::vector<uint64_t>& request_zone_ids) const;
	bool Hits_zone(uint64_t zone_id, const std::vector<uint64_t>& request_zone_ids) const;
	bool Hits_only_zone(uint64_t zone_id, const std::vector<uint64_t>& request_zone_ids) const;
	void Mark_dirty_blocks(InflightTask& task, const SSD_Components::User_Request* request, ZoneDirectory& directory);
	void Append_restore_copy(InflightTask& task, stream_id_type stream_id, unsigned int block_offset, const ZoneDirectory& directory);
	void Request_restore_after_dirty(InflightTask& task, stream_id_type stream_id, unsigned int block_offset, const ZoneDirectory& directory);
	void Build_restore_copies(InflightTask& task, ZoneDirectory& directory);
	void Discard_source_copies(InflightTask& task, const DiscardFunction& discard_source, uint64_t task_index);
	void Drain_task(InflightTask& task, ZoneDirectory& directory);

	unsigned int buffer_limit_per_task;	// task 하나당 연기된 요청 최대치
	uint64_t backpressure_events;	// 버퍼한계 초과 카운터
	uint64_t grabbed_blocks;
	uint64_t restored_blocks;
	uint64_t discarded_source_blocks;
	uint64_t discarded_source_sectors;
	uint64_t dirty_blocks;
	uint64_t max_queue_depth;
	std::vector<InflightTask> inflight;	// 지금 진행 중인 migration 작업
	std::vector<DeferredRequest> replay_queue;	// migration 끝난 후 처리할 요청들
};

} // namespace RAID_Policy

#endif // RAID_SWANS_MIGRATION_EXECUTOR_H
