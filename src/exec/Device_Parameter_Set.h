#ifndef DEVICE_PARAMETER_SET_H
#define DEVICE_PARAMETER_SET_H

#include "../ssd/SSD_Defs.h"
#include "../ssd/Host_Interface_Defs.h"
#include "../ssd/Host_Interface_Base.h"
#include "../ssd/Data_Cache_Manager_Base.h"
#include "../ssd/Address_Mapping_Unit_Base.h"
#include "../ssd/TSU_Base.h"
#include "../ssd/ONFI_Channel_Base.h"
#include "../ssd/GC_and_WL_Unit_Page_Level.h"
#include "../nvm_chip/NVM_Types.h"
#include "Parameter_Set_Base.h"
#include "Flash_Parameter_Set.h"
#include <string>

// RAID parameters
class Device_Parameter_Set : public Parameter_Set_Base
{
public:
	Device_Parameter_Set();
	int Seed;//Seed for random number generation (used in device's random number generators)
	bool Enabled_Preconditioning;
	NVM::NVM_Type Memory_Type;
	HostInterface_Types HostInterface_Type;
	uint16_t IO_Queue_Depth;//For NVMe, it determines the size of the submission/completion queues; for SATA, it determines the size of NCQ_Control_Structure
	uint16_t Queue_Fetch_Size;//Used in NVMe host interface
	SSD_Components::Caching_Mechanism Caching_Mechanism;
	SSD_Components::Cache_Sharing_Mode Data_Cache_Sharing_Mode;//Data cache sharing among concurrently running I/O flows, if NVMe host interface is used
	unsigned int Data_Cache_Capacity;//Data cache capacity in bytes
	unsigned int Data_Cache_DRAM_Row_Size;//The row size of DRAM in the data cache, the unit is bytes
	unsigned int Data_Cache_DRAM_Data_Rate;//Data access rate to access DRAM in the data cache, the unit is MT/s
	unsigned int Data_Cache_DRAM_Data_Busrt_Size;//The number of bytes that are transferred in one burst (it depends on the number of DRAM chips)
	sim_time_type Data_Cache_DRAM_tRCD;//tRCD parameter to access DRAM in the data cache, the unit is nano-seconds
	sim_time_type Data_Cache_DRAM_tCL;//tCL parameter to access DRAM in the data cache, the unit is nano-seconds
	sim_time_type Data_Cache_DRAM_tRP;//tRP parameter to access DRAM in the data cache, the unit is nano-seconds
	SSD_Components::Flash_Address_Mapping_Type Address_Mapping;
	bool Ideal_Mapping_Table;//If mapping is ideal, then all the mapping entries are found in the DRAM and there is no need to read mapping entries from flash
	unsigned int CMT_Capacity;//Size of SRAM/DRAM space that is used to cache address mapping table, the unit is bytes
	SSD_Components::CMT_Sharing_Mode CMT_Sharing_Mode;//How the entire CMT space is shared among concurrently running flows
	SSD_Components::Flash_Plane_Allocation_Scheme_Type Plane_Allocation_Scheme;
	SSD_Components::Flash_Scheduling_Type Transaction_Scheduling_Policy;
	double Overprovisioning_Ratio;//The ratio of spare space with respect to the whole available storage space of SSD
	double GC_Exec_Threshold;//The threshold for the ratio of free pages that used to trigger GC
	SSD_Components::GC_Block_Selection_Policy_Type GC_Block_Selection_Policy;
	bool Use_Copyback_for_GC;
	bool Preemptible_GC_Enabled;
	double GC_Hard_Threshold;//The hard gc execution threshold, used to stop preemptible gc execution
	bool Dynamic_Wearleveling_Enabled;
	bool Static_Wearleveling_Enabled;
	unsigned int Static_Wearleveling_Threshold;
	sim_time_type Preferred_suspend_erase_time_for_read;//in nano-seconds, if the remaining time of the ongoing erase is smaller than Prefered_suspend_erase_time_for_read, then the ongoing erase operation will be suspended
	sim_time_type Preferred_suspend_erase_time_for_write;//in nano-seconds, if the remaining time of the ongoing erase is smaller than Prefered_suspend_erase_time_for_write, then the ongoing erase operation will be suspended
	sim_time_type Preferred_suspend_write_time_for_read;//in nano-seconds, if the remaining time of the ongoing write is smaller than Prefered_suspend_write_time_for_read, then the ongoing erase operation will be suspended
	unsigned int Flash_Channel_Count;
	unsigned int Flash_Channel_Width;//Channel width in byte
	unsigned int Channel_Transfer_Rate;//MT/s
	unsigned int Chip_No_Per_Channel;
	SSD_Components::ONFI_Protocol Flash_Comm_Protocol;
	Flash_Parameter_Set Flash_Parameters;
	unsigned int SSD_Count; // Number of SSDs in the virtual RAID device
	unsigned int Stripe_Unit_LBA; // Stripe unit size in LBA units
	bool SWANS_Enabled;
	unsigned int SWANS_Zone_Size_LBA;
	unsigned int Zone_Stripe_Multiplier;
	sim_time_type SWANS_Epoch_Length;
	sim_time_type SWANS_Epoch_Default;
	sim_time_type SWANS_Epoch_Placement;
	sim_time_type SWANS_Epoch_Migration;
	double SWANS_TH_Precautionary;
	double SWANS_TH_Critical;
	unsigned int SWANS_Max_Concurrent_Migrations;
	unsigned int SWANS_Migration_Buffer_Limit;
	unsigned int SWANS_Migration_Working_Queue_Limit;
	std::string SWANS_Buffered_Write_Completion_Mode;
	bool Bad_Block_Retirement_Enabled;
	std::string Simulation_Stop_Mode;
	double End_of_Life_Threshold;
	void XML_serialize(Utils::XmlWriter& xmlwriter);
	void XML_deserialize(rapidxml::xml_node<> *node);
};

#endif // !DEVICE_PARAMETER_SET_H
