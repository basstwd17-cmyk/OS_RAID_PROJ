#ifndef EXECUTION_PARAMETER_SET_H
#define EXECUTION_PARAMETER_SET_H

#include <vector>
#include "Parameter_Set_Base.h"
#include "Device_Parameter_Set.h"
#include "IO_Flow_Parameter_Set.h"
#include "Host_Parameter_Set.h"

class Execution_Parameter_Set : public Parameter_Set_Base
{
public:
// Host_Parameter_Set과 Device_Parameter_Set을 멤버로 갖도록 변경
	Execution_Parameter_Set();
	Host_Parameter_Set Host_Configuration;
	Device_Parameter_Set SSD_Device_Configuration;

	void XML_serialize(Utils::XmlWriter& xmlwriter);
	void XML_deserialize(rapidxml::xml_node<> *node);
};

#endif // !EXECUTION_PARAMETER_SET_H
