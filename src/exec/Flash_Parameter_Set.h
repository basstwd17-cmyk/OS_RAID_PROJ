#ifndef FLASH_PARAMETER_SET_H
#define FLASH_PARAMETER_SET_H

#include "../sim/Sim_Defs.h"
#include "../nvm_chip/flash_memory/FlashTypes.h"
#include "Parameter_Set_Base.h"

class Flash_Parameter_Set : Parameter_Set_Base
{
public:
	Flash_Parameter_Set();
	Flash_Technology_Type Flash_Technology;
	NVM::FlashMemory::Command_Suspension_Mode CMD_Suspension_Support;
	sim_time_type Page_Read_Latency_LSB;
	sim_time_type Page_Read_Latency_CSB;
	sim_time_type Page_Read_Latency_MSB;
	sim_time_type Page_Program_Latency_LSB;
	sim_time_type Page_Program_Latency_CSB;
	sim_time_type Page_Program_Latency_MSB;
	sim_time_type Block_Erase_Latency;//Block erase latency in nano-seconds
	unsigned int Block_PE_Cycles_Limit;
	sim_time_type Suspend_Erase_Time;//in nano-seconds
	sim_time_type Suspend_Program_Time;//in nano-seconds
	unsigned int Die_No_Per_Chip;
	unsigned int Plane_No_Per_Die;
	unsigned int Block_No_Per_Plane;
	unsigned int Page_No_Per_Block;//Page no per block
	unsigned int Page_Capacity;//Flash page capacity in bytes
	unsigned int Page_Metadat_Capacity;//Flash page metadata capacity in bytes
	void XML_serialize(Utils::XmlWriter& xmlwriter);
	void XML_deserialize(rapidxml::xml_node<> *node);
};

#endif // !FLASH_PARAMETER_SET_H
