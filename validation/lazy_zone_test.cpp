#include "../src/policy/migration_executor.h"
#include "../src/policy/wear_leveling_policy.h"
#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <utility>
using namespace RAID_Policy;
using namespace SSD_Components;
using Address = std::pair<unsigned int, LHA_type>;

struct Fixture {
    ZoneDirectory d;
    MigrationExecutor e;
    std::map<Address,int> storage;
    std::map<std::pair<uint64_t,uint64_t>,int> expected;
    std::vector<int> buffer;
    std::vector<std::unique_ptr<User_Request>> requests;
    io_request_id_type active;
    unsigned io=0, failed_discards=0, finished=0, reads=0, writes=0;
    bool exchange;
    Fixture(bool reserve) : e(8), exchange(reserve) {
        d.Initialize(2,16,16,8,64,reserve,48);
        d.Configure_popularity("HALF_EPOCH",100);
        for (uint64_t z=0; z<(reserve?4:1); ++z) {
            // Sparse second page: copy only valid sectors, never synthesize padding data.
            d.Observe_data(0,z,0,8); d.Observe_data(0,z,10,3);
            for (uint64_t s=0;s<16;++s) if (s<8 || (s>=10 && s<13)) {
                auto a=address(z,s,false); storage[a]=int(z*100+s+1); expected[{z,s}]=storage[a];
            }
        }
        MigrationLimits limits; limits.On_demand=true; limits.Period=100;
        e.Configure_limits(limits);
        e.Task_finished=[&](uint64_t z,bool abort){assert(z==0 && !abort);++finished;};
        MigrationTask task; task.Op.Hot_zone=0; task.Op.Cold_zone=1;
        task.Op.Hot_ssd=0; task.Op.Cold_ssd=1;
        if(reserve) task.Op.Reserve_zone=4;
        e.Start({task},d,100);
    }
    Address address(uint64_t zone,uint64_t offset,bool mixed=true) {
        ZoneResolveResult r; d.Resolve(zone*16+offset,r); if(mixed)e.Resolve_on_demand(r);
        return {r.Disk_id,r.Local_lba};
    }
    User_Request* request(uint64_t start,unsigned count,UserRequestType type=UserRequestType::WRITE) {
        requests.emplace_back(new User_Request()); auto p=requests.back().get();
        p->Start_LBA=start;p->SizeInSectors=count;p->Type=type;p->Stream_id=0;
        return p;
    }
    void enqueue(User_Request* p,sim_time_type now) {
        std::vector<uint64_t> zones;
        for(unsigned s=0;s<p->SizeInSectors;++s){auto z=d.Zone_id_of_lba(p->Start_LBA+s);if(std::find(zones.begin(),zones.end(),z)==zones.end())zones.push_back(z);}
        assert(e.Maybe_intercept(p,zones,d,{},now)==MigrationExecutor::InterceptResult::BUFFERED);
    }
    void step(sim_time_type now,bool host_complete=true) {
        if(!active.empty()){auto id=active;active.clear();assert(e.Notify_request_completed(id,d));}
        e.Poll(d,[&](const StripeCopyPlan& p,bool write,uint64_t){
            assert(active.empty());
            if(!write){buffer.clear();for(unsigned s=0;s<p.Lba_count;++s)buffer.push_back(storage.at({p.Source_disk_id,p.Source_lba+s}));++reads;}
            else{assert(buffer.size()==p.Lba_count);for(unsigned s=0;s<p.Lba_count;++s)storage[{p.Destination_disk_id,p.Destination_lba+s}]=buffer[s];++writes;}
            active="copy-"+std::to_string(++io);return active;
        },[&](const StripeCopyPlan& p,uint64_t){
            if(failed_discards){--failed_discards;return false;}
            for(unsigned s=0;s<p.Lba_count;++s)storage.erase({p.Source_disk_id,p.Source_lba+s});return true;
        },now);
        for(auto item:e.Drain_replay_requests()) {
            auto p=item.Request;assert(e.Replay_ready(p));assert(!item.Complete_without_dispatch);
            for(unsigned s=0;s<p->SizeInSectors;++s){
                const uint64_t lba=p->Start_LBA+s,z=d.Zone_id_of_lba(lba),off=lba%16;
                if(p->Type==UserRequestType::WRITE){
                    const int value=10000+int(lba);storage[address(z,off)]=value;expected[{z,off}]=value;
                    d.Observe_write(0,z,off,1,s==0);
                }else assert(storage.at(address(z,off))==expected.at({z,off}));
            }
            if(host_complete)e.Notify_host_completed(p->ID);
        }
    }
    void verify() {
        for(auto item:expected) assert(storage.at(address(item.first.first,item.first.second))==item.second);
        assert(d.Duplicate_physical_location_count()==0);
    }
    void finish(sim_time_type now=200) {
        for(unsigned i=0;e.Has_inflight() && i<500;++i)step(now+i);
        assert(!e.Has_inflight() && finished==1 && d.Migrating_zone_count()==0);
        verify();
    }
};

