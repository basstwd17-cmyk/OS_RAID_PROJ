#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include "../src/exec/RAID_Controller.h"

using namespace RAID_Policy;
using SSD_Components::User_Request;
using SSD_Components::UserRequestType;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ": " + #condition); } while (false)

struct RAID_Controller_Test_Access {
    static ZoneDirectory& Directory(RAID_Controller& c) { return c.zone_directory; }
    static MigrationExecutor& Executor(RAID_Controller& c) { return c.migration_executor; }
    static uint64_t Zone(RAID_Controller& c, User_Request* r) { return c.subrequest_metadata_by_id.at(r->ID).Zone_id; }
    static bool Priority(RAID_Controller& c, User_Request* r) {
        auto* parent = c.subrequest_metadata_by_id.at(r->ID).Parent;
        return parent && c.migration_executor.Is_priority_read(parent->ID);
    }
    static void Receive(RAID_Controller& c, User_Request* r) { c.process_new_user_request(r); }
    static void Start(RAID_Controller& c, const std::vector<MigrationTask>& tasks) {
        c.migration_executor.Start(tasks, c.zone_directory);
        c.Schedule_swans_event(Simulator->Time() + 1);
    }
    static bool Empty(RAID_Controller& c) {
        return c.inflight.empty() && c.subrequest_metadata_by_id.empty()
            && c.blocked_user_requests.empty() && c.cached_write_requests.empty()
            && !c.migration_executor.Has_inflight();
    }
    static uint64_t Cached_completions(RAID_Controller& c) { return c.swans_stats.Buffered_write_completions; }
};

static User_Request request(UserRequestType type, LHA_type lba = 0, unsigned int sectors = 8,
    stream_id_type stream = 0)
{
    User_Request r;
    r.Type = type; r.Start_LBA = lba; r.SizeInSectors = sectors;
    r.Size_in_byte = sectors * SECTOR_SIZE_IN_BYTE; r.Stream_id = stream;
    r.Priority_class = IO_Flow_Priority_Class::Priority::HIGH;
    r.STAT_InitiationTime = 0; r.IO_command_info = nullptr; r.Data = nullptr;
    return r;
}

static MigrationTask task(ZoneDirectory& d, uint64_t hot, uint64_t cold)
{
    MigrationTask t;
    t.Op.Hot_zone = hot; t.Op.Cold_zone = cold;
    t.Op.Hot_ssd = d.Owner_ssd(hot); t.Op.Cold_ssd = d.Owner_ssd(cold);
    for (unsigned int block = 0; block < 2; ++block) {
        StripeCopyPlan p;
        p.Stream_id = 0; p.Stripe_offset = block; p.Lba_count = 8;
        d.Resolve_zone_lba(hot, block * 8, p.Source_disk_id, p.Source_lba);
        d.Resolve_zone_lba(cold, block * 8, p.Destination_disk_id, p.Destination_lba);
        t.Copies.push_back(p);
        d.Observe_write(0, hot, block * 8, 8, true);
    }
    return t;
}

static void independent_tasks_and_multizone_read()
{
    ZoneDirectory d; d.Initialize(2, 8, 16, 8, 128);
    MigrationExecutor e(1);
    e.Start({ task(d, 0, 1), task(d, 2, 3) }, d);
    unsigned int copies = 0, discards = 0;
    std::vector<std::string> active(2);
    auto submit = [&](const StripeCopyPlan&, bool, uint64_t index) {
        active[index] = "copy" + std::to_string(++copies); return active[index];
    };
    auto discard = [&](const StripeCopyPlan&, uint64_t) { ++discards; return true; };
    e.Poll(d, submit, discard);
    CHECK(copies == 2);
    auto read = request(UserRequestType::READ);
    CHECK(e.Maybe_intercept(&read, { 0 }, 1) == MigrationExecutor::InterceptResult::PRIORITY_READ);
    CHECK(e.Drain_ready_reads().empty());
    e.Notify_request_completed(active[0], d);
    e.Notify_request_completed(active[1], d);
    CHECK(e.Drain_ready_reads().size() == 1);
    e.Poll(d, submit, discard);
    CHECK(copies == 3); // The unrelated second task advances.
    CHECK(e.Is_priority_read(read.ID));
    auto multi = request(UserRequestType::READ, 0, 48);
    e.Maybe_intercept(&multi, { 0, 2 }, 2);
    e.Notify_read_completed(read.ID);
    CHECK(e.Drain_ready_reads().empty()); // Wait for task 2's active copy too.
    e.Notify_request_completed(active[1], d);
    CHECK(e.Drain_ready_reads().size() == 1);
    e.Poll(d, submit, discard);
    CHECK(copies == 3 && discards == 0);
    e.Notify_read_completed(multi.ID);
    e.Poll(d, submit, discard);
    CHECK(copies == 5); // Both tasks resume at their saved offsets.
}

