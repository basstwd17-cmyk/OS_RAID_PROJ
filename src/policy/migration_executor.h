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
		sim_time_type Enqueue_time = 0;
	};

	struct AbortSummary {
		uint64_t Task_count = 0;
		uint64_t Deferred_request_count = 0;
	};

	typedef std::function<io_request_id_type(const StripeCopyPlan&, bool is_write, uint64_t task_index)> SubmitCopyFunction;
	typedef std::function<bool(const StripeCopyPlan&, uint64_t task_index)> DiscardFunction;
	typedef std::function<void(const MigrationTask&, const SSD_Components::User_Request*)> BufferedWriteObserveFunction;

	MigrationExecutor();
	explicit MigrationExecutor(unsigned int buffer_limit_per_task);

	void Configure(unsigned int buffer_limit_per_task);
	void Configure_limits(const MigrationLimits& limits);
	const MigrationLimits& Limits() const { return limits; }
	void Start(const std::vector<MigrationTask>& tasks, ZoneDirectory& directory, sim_time_type now = 0);
	sim_time_type Next_wakeup(sim_time_type fallback) const;
	uint64_t Time_pauses() const { return time_pauses; }
	uint64_t Sector_pauses() const { return sector_pauses; }
	uint64_t Read_pauses() const { return read_pauses; }
	uint64_t Cold_stages_completed() const { return cold_stages_completed; }
	bool Has_inflight() const { return lazy.Active || !inflight.empty(); }
	bool On_demand() const { return limits.On_demand; }
	bool Replay_ready(const SSD_Components::User_Request* request) const;
	void Resolve_on_demand(ZoneResolveResult& resolved) const;
	void Notify_host_completed(const io_request_id_type& id);
	void Force_finish();
	uint64_t Lazy_offloaded_pages() const { return lazy_offloaded_pages; }
	uint64_t Lazy_copied_pages() const { return lazy_copied_pages; }
	uint64_t Lazy_deadline_drains() const { return lazy_deadline_drains; }
	uint64_t Lazy_cold_write_drains() const { return lazy_cold_write_drains; }
	uint64_t Lazy_remaining_pages() const;
	// A migration copy is issued one at a time per task.  The controller uses
	// this to wait for the completion callback instead of continuously polling
	// the simulator while flash I/O is in flight.
	bool Has_active_copy() const;
	// A request that intersects any active task must not be replayed against a
	// zone whose placement is still changing.
	bool Intersects_inflight_zones(const std::vector<uint64_t>& request_zone_ids) const;
	AbortSummary Abort_all(ZoneDirectory& directory);

	InterceptResult Maybe_intercept(SSD_Components::User_Request* request,
		const std::vector<uint64_t>& request_zone_ids,
		ZoneDirectory& directory,
		const BufferedWriteObserveFunction& observe_buffered_write = BufferedWriteObserveFunction(),
		sim_time_type enqueue_time = 0);
	bool Notify_request_completed(io_request_id_type request_id, ZoneDirectory& directory);
	void Poll(ZoneDirectory& directory,
		const SubmitCopyFunction& submit_copy,
		const DiscardFunction& discard_source, sim_time_type now = 0);

	std::vector<DeferredRequest> Drain_replay_requests();
	uint64_t Buffered_count() const;
	uint64_t Backpressure_events() const { return backpressure_events; }
	uint64_t Grabbed_blocks() const { return grabbed_blocks; }
	uint64_t Restored_blocks() const { return restored_blocks; }
	uint64_t Discarded_source_blocks() const { return discarded_source_blocks; }
	uint64_t Discarded_source_sectors() const { return discarded_source_sectors; }
	uint64_t Dirty_blocks() const { return dirty_blocks; }
	uint64_t Max_queue_depth() const { return max_queue_depth; }
	unsigned int Queue_limit() const { return buffer_limit_per_task; }
	uint64_t Queued_request_bytes() const;
	uint64_t Copy_buffer_bytes() const;
	uint64_t Peak_queued_request_bytes() const { return peak_queued_request_bytes; }
	uint64_t Peak_copy_buffer_bytes() const { return peak_copy_buffer_bytes; }
	uint64_t Peak_total_queue_depth() const { return peak_total_queue_depth; }
	std::function<void(uint64_t, bool)> Task_finished;

