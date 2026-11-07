#include "Trace_Pool.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace Host_Components {
namespace {
uint64_t integer(const std::string& value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("Trace fields must be unsigned decimal integers");
    return std::stoull(value);
}
uint64_t add(uint64_t a, uint64_t b) {
    if (a > std::numeric_limits<uint64_t>::max() - b) throw std::overflow_error("Trace time overflow");
    return a + b;
}
}
PoolTraceRecord TracePool::Parse(const std::string& line, Trace_Time_Unit unit) {
    std::istringstream input(line);
    std::string t, d, l, s, op, extra;
    if (!(input >> t >> d >> l >> s >> op) || (input >> extra))
        throw std::invalid_argument("Trace requires five fields: time device LBA sectors type");
    PoolTraceRecord r;
    r.Time = integer(t); r.Device = integer(d); r.Lba = integer(l);
    const auto sectors = integer(s);
    if (!sectors || sectors > std::numeric_limits<unsigned int>::max() || (op != "0" && op != "1"))
        throw std::invalid_argument("Trace requires positive sectors and type 0(write) or 1(read)");
    r.Sectors = static_cast<unsigned int>(sectors); r.Write = op == "0";
    if (unit == Trace_Time_Unit::MICROSECOND) {
        if (r.Time > std::numeric_limits<uint64_t>::max() / 1000) throw std::overflow_error("Trace time overflow");
        r.Time *= 1000;
    } else if (unit == Trace_Time_Unit::PICOSECOND) r.Time /= 1000;
    return r;
}
void TracePool::Initialize(const std::string& directory, const std::string& extension,
    uint32_t seed, Trace_Time_Unit u, uint64_t g) {
    if (!g || extension.empty() || extension[0] != '.') throw std::invalid_argument("Trace pool needs a positive gap and dotted extension");
    files.clear(); stream.close(); stream.clear(); requests = writes = round = last_time = 0;
    unit = u; gap = g; random.seed(seed);
    auto accept = [&](const std::string& name) {
        if (name.size() >= extension.size() && name.compare(name.size() - extension.size(), extension.size(), extension) == 0)
            files.push_back(directory + "/" + name);
    };
#ifdef _WIN32
    WIN32_FIND_DATAA data;
    HANDLE search = FindFirstFileA((directory + "/*").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) throw std::invalid_argument("Cannot read trace directory: " + directory);
    do { if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) accept(data.cFileName); } while (FindNextFileA(search, &data));
    FindClose(search);
#else
    DIR* dir = opendir(directory.c_str());
    if (!dir) throw std::invalid_argument("Cannot read trace directory: " + directory);
    while (auto entry = readdir(dir)) {
        struct stat info;
        if (stat((directory + "/" + entry->d_name).c_str(), &info) == 0 && S_ISREG(info.st_mode)) accept(entry->d_name);
    }
    closedir(dir);
#endif
    std::sort(files.begin(), files.end());
    if (files.empty()) throw std::invalid_argument("Trace directory has no matching files");
    for (const auto& path : files) {
        std::ifstream file(path);
        if (!file) throw std::invalid_argument("Cannot open trace: " + path);
        std::string line; uint64_t count = 0, previous = 0;
        while (std::getline(file, line)) {
            try {
                auto r = Parse(line, unit);
                if (count && r.Time < previous) throw std::invalid_argument("Non-monotonic timestamp");
                previous = r.Time; ++count; ++requests; writes += r.Write ? 1 : 0;
            } catch (const std::exception& e) { throw std::invalid_argument(path + ":" + std::to_string(count + 1) + ": " + e.what()); }
        }
        if (!count) throw std::invalid_argument("Empty trace: " + path);
    }
    if (!writes) throw std::invalid_argument("EOL trace pool contains no writes");
}
void TracePool::Open_next() {
    // Rejection sampling avoids implementation-dependent uniform_int_distribution sequences.
    const uint64_t range = uint64_t(1) << 32;
    if (files.empty() || files.size() > range) throw std::runtime_error("Invalid trace pool size");
    const uint64_t limit = range - range % files.size();
    uint32_t value;
    do { value = random(); } while (value >= limit);
    current_file = files[value % files.size()];
    stream.close(); stream.clear(); stream.open(current_file);
    if (!stream) throw std::runtime_error("Cannot reopen trace: " + current_file);
    offset = round ? add(last_time, gap) : 0;
    first = true; ++round;
    std::cout << "TRACE_POOL round=" << round << " file=" << current_file << " start_ns=" << offset << '\n';
}
PoolTraceRecord TracePool::Next() {
    if (!stream.is_open()) Open_next();
    std::string line;
    if (!std::getline(stream, line)) {
        Open_next();
        if (!std::getline(stream, line)) throw std::runtime_error("Trace became empty: " + current_file);
    }
    auto r = Parse(line, unit);
    if (first) { origin = r.Time; first = false; }
    if (r.Time < origin) throw std::runtime_error("Trace timestamp precedes file origin");
    r.Time = add(offset, r.Time - origin);
    if (r.Time < last_time) throw std::runtime_error("Non-monotonic trace replay");
    last_time = r.Time;
    return r;
}
}
