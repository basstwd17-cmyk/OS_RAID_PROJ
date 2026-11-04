#ifndef MQSIM_TRACE_POOL_H
#define MQSIM_TRACE_POOL_H
#include <cstdint>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include "ASCII_Trace_Definition.h"

namespace Host_Components {
struct PoolTraceRecord {
    uint64_t Time = 0, Device = 0, Lba = 0;
    unsigned int Sectors = 0;
    bool Write = false;
};
class TracePool {
public:
    void Initialize(const std::string& directory, const std::string& extension,
        uint32_t seed, Trace_Time_Unit unit, uint64_t gap);
    PoolTraceRecord Next();
    const std::vector<std::string>& Files() const { return files; }
    const std::string& Current_file() const { return current_file; }
    uint64_t Round() const { return round; }
    uint64_t Requests() const { return requests; }
    uint64_t Writes() const { return writes; }
    static PoolTraceRecord Parse(const std::string& line, Trace_Time_Unit unit);
private:
    void Open_next();
    std::vector<std::string> files;
    std::string current_file;
    std::mt19937 random;
    std::ifstream stream;
    Trace_Time_Unit unit = Trace_Time_Unit::NANOSECOND;
    uint64_t gap = 1, round = 0, requests = 0, writes = 0;
    uint64_t origin = 0, offset = 0, last_time = 0;
    bool first = true;
};
}
#endif