private:
	enum class LazyPhase { IDLE, READ, WRITE, DISCARD };
	struct LazyState {
		bool Active = false, Started = false, Cold = false, Force = false, Host_discard_ready = false;
		bool Deadline_counted = false;
		MigrationTask Task;
		uint64_t Target_zone = INVALID_ZONE_ID;
		sim_time_type Deadline = 0;
		unsigned int Page_sectors = 0, Page = 0;
		unsigned int Target_disk = 0;
		LHA_type Target_lba = 0;
		LazyPhase Phase = LazyPhase::IDLE;
		std::vector<bool> Moved;
		std::deque<unsigned int> Pending_host_discards;
		std::vector<StripeCopyPlan> Runs;
		size_t Run_index = 0;
		io_request_id_type Copy_id, Host_id, Preparing_id;
		std::deque<DeferredRequest> Requests;
	};
	LazyState lazy;
	uint64_t lazy_offloaded_pages = 0, lazy_copied_pages = 0;
	uint64_t lazy_deadline_drains = 0, lazy_cold_write_drains = 0;
	void Lazy_start(const std::vector<MigrationTask>& tasks, ZoneDirectory& directory, sim_time_type now);
	InterceptResult Lazy_intercept(SSD_Components::User_Request* request,
		const std::vector<uint64_t>& zones, ZoneDirectory& directory, sim_time_type now);
	void Lazy_poll(ZoneDirectory& directory, const SubmitCopyFunction& submit,
		const DiscardFunction& discard, sim_time_type now);
	bool Lazy_copy_completed(const io_request_id_type& id);
	StripeCopyPlan Lazy_page_plan(const ZoneDirectory& directory, unsigned int page) const;
	bool Lazy_prepare_page(ZoneDirectory& directory, unsigned int page);
	std::vector<unsigned int> Lazy_request_pages(const SSD_Components::User_Request* request,
		const ZoneDirectory& directory, bool full_only = false) const;
	bool Lazy_touches_cold(const SSD_Components::User_Request* request, const ZoneDirectory& directory) const;
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
		MigrationOp Original_op;
		std::vector<StripeCopyPlan> Hot_copies;
		std::vector<uint64_t> Locked_zones;
		bool Cold_stage = false;
		sim_time_type Slice_start = 0, Resume_at = 0;
		uint64_t Slice_sectors = 0;
		bool Issued_in_slice = false, Read_pause_pending = false;
		unsigned int Active_sectors = 0;
		MigrationTask Task;	// hot/cold 쌍 + 복사할 stripe 목록
		MigrationBuffer Buffer;
		std::vector<StripeCopyPlan> Restore_copies;
		std::map<stream_id_type, std::vector<RestoreBlockState>> Restore_block_states;
		std::map<stream_id_type, std::vector<bool>> Restore_after_active_write;
		std::map<stream_id_type, std::vector<bool>> Discarded_source_blocks;
		std::deque<std::pair<stream_id_type, unsigned int>> Pending_source_discards;
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
	void Discard_source_block(InflightTask& task, stream_id_type stream_id, unsigned int block_offset,
		const DiscardFunction& discard_source, uint64_t task_index);
	void Drain_pending_source_discards(InflightTask& task, const DiscardFunction& discard_source, uint64_t task_index);
	void Discard_source_copies(InflightTask& task, const DiscardFunction& discard_source, uint64_t task_index);
	bool Drain_task(InflightTask& task, ZoneDirectory& directory);
	void Initialize_stage(InflightTask& task, ZoneDirectory& directory);
	bool Pause_before_copy(InflightTask& task, sim_time_type now);
	MigrationLimits limits;
	uint64_t time_pauses = 0, sector_pauses = 0, read_pauses = 0, cold_stages_completed = 0;

	unsigned int buffer_limit_per_task;	// task 하나당 연기된 요청 최대치
	uint64_t backpressure_events;	// 버퍼한계 초과 카운터
	uint64_t grabbed_blocks;
	uint64_t restored_blocks;
	uint64_t discarded_source_blocks;
	uint64_t discarded_source_sectors;
	uint64_t dirty_blocks;
	uint64_t max_queue_depth;
	uint64_t peak_queued_request_bytes = 0;
	uint64_t peak_copy_buffer_bytes = 0;
	uint64_t peak_total_queue_depth = 0;
	uint64_t zone_bytes = 0;
	uint64_t block_bytes = 0;
	void Update_peaks();
	std::vector<InflightTask> inflight;	// 지금 진행 중인 migration 작업
	std::vector<DeferredRequest> replay_queue;	// migration 끝난 후 처리할 요청들
};

} // namespace RAID_Policy

#endif // RAID_SWANS_MIGRATION_EXECUTOR_H
