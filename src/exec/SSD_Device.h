#ifndef SSD_DEVICE_H
#define SSD_DEVICE_H

#include <string>
#include <vector>
#include "../sim/Sim_Object.h"
#include "../sim/Sim_Reporter.h"
#include "../ssd/SSD_Defs.h"
#include "../ssd/Host_Interface_Base.h"
#include "../ssd/Host_Interface_SATA.h"
#include "../ssd/Host_Interface_NVMe.h"
#include "../ssd/Data_Cache_Manager_Base.h"
#include "../ssd/Data_Cache_Flash.h"
#include "../ssd/NVM_Firmware.h"
#include "../ssd/NVM_PHY_Base.h"
#include "../ssd/NVM_Channel_Base.h"
#include "../host/PCIe_Switch.h"
#include "../nvm_chip/NVM_Types.h"
#include "Device_Parameter_Set.h"
#include "IO_Flow_Parameter_Set.h"
#include "../utils/Workload_Statistics.h"
#include "Storage_Device.h"

/*********************************************************************************************************
* An SSD device has the following components:
* 
* Host_Interface <---> Data_Cache_Manager <----> NVM_Firmware <---> NVM_PHY <---> NVM_Channel <---> Chips
*
*********************************************************************************************************/

class SSD_Device : public MQSimEngine::Sim_Object, public MQSimEngine::Sim_Reporter, public Storage_Device
{
public:
	SSD_Device(Device_Parameter_Set* parameters, std::vector<IO_Flow_Parameter_Set*>* io_flows, const std::string& id = "SSDDevice");
	~SSD_Device();
	bool Preconditioning_required;
	NVM::NVM_Type Memory_Type;
	SSD_Components::Host_Interface_Base *Host_interface;
	SSD_Components::Data_Cache_Manager_Base *Cache_manager;
	SSD_Components::NVM_Firmware* Firmware;
	SSD_Components::NVM_PHY_Base* PHY;
	std::vector<SSD_Components::NVM_Channel_Base*> Channels;
	void Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter) override;
	unsigned int Get_no_of_LHAs_in_an_NVM_write_unit() override;

	SSD_Components::Host_Interface_Base* Get_host_interface() override { return Host_interface; }
	void Attach_to_host(Host_Components::PCIe_Switch* pcie_switch) override;
	void Perform_preconditioning(std::vector<Utils::Workload_Statistics*> workload_stats) override;
	void Initialize_io_streams(const std::vector<Host_Components::IO_Flow_Base*>& io_flows,
		Host_Components::SATA_HBA* sata_hba) override;
	void Start_simulation();
	void Validate_simulation_config();
	void Execute_simulator_event(MQSimEngine::Sim_Event* event);
	LPA_type Convert_host_logical_address_to_device_address(LHA_type lha) override;
	page_status_type Find_NVM_subunit_access_bitmap(LHA_type lha) override;

	unsigned int Channel_count;
	unsigned int Chip_no_per_channel;

private:
};

#endif //!SSD_DEVICE_H
