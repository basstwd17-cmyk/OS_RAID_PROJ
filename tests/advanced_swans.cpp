#include "../src/ssd/TEC_Wear.h"
#include "../src/policy/wear_leveling_policy.h"
#include "../src/host/Trace_Pool.h"
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
using namespace RAID_Policy;
int main(int argc, char** argv) {
    assert(argc == 2); // caller creates an empty scratch directory
    std::vector<unsigned int> counts(100);
    std::iota(counts.begin(), counts.end(), 0);
    std::reverse(counts.begin(), counts.end());
    auto original = counts;
    auto s = SSD_Components::Calculate_TEC(counts, 20, 100);
    assert(s.Valid && s.User_blocks == 80 && s.Tail_erase_count == 80 && s.Wear_percent == 80);
    assert(counts == original);
    assert(SSD_Components::Calculate_TEC(counts, 20, 200).Wear_percent == 40);
    assert(!SSD_Components::Calculate_TEC(counts, 0, 100).Valid);
    assert(!SSD_Components::Calculate_TEC(counts, 100, 100).Valid);
    assert(!SSD_Components::Calculate_TEC(counts, 20, 0).Valid);
    counts.assign(100, 0); for (unsigned i = 80; i < 100; ++i) counts[i] = 25;
    assert(SSD_Components::Calculate_TEC(counts, 20, 25).Wear_percent == 100); // includes retired tail
    for (unsigned op = 1; op < 100; ++op) {
        auto c = original; auto sorted = c; std::sort(sorted.begin(), sorted.end());
        assert(SSD_Components::Calculate_TEC(c, op, 100).Tail_erase_count == sorted[100-op]);
    }
    ZoneDirectory d; d.Initialize(4, 8, 32, 8, 512); d.Configure_popularity("HALF_DECAY", 40000000000ULL);
    d.Observe_write(0, 0, 32, true); d.Observe_write(4, 0, 16, true); d.Observe_write(1, 0, 8, true);
    d.Advance_popularity(40000000000ULL);
    assert(d.Zone_popularity(0) == 32 && d.Zone_popularity(4) == 16);
    d.On_migration_completed();
    d.Observe_write(4, 0, 12, true); d.Advance_popularity(80000000000ULL);
    assert(d.Zone_popularity(0) == 16 && d.Zone_popularity(4) == 20);
    assert(d.Find_migration_zone(0, false, true, {}) == 0);
    assert(d.Find_migration_zone(0, false, false, {}) == 4);
    assert(d.Find_migration_zone(3, true, false, {}) == 4);
    d.Mark_migrating(4, true); assert(d.Find_migration_zone(0, true, false, {}) == 0); d.Mark_migrating(4, false);
    assert(d.Find_migration_zone(0, true, false, {4,0}) == 1);
    WearSnapshot w; w.Values = {.8,.2,.2,.2};
    WearLevelingPolicy p; p.Initialize(4, 8, 32, 2, 1);
    p.Configure_metric("TEC", "DYNAMIC_WEAR", "SSD_POPULARITY", 5, 20);
    p.Observe_host_write(3, 999999); // TEC must override opposite byte ordering
    auto r = p.Evaluate(d,w);
    assert(r.Migrations.size() == 2 && r.Migrations[0].Hot_zone == 4 && r.Migrations[0].Hot_ssd == 0);
    assert(p.Current_hottest_ssd() == 0 && p.Current_coldest_ssd() == 1);
    assert(std::abs(r.Mu - 25.98076211353316) < 1e-9);
    w.Values = {.75,.25,.5,.5}; assert(p.Evaluate(d,w).Migrations.empty()); // strict threshold
    w.Values = {.4,.2,.2,.2}; r=p.Evaluate(d,w);
    assert(r.State == PolicyState::REDIRECT && r.Redirect.Hot_ssd == 0);
    w.Values = {.2,.4,.2,.2}; p.Update_wear(w); assert(p.Current_hottest_ssd() == 1);
    w.Values = {.8,.2,.2,.2};
    p.Configure_metric("TEC", "IMBALANCE", "CUMULATIVE", 5, 20); r=p.Evaluate(d,w);
    assert(r.Migrations[0].Hot_zone == 0);
    p.Configure_metric("TEC", "DYNAMIC_WEAR", "GLOBAL_POPULARITY", 5, 20);
    assert(p.Evaluate(d,w).Migrations[0].Hot_zone == 4);
    w.Values = {.9,.9,.9,.9}; assert(p.Evaluate(d,w).State == PolicyState::NORMAL);
    bool bad=false; try { p.Configure_metric("TEC", "DYNAMIC_WEAR", "CUMULATIVE", -1, -1); } catch (...) { bad=true; } assert(bad);
    bad=false; try { p.Update_wear(WearSnapshot()); } catch (...) { bad=true; } assert(bad);
    const std::string dir=argv[1];
    { std::ofstream f(dir+"/a.trace"); f << "500 0 0 8 0\n510 0 8 8 1\n"; }
    { std::ofstream f(dir+"/b.trace"); f << "900 0 16 8 0\n"; }
    Host_Components::TracePool a,b; a.Initialize(dir,".trace",42,Trace_Time_Unit::MICROSECOND,7);
    b.Initialize(dir,".trace",42,Trace_Time_Unit::MICROSECOND,7);
    assert(a.Requests()==3 && a.Writes()==2);
    uint64_t previous=0, previous_round=0; bool seen_a=false,seen_b=false;
    for (unsigned i=0;i<40;++i) {
        auto x=a.Next(), y=b.Next();
        assert(x.Time==y.Time && x.Lba==y.Lba && a.Current_file()==b.Current_file());
        if (i && a.Round()!=previous_round) assert(x.Time==previous+7);
        else if (i) assert(x.Time==previous+10000);
        if (!i) assert(x.Time==0);
        seen_a |= x.Lba==0; seen_b |= x.Lba==16; previous=x.Time; previous_round=a.Round();
    }
    assert(seen_a && seen_b);
    { std::ofstream f(dir+"/empty.trace"); }
    bad=false; try { Host_Components::TracePool c; c.Initialize(dir,".trace",1,Trace_Time_Unit::NANOSECOND,1); } catch (...) { bad=true; } assert(bad);
    for (auto text : {"-1 0 0 8 0", "0 0 0 0 0", "0 0 0 8 2", "0 0 0 8 0 extra"}) {
        bad=false; try { Host_Components::TracePool::Parse(text,Trace_Time_Unit::NANOSECOND); } catch (...) { bad=true; } assert(bad);
    }
    std::cout << "TEC, policy modes, zone selection, trace pool tests passed\n";
}
