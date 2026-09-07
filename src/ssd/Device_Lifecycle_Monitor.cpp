#include "Device_Lifecycle_Monitor.h"

namespace SSD_Components
{
	Device_Lifecycle_Status Device_Lifecycle_Monitor::status;
	std::function<void()> Device_Lifecycle_Monitor::end_of_life_handler;

	void Device_Lifecycle_Monitor::Reset()
	{
		status = Device_Lifecycle_Status();
		end_of_life_handler = std::function<void()>();
	}

	bool Device_Lifecycle_Monitor::Report_end_of_life(const std::string& device_id, sim_time_type time,
		uint64_t bad_block_count, uint64_t total_block_count,
		uint64_t remaining_usable_blocks, double remaining_op_ratio)
	{
		if (status.Triggered) {
			return false;
		}

		status.Triggered = true;
		status.Device_id = device_id;
		status.Time = time;
		status.Bad_block_count = bad_block_count;
		status.Total_block_count = total_block_count;
		status.Remaining_usable_blocks = remaining_usable_blocks;
		status.Remaining_op_ratio = remaining_op_ratio;

		PRINT_MESSAGE("*** " << device_id << " reached END OF LIFE at " << time
			<< ": remaining OP ratio=" << remaining_op_ratio
			<< ", bad blocks=" << bad_block_count << " ***")
		if (end_of_life_handler) {
			end_of_life_handler();
		}
		return true;
	}

	bool Device_Lifecycle_Monitor::Has_reached_end_of_life()
	{
		return status.Triggered;
	}

	Device_Lifecycle_Status Device_Lifecycle_Monitor::Get_status()
	{
		return status;
	}

	void Device_Lifecycle_Monitor::Record_eol_replay_round(uint64_t replay_round)
	{
		if (status.Triggered && status.Replay_round == 0 && replay_round > 0) {
			status.Replay_round = replay_round;
		}
	}

	void Device_Lifecycle_Monitor::Register_end_of_life_handler(const std::function<void()>& handler)
	{
		end_of_life_handler = handler;
	}
}
