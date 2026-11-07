#ifndef SWANS_PARAMETER_SET_H
#define SWANS_PARAMETER_SET_H
#include "Device_Parameter_Set.h"

// Overlay policy settings after SSD XML and before creating any simulated device.
void Read_swans_configuration(const std::string& path, Device_Parameter_Set& parameters);
bool Set_swans_parameter(const std::string& key, const std::string& value, Device_Parameter_Set& parameters);
#endif
