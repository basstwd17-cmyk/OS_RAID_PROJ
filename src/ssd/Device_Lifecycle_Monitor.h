#ifndef DEVICE_LIFECYCLE_MONITOR_H
#define DEVICE_LIFECYCLE_MONITOR_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "../sim/Sim_Defs.h"

namespace SSD_Components
{
	struct Device_Lifecycle_Status
	{
		bool Triggered;
		std::string Device_id;
		sim_time_type Time;
		uint64_t Bad_block_count;
		uint64_t Total_block_count;
		uint64_t Remaining_usable_blocks;
		double Remaining_op_ratio;
		uint64_t Replay_round;

		Device_Lifecycle_Status()
			: Triggered(false), Device_id(""), Time(0), Bad_block_count(0), Total_block_count(0),
			  Remaining_usable_blocks(0), Remaining_op_ratio(0.0), Replay_round(0)
		{
		}
	};

	// Stores the first SSD that reaches EOL. Trace flows use this signal to stop
	// generating new requests while already-issued simulator events drain normally.
	class Device_Lifecycle_Monitor
	{
	public:
		static void Reset();
		static bool Report_end_of_life(const std::string& device_id, sim_time_type time,
			uint64_t bad_block_count, uint64_t total_block_count,
			uint64_t remaining_usable_blocks, double remaining_op_ratio);
		static bool Has_reached_end_of_life();
		static Device_Lifecycle_Status Get_status();
		// A trace flow records the replay round that first observes an EOL signal.
		// Zero means that EOL was not observed from an EOL-repeat trace flow.
		static void Record_eol_replay_round(uint64_t replay_round);
		// Components that own long-running work can schedule a terminal cleanup
		// event when an SSD reports EOL.
		static void Register_end_of_life_handler(const std::function<void()>& handler);

	private:
		static Device_Lifecycle_Status status;
		static std::vector<std::function<void()>> end_of_life_handlers;
	};
}

#endif // !DEVICE_LIFECYCLE_MONITOR_H