static void cache_and_source_lifetime()
{
    ZoneDirectory d; d.Initialize(2, 8, 16, 8, 128);
    MigrationExecutor e(1);
    const auto plan = task(d, 0, 1);
    e.Start({ plan }, d);
    auto write = request(UserRequestType::WRITE, 0, 8, 1);
    auto overflow = request(UserRequestType::WRITE);
    auto read = request(UserRequestType::READ, 8); // Cold zone.
    CHECK(e.Maybe_intercept(&write, { 0 }, 1) == MigrationExecutor::InterceptResult::BUFFERED);
    CHECK(e.Maybe_intercept(&overflow, { 0 }, 2) == MigrationExecutor::InterceptResult::BACKPRESSURE);
    CHECK(e.Maybe_intercept(&read, { 1 }, 3) == MigrationExecutor::InterceptResult::PRIORITY_READ);
    CHECK(e.Dirty_blocks() == 0 && e.Backpressure_events() == 1);
    CHECK(e.Drain_ready_reads().size() == 1);
    unsigned int submitted = 0, discarded = 0;
    std::string active;
    auto submit = [&](const StripeCopyPlan& p, bool write_copy, uint64_t) {
        CHECK(d.Owner_ssd(0) == 0 && discarded == 0);
        CHECK(p.Source_disk_id == 0 && p.Destination_disk_id == 1);
        CHECK(write_copy == (submitted >= 2));
        active = "copy" + std::to_string(++submitted); return active;
    };
    auto discard = [&](const StripeCopyPlan&, uint64_t) {
        CHECK(submitted == 4); ++discarded; return true;
    };
    e.Poll(d, submit, discard); CHECK(submitted == 0);
    e.Notify_read_completed(read.ID);
    for (int i = 0; i < 4; ++i) {
        e.Poll(d, submit, discard);
        CHECK(e.Drain_replay_requests().empty());
        e.Notify_request_completed(active, d);
        CHECK(d.Owner_ssd(0) == 0 && discarded == 0);
    }
    e.Poll(d, submit, discard);
    CHECK(!e.Has_inflight() && d.Owner_ssd(0) == 1 && discarded == 2);
    const auto replay = e.Drain_replay_requests();
    CHECK(replay.size() == 1 && replay[0].Request == &write);
    CHECK(e.Grabbed_blocks() == 2 && e.Restored_blocks() == 2);
    CHECK(e.Copy_buffer_bytes() == 0 && e.Buffered_count() == 0);
}

static void abort_paused_migration()
{
    ZoneDirectory d; d.Initialize(2, 8, 16, 8, 128);
    MigrationExecutor e;
    e.Start({ task(d, 0, 1) }, d);
    auto read = request(UserRequestType::READ);
    auto write = request(UserRequestType::WRITE);
    e.Maybe_intercept(&read, { 0 }); e.Maybe_intercept(&write, { 0 });
    const auto summary = e.Abort_all(d);
    CHECK(summary.Task_count == 1 && summary.Deferred_request_count == 2);
    CHECK(!e.Has_priority_reads() && e.Drain_ready_reads().empty());
    CHECK(!e.Has_inflight() && d.Migrating_zone_count() == 0);
    CHECK(d.Owner_ssd(0) == 0 && e.Drain_replay_requests().empty());
}

// A deterministic backend uses real controller/engine events and delayed
// completions, while making request order and destination addresses observable.
struct EventDriver : MQSimEngine::Sim_Object {
    std::vector<std::unique_ptr<std::function<void()>>> actions;
    unsigned int events = 0;
    EventDriver() : Sim_Object("migration-test-events") {}
    void At(sim_time_type time, std::function<void()> action) {
        actions.emplace_back(new std::function<void()>(action));
        Simulator->Register_sim_event(time, this, actions.back().get());
    }
    void Start_simulation() override {}
    void Validate_simulation_config() override {}
    void Execute_simulator_event(MQSimEngine::Sim_Event* ev) override {
        CHECK(++events < 500);
        (*static_cast<std::function<void()>*>(ev->Parameters))();
    }
};

