#include "SWANS_Parameter_Set.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>
#include "../utils/rapidxml/rapidxml.hpp"

namespace {
uint64_t number(const std::string& value, uint64_t maximum = std::numeric_limits<uint64_t>::max())
{
	if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
		throw std::invalid_argument("Expected a nonnegative decimal integer: " + value);
	auto result = std::stoull(value);
	if (result > maximum) throw std::out_of_range("SWANS integer out of range: " + value);
	return result;
}
bool boolean(const std::string& value)
{
	if (value == "true") return true;
	if (value == "false") return false;
	throw std::invalid_argument("Expected true or false: " + value);
}
double real(const std::string& value)
{
	size_t end = 0;
	double result = std::stod(value, &end);
	if (end != value.size() || !std::isfinite(result) || result < 0)
		throw std::invalid_argument("Expected a finite nonnegative number: " + value);
	return result;
}
}

bool Set_swans_parameter(const std::string& key, const std::string& value, Device_Parameter_Set& p)
{
	// Public name for the hidden-zone, two-stage migration path. Keep the
	// earlier reserve-zone option as an alias for existing experiment files.
	if (key == "op_zone_migration") { p.SWANS_Reserve_Zone_Enabled = boolean(value); return true; }
#define BOOL(field) if (key == #field) { p.field = boolean(value); return true; }
#define UINT(field) if (key == #field) { p.field = number(value, std::numeric_limits<unsigned int>::max()); return true; }
#define U64(field) if (key == #field) { p.field = number(value); return true; }
	BOOL(SWANS_Enabled)
	BOOL(SWANS_Reserve_Zone_Enabled)
	BOOL(SWANS_Migration_Pause_On_Read)
	UINT(SWANS_Zone_Size_LBA)
	UINT(Zone_Stripe_Multiplier)
	UINT(SWANS_Max_Concurrent_Migrations)
	UINT(SWANS_Migration_Working_Queue_Limit)
	U64(SWANS_Epoch_Default)
	U64(SWANS_Epoch_Placement)
	U64(SWANS_Epoch_Migration)
	U64(SWANS_Zone_Popularity_Period)
	U64(SWANS_Balance_Unit_Bytes)
	U64(SWANS_Migration_Slice_Time)
	U64(SWANS_Migration_Slice_Sectors)
	U64(SWANS_Migration_Pause_Duration)
#undef BOOL
#undef UINT
#undef U64
	if (key == "SWANS_TH_Precautionary") { p.SWANS_TH_Precautionary = real(value); return true; }
	if (key == "SWANS_TH_Critical") { p.SWANS_TH_Critical = real(value); return true; } // legacy only; not the migration trigger
	if (key == "SWANS_Wear_Metric") {
		if (value != "BAD_BLOCK_COUNT" && value != "TEC" && value != "WRITE_BYTES") throw std::invalid_argument("Unsupported SWANS_Wear_Metric: " + value);
		p.SWANS_Wear_Metric = value; return true;
	}
	if (key == "SWANS_TEC_TH_Precautionary_PctPoint") { p.SWANS_TEC_TH_Precautionary_PctPoint = real(value); return true; }
	if (key == "SWANS_TEC_TH_Critical_PctPoint") { p.SWANS_TEC_TH_Critical_PctPoint = real(value); return true; }
	if (key == "SWANS_Migration_Trigger") { p.SWANS_Migration_Trigger = value; return true; }
	if (key == "SWANS_Zone_Selection") { p.SWANS_Zone_Selection = value; return true; }
	if (key == "SWANS_Zone_Popularity_Mode") {
		if (value != "HALF_DECAY" && value != "LAST_EPOCH") throw std::invalid_argument("Invalid popularity mode: " + value);
		p.SWANS_Zone_Popularity_Mode = value; return true;
	}
	return false;
}

void Read_swans_configuration(const std::string& path, Device_Parameter_Set& parameters)
{
	std::ifstream file(path);
	if (!file) throw std::invalid_argument("Cannot open SWANS configuration: " + path);
	std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	std::vector<char> text(contents.begin(), contents.end()); text.push_back('\0');
	rapidxml::xml_document<> doc;
	doc.parse<rapidxml::parse_validate_closing_tags>(text.data());
	auto root = doc.first_node();
	if (!root || std::string(root->name()) != "SWANS_Parameter_Set" || root->next_sibling() || root->first_attribute())
		throw std::invalid_argument("Expected a single SWANS_Parameter_Set root with child setting elements");
	Device_Parameter_Set next = parameters; // no partial overlay on validation error
	std::set<std::string> seen;
	for (auto node = root->first_node(); node; node = node->next_sibling()) {
		const std::string key = node->name();
		std::string value = node->value();
		const auto begin = value.find_first_not_of(" \t\n\r");
		value = begin == std::string::npos ? "" : value.substr(begin, value.find_last_not_of(" \t\n\r") - begin + 1);
		if (node->type() != rapidxml::node_element || node->first_attribute()
			|| (node->first_node() && node->first_node()->next_sibling())
			|| (node->first_node() && node->first_node()->type() != rapidxml::node_data)
			|| !seen.insert(key == "op_zone_migration" ? "SWANS_Reserve_Zone_Enabled" : key).second)
			throw std::invalid_argument("Duplicate or malformed SWANS setting: " + key);
		try {
			if (!Set_swans_parameter(key, value, next)) throw std::invalid_argument("Unknown setting");
		} catch (const std::exception& e) { throw std::invalid_argument(key + ": " + e.what()); }
	}
	if (!next.SWANS_Epoch_Default || !next.SWANS_Balance_Unit_Bytes || !next.SWANS_Migration_Working_Queue_Limit
		|| !next.SWANS_Max_Concurrent_Migrations)
		throw std::invalid_argument("SWANS epoch, balance unit, queue limit and concurrency must be positive");
	if ((next.SWANS_Migration_Slice_Time || next.SWANS_Migration_Slice_Sectors || next.SWANS_Migration_Pause_On_Read)
		&& !next.SWANS_Migration_Pause_Duration)
		throw std::invalid_argument("SWANS_Migration_Pause_Duration must be positive when pauses are enabled");
	parameters = next;
}