void normal_lazy() {
    Fixture f(false);f.step(101);assert(f.io==0); // no eager hot copy
    auto source=f.address(0,0);auto read=f.request(0,1,UserRequestType::READ);
    assert(f.e.Maybe_intercept(read,{0},f.d)==MigrationExecutor::InterceptResult::SUBMITTED);
    auto full=f.request(0,8);f.enqueue(full,102);f.step(102,false);
    assert(f.io==0 && f.e.Lazy_offloaded_pages()==1 && f.address(0,0)!=source);
    auto during=f.request(0,1,UserRequestType::READ);f.enqueue(during,103);f.step(103);
    assert(f.e.Buffered_count()==1); // wait for the real host write completion
    f.e.Notify_host_completed(full->ID);f.failed_discards=1;f.step(104);f.step(105);f.step(106);
    auto partial=f.request(11,1);f.enqueue(partial,107);
    for(unsigned i=0;f.e.Buffered_count() && i<30;++i)f.step(108+i);
    assert(f.e.Buffered_count()==0 && f.reads==1 && f.writes==1); // only valid 10..12 copied
    f.finish();assert(f.e.Lazy_deadline_drains()==1);
}

void exchange_and_deadline() {
    Fixture f(true);
    auto old_hot=f.address(0,0),old_cold=f.address(1,0);
    assert(f.d.Is_reserve_zone(4)); // an admitted reserve is locked, so it is no longer an available spare
    for(unsigned i=0;f.e.Cold_stages_completed()==0 && i<50;++i)f.step(101+i);
    assert(f.e.Cold_stages_completed()==1 && f.e.Has_inflight());
    assert(f.address(0,0)==old_hot && f.address(1,0)!=old_cold);
    auto before=f.io;f.step(190);assert(f.io==before); // hot phase remains lazy
    f.verify();f.failed_discards=2;f.finish();
    assert(f.address(0,0)==old_cold);
    unsigned disk;LHA_type lba;f.d.Resolve_zone_lba(4,0,disk,lba);
    assert(Address(disk,lba)==old_hot && f.d.Is_empty(4));
}

void cold_write_forces_completion() {
    Fixture f(true);
    for(unsigned i=0;f.e.Cold_stages_completed()==0 && i<50;++i)f.step(101+i);
    auto cold_write=f.request(16,8);f.enqueue(cold_write,130);
    for(unsigned i=0;f.e.Has_inflight() && i<100;++i)f.step(131+i/4);
    assert(!f.e.Has_inflight() && f.e.Lazy_cold_write_drains()==1);
    assert(f.e.Lazy_deadline_drains()==0);f.verify();
}

void popularity_and_policy() {
    ZoneDirectory d;d.Initialize(2,16,16,8,64,true,48);d.Configure_popularity("HALF_EPOCH",10);
    d.Observe_write(0,0,8,true);assert(d.Zone_popularity(0)==8);
    d.Advance_popularity(10);assert(d.Zone_popularity(0)==4);
    d.Observe_write(0,0,4,true);assert(d.Zone_popularity(0)==8);
    d.On_migration_completed();assert(d.Zone_popularity(0)==8);
    d.Advance_popularity(30);assert(d.Zone_popularity(0)==2);
    WearLevelingPolicy p;p.Initialize(2,1e12,1e13,8,1024);p.Require_single_zone_epochs();
    p.Configure_metric("TEC","DYNAMIC_WEAR","SSD_POPULARITY",5,10);
    WearSnapshot wear;wear.Values={.9,.1};auto decision=p.Evaluate(d,wear);
    assert(decision.Migrations.size()==1 && decision.Migrations[0].Hot_ssd==0);
    assert(decision.Wear_threshold==.75); // fixed max=1, not current observed max=.9
    wear.Values={.75,.25};assert(p.Evaluate(d,wear).Migrations.empty()); // strict >
}

