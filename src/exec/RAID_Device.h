#ifndef RAID_DEVICE_H
#define RAID_DEVICE_H

#include <vector>
#include <fstream>
#include "../sim/Sim_Object.h"
#include "../sim/Sim_Reporter.h"
#include "../ssd/Host_Interface_Base.h"
#include "../host/PCIe_Switch.h"
#include "../utils/Workload_Statistics.h"
#include "Device_Parameter_Set.h"
#include "IO_Flow_Parameter_Set.h"
#include "RAID_Controller.h"
#include "SSD_Device.h"
#include "Storage_Device.h"

class RAID_Device : public MQSimEngine::Sim_Object, public MQSimEngine::Sim_Reporter, public Storage_Device
{
public:
	RAID_Device(Device_Parameter_Set* parameters, std::vector<IO_Flow_Parameter_Set*>* io_flows);
	~RAID_Device();

	SSD_Components::Host_Interface_Base* Host_interface;
	SSD_Components::Host_Interface_Base* Get_host_interface() override { return Host_interface; }

	void Attach_to_host(Host_Components::PCIe_Switch* pcie_switch) override;
	void Perform_preconditioning(std::vector<Utils::Workload_Statistics*> workload_stats) override;
	void Initialize_io_streams(const std::vector<Host_Components::IO_Flow_Base*>& io_flows,
		Host_Components::SATA_HBA* sata_hba) override;
	void Start_simulation();
	void Configure_telemetry_output(const std::string& prefix);
	void Validate_simulation_config();
	void Execute_simulator_event(MQSimEngine::Sim_Event* event);
	void Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter) override;
	unsigned int Get_no_of_LHAs_in_an_NVM_write_unit() override;
	LPA_type Convert_host_logical_address_to_device_address(LHA_type lha) override;
	page_status_type Find_NVM_subunit_access_bitmap(LHA_type lha) override;

private:
	struct SSD_Observation {
		uint64_t Programs = 0, Erases = 0, Bad_blocks = 0, Host_bytes = 0, Programmed_bytes = 0;
		double Block_program_SD = 0, Block_erase_SD = 0;
	};
	struct Snapshot {
		sim_time_type Time = 0;
		RAID_Controller::Observation Host{};
		std::vector<SSD_Observation> SSDs;
	};
	bool telemetry_enabled = false, eol_captured = false;
	sim_time_type telemetry_period = 0, next_telemetry_time = 0;
	uint64_t telemetry_samples = 0;
	std::string telemetry_path;
	std::ofstream telemetry;
	Snapshot eol_snapshot;
	Snapshot Capture_snapshot(sim_time_type time, bool detailed) const;
	void Emit_snapshot(const std::string& event, uint64_t migration, const Snapshot& sample);
	void Write_snapshot_XML(const std::string& tag, const Snapshot& sample, Utils::XmlWriter& xmlwriter) const;
	unsigned int ssd_count;
	unsigned int stripe_unit_lba;
	std::vector<Device_Parameter_Set> ssd_configs;
	std::vector<SSD_Device*> ssds;
	RAID_Controller* raid_controller;
};

#endif