static void controller_read_priority(int phase)
{
    Simulator->Reset();
    EventDriver driver;
    RAID_Controller c("raid-test", nullptr, 2, 2, 8, 8, true, 16, 2,
        100000, 100000, 100000, 1e30, 1e30, 2, 2, 4096, 128);
    auto& d = RAID_Controller_Test_Access::Directory(c);
    auto& e = RAID_Controller_Test_Access::Executor(c);
    auto read = request(UserRequestType::READ, 0, 24); // hot, cold, hot stripes
    auto second_read = request(UserRequestType::READ, 0, 8, 1);
    auto hot_write = request(UserRequestType::WRITE, 0, 8, 1);
    auto cold_write = request(UserRequestType::WRITE, 8);
    auto overflow_write = request(UserRequestType::WRITE, 0, 24);
    auto unrelated = request(UserRequestType::READ, 32);
    std::map<std::string, sim_time_type> completed;
    sim_time_type migration_end = 0, read_last_completion = 0;
    unsigned int copies = 0, user_reads = 0, user_writes = 0;
    unsigned int outstanding_priority_reads = 0;
    bool injected = false;
    auto inject = [&]() {
        CHECK(!injected); injected = true;
        read.STAT_InitiationTime = Simulator->Time();
        RAID_Controller_Test_Access::Receive(c, &hot_write);
        RAID_Controller_Test_Access::Receive(c, &cold_write);
        RAID_Controller_Test_Access::Receive(c, &overflow_write); // Full cache.
        RAID_Controller_Test_Access::Receive(c, &read);
        RAID_Controller_Test_Access::Receive(c, &second_read);
        RAID_Controller_Test_Access::Receive(c, &unrelated);
        CHECK(e.Backpressure_events() == 1);
    };
    e.Task_finished = [&](uint64_t, bool aborted) {
        CHECK(!aborted && outstanding_priority_reads == 0);
        migration_end = Simulator->Time();
    };
    c.Set_complete_callback([&](User_Request* r) {
        CHECK(completed.count(r->ID) == 0);
        completed[r->ID] = Simulator->Time();
        if (r == &read || r == &second_read) read_last_completion = Simulator->Time();
        if (r->Type == UserRequestType::WRITE) CHECK(migration_end > 0);
    });
    c.Set_submit_callback([&](unsigned int disk, User_Request* r) {
        const bool priority = RAID_Controller_Test_Access::Priority(c, r);
        if (!r->Is_migration) CHECK(disk == d.Owner_ssd(RAID_Controller_Test_Access::Zone(c, r)));
        if (r->Is_migration) {
            CHECK(outstanding_priority_reads == 0);
            ++copies;
            CHECK(copies <= 4);
            CHECK(d.Owner_ssd(0) == 0); // Mapping stays at source during restore.
            if (!injected && ((phase == 1 && copies == 1) || (phase == 2 && copies == 3)))
                driver.At(Simulator->Time() + 1, inject);
        } else if (r->Type == UserRequestType::READ) {
            ++user_reads;
            if (priority) {
                CHECK(e.Has_inflight() && d.Owner_ssd(0) == 0);
                ++outstanding_priority_reads;
            }
        } else {
            ++user_writes;
            CHECK(migration_end > 0 && Simulator->Time() >= migration_end);
            CHECK(completed.count(read.ID) && completed.count(second_read.ID));
        }
        // The second hot stripe finishes later: resuming on a subrequest
        // instead of the full host read would violate the assertions above.
        const sim_time_type delay = priority && r->Start_LBA == 8 ? 12 : 5;
        driver.At(Simulator->Time() + delay, [&, r, priority]() {
            if (priority) --outstanding_priority_reads;
            c.Notify_sub_request_completed(r); delete r;
        });
        if (!injected && phase == 3 && copies == 4 && r->Is_migration)
            driver.At(Simulator->Time() + delay, inject); // Last restore completed, before commit.
    });
    RAID_Controller_Test_Access::Start(c, { task(d, 0, 1) });
    if (phase == 0) inject();
    unsigned int advances = 0;
    Simulator->Set_time_advance_observer([&](sim_time_type) { CHECK(++advances < 200); });
    Simulator->Start_simulation();
    CHECK(injected && copies == 4 && user_reads == 5 && user_writes == 5);
    CHECK(completed.size() == 6 && migration_end > read_last_completion);
    CHECK(RAID_Controller_Test_Access::Empty(c));
    CHECK(d.Owner_ssd(0) == 1 && d.Owner_ssd(1) == 0);
    CHECK(e.Dirty_blocks() == 0 && e.Restored_blocks() == 2);
    CHECK(c.Get_total_host_write_sectors() == 40);
    CHECK(c.Get_total_attributed_host_write_sectors() == 40);
    CHECK(RAID_Controller_Test_Access::Cached_completions(c) == 2);
    Simulator->Reset();
}

int main()
{
    try {
        independent_tasks_and_multizone_read();
        cache_and_source_lifetime();
        abort_paused_migration();
        for (int phase = 0; phase < 4; ++phase) controller_read_priority(phase);
        std::cout << "PASS: 3 executor scenarios and 4 controller/event scenarios\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: " << ex.what() << '\n'; return 1;
    }
}