void independent_state_and_space_options() {
    // Executor concurrency must not disable the three SWANS states.
    ZoneDirectory d;d.Initialize(2,16,16,8,64,true,48);
    d.Configure_popularity("HALF_EPOCH",10);d.Observe_write(0,0,8,true);
    WearLevelingPolicy writes;writes.Initialize(2,5,10,8,1);
    writes.Require_single_zone_epochs();
    writes.Configure_metric("WRITE_BYTES","IMBALANCE","SSD_POPULARITY",-1,-1);
    assert(writes.Evaluate(d,{}).State==PolicyState::NORMAL);
    writes.Observe_host_write(0,8);assert(writes.Evaluate(d,{}).State==PolicyState::NORMAL);
    writes.Observe_host_write(0,2);auto redirect=writes.Evaluate(d,{});
    assert(redirect.State==PolicyState::REDIRECT && redirect.Redirect.Valid && redirect.Mu==5);
    writes.Observe_host_write(0,10);auto migration=writes.Evaluate(d,{});
    assert(migration.State==PolicyState::MIGRATION && migration.Mu==10);
    assert(migration.Migrations.size()==1 && migration.Migrations[0].Reserve_zone==INVALID_ZONE_ID);

    WearLevelingPolicy tec;tec.Initialize(2,1,2,8,1);tec.Require_single_zone_epochs();
    tec.Configure_metric("TEC","DYNAMIC_WEAR","SSD_POPULARITY",5,10);
    tec.Observe_host_write(1,1000000); // TEC decisions must ignore write-byte ordering.
    WearSnapshot wear;wear.Values={.3,.3};assert(tec.Evaluate(d,wear).State==PolicyState::NORMAL);
    wear.Values={.6,.2};redirect=tec.Evaluate(d,wear);
    assert(redirect.State==PolicyState::REDIRECT && redirect.Redirect.Hot_ssd==0);
    wear.Values={.75,.25};assert(tec.Evaluate(d,wear).State==PolicyState::REDIRECT);
    wear.Values={.76,.24};migration=tec.Evaluate(d,wear);
    assert(migration.State==PolicyState::MIGRATION && migration.Migrations.size()==1);
    assert(migration.Migrations[0].Reserve_zone==INVALID_ZONE_ID); // ordinary empty first
    d.Observe_write(1,0,8,true);d.Observe_write(3,0,8,true); // fill youngest SSD
    migration=tec.Evaluate(d,wear);
    assert(migration.Migrations.size()==1 && migration.Migrations[0].Reserve_zone!=INVALID_ZONE_ID);
    assert(d.Is_reserve_zone(migration.Migrations[0].Reserve_zone));

    ZoneDirectory no_reserve;no_reserve.Initialize(2,16,16,8,64,false,32);
    no_reserve.Configure_popularity("HALF_EPOCH",10);
    for(unsigned z=0;z<4;++z)no_reserve.Observe_write(z,0,8,true);
    assert(tec.Evaluate(no_reserve,wear).Migrations.empty());
}

void queue_and_abort() {
    Fixture f(false);f.step(101);unsigned callbacks=0;
    f.e.Task_finished=[&](uint64_t,bool aborted){assert(aborted);++callbacks;};
    for(unsigned i=0;i<8;++i)f.enqueue(f.request(0,8),102);
    auto overflow=f.request(0,8);
    assert(f.e.Maybe_intercept(overflow,{0},f.d)==MigrationExecutor::InterceptResult::BACKPRESSURE);
    auto summary=f.e.Abort_all(f.d);assert(summary.Task_count==1 && summary.Deferred_request_count==8 && callbacks==1);
    assert(f.d.Migrating_zone_count()==2 && !f.e.Has_inflight()); // EOL never advertises partial zones as reusable
}

void empty_destination_read_waits() {
    Fixture f(false);f.step(101);
	f.e.Task_finished=[](uint64_t,bool aborted){assert(aborted);};
    auto read=f.request(16,1,UserRequestType::READ);f.enqueue(read,102);
    f.step(103);assert(f.e.Buffered_count()==1 && f.io==0);
    auto summary=f.e.Abort_all(f.d);assert(summary.Deferred_request_count==1);
}

void cross_page_updates() {
    Fixture f(false);f.step(101);
    f.enqueue(f.request(6,4),102);
    for(unsigned i=0;f.e.Buffered_count() && i<40;++i)f.step(103+i);
    assert(f.e.Buffered_count()==0 && f.reads==2 && f.writes==2);
    f.finish();
    Fixture g(false);g.step(101);
    g.enqueue(g.request(0,12),102);g.step(102);
    assert(g.e.Lazy_offloaded_pages()==1 && g.e.Has_active_copy());
    // Deadline arrives while preserving the partial second page. The first
    // page's full overwrite must complete before its old source is discarded.
    g.finish(200);assert(g.e.Lazy_deadline_drains()==1);
}

int main(){normal_lazy();exchange_and_deadline();cold_write_forces_completion();popularity_and_policy();independent_state_and_space_options();queue_and_abort();empty_destination_read_waits();cross_page_updates();std::cout<<"lazy migration unit tests passed\n";}
