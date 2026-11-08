#include "migration_executor.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace RAID_Policy {

void MigrationExecutor::Lazy_start(const std::vector<MigrationTask>& tasks, ZoneDirectory& directory, sim_time_type now)
{
    if (Has_inflight() || tasks.empty()) return;
    if (tasks.size() != 1 || !limits.Period || directory.Stripe_unit_lba() != directory.Zone_size_lba())
        throw std::invalid_argument("ON_DEMAND requires one task and stripe = zone");
    if (now / limits.Period >= std::numeric_limits<sim_time_type>::max() / limits.Period)
        throw std::overflow_error("Migration epoch overflow");
    lazy = LazyState();
    lazy.Active = true;
    lazy.Task = tasks.front();
    lazy.Page_sectors = directory.Block_unit_lba();
    if (!lazy.Page_sectors || directory.Zone_size_lba() % lazy.Page_sectors)
        throw std::invalid_argument("ON_DEMAND requires page-aligned zones");
    lazy.Deadline = (now / limits.Period + 1) * limits.Period;
    lazy.Moved.assign(directory.Zone_size_lba() / lazy.Page_sectors, false);
    lazy.Cold = lazy.Task.Op.Reserve_zone != INVALID_ZONE_ID;
    lazy.Target_zone = lazy.Cold ? lazy.Task.Op.Reserve_zone : lazy.Task.Op.Cold_zone;
    if (lazy.Cold && (!directory.Is_reserve_zone(lazy.Target_zone) || !directory.Is_empty(lazy.Target_zone)))
        throw std::invalid_argument("ON_DEMAND exchange requires an empty source OP zone");
    directory.Resolve_zone_lba(lazy.Target_zone, 0, lazy.Target_disk, lazy.Target_lba);
    directory.Mark_migrating(lazy.Task.Op.Hot_zone, true);
    directory.Mark_migrating(lazy.Task.Op.Cold_zone, true);
    if (lazy.Cold) directory.Mark_migrating(lazy.Task.Op.Reserve_zone, true);
    zone_bytes = directory.Zone_size_lba() * SECTOR_SIZE_IN_BYTE;
    block_bytes = lazy.Page_sectors * SECTOR_SIZE_IN_BYTE;
}

bool MigrationExecutor::Replay_ready(const SSD_Components::User_Request* request) const
{
    if (!limits.On_demand || !request) return false;
    if (!lazy.Active) return true;
    if (lazy.Host_id == request->ID) return true;
    return request->Type == SSD_Components::UserRequestType::READ && !lazy.Cold && !lazy.Force
        && lazy.Phase == LazyPhase::IDLE && lazy.Host_id.empty() && lazy.Preparing_id.empty();
}

void MigrationExecutor::Resolve_on_demand(ZoneResolveResult& resolved) const
{
    if (!limits.On_demand || !lazy.Active || lazy.Cold || resolved.Zone_id != lazy.Task.Op.Hot_zone) return;
    const auto page = resolved.Zone_lba_offset / lazy.Page_sectors;
    if (page < lazy.Moved.size() && lazy.Moved[page]) {
        resolved.Disk_id = lazy.Target_disk;
        resolved.Local_lba = lazy.Target_lba + resolved.Zone_lba_offset;
    }
}

void MigrationExecutor::Notify_host_completed(const io_request_id_type& id)
{
    if (limits.On_demand && lazy.Active && !id.empty() && lazy.Host_id == id) {
        lazy.Host_id.clear();
        lazy.Host_discard_ready = true;
    }
}

void MigrationExecutor::Force_finish()
{
    if (limits.On_demand && lazy.Active) lazy.Force = true;
}

uint64_t MigrationExecutor::Lazy_remaining_pages() const
{
    if (!lazy.Active || lazy.Cold) return 0;
    return std::count(lazy.Moved.begin(), lazy.Moved.end(), false) + lazy.Pending_host_discards.size();
}

MigrationExecutor::InterceptResult MigrationExecutor::Lazy_intercept(SSD_Components::User_Request* request,
    const std::vector<uint64_t>& zones, ZoneDirectory&, sim_time_type now)
{
    if (!request || !lazy.Active || !Intersects_inflight_zones(zones)) return InterceptResult::SUBMITTED;
    const bool cold = Hits_zone(lazy.Task.Op.Cold_zone, zones);
    const bool hot = Hits_zone(lazy.Task.Op.Hot_zone, zones);
    if (cold && request->Type == SSD_Components::UserRequestType::WRITE) {
        if (!lazy.Force) ++lazy_cold_write_drains;
        lazy.Force = true;
    }
    if (request->Type == SSD_Components::UserRequestType::READ && lazy.Started && !lazy.Cold
        && (!cold || lazy.Task.Op.Reserve_zone != INVALID_ZONE_ID)
        && (!hot || (!lazy.Force && lazy.Phase == LazyPhase::IDLE
            && lazy.Host_id.empty() && lazy.Preparing_id.empty()))) return InterceptResult::SUBMITTED;
    if (lazy.Requests.size() >= buffer_limit_per_task) {
        ++backpressure_events;
        return InterceptResult::BACKPRESSURE;
    }
    DeferredRequest item;
    item.Request = request;
    item.Enqueue_time = now;
    lazy.Requests.push_back(item);
    max_queue_depth = std::max<uint64_t>(max_queue_depth, lazy.Requests.size());
    Update_peaks();
    return InterceptResult::BUFFERED;
}

std::vector<unsigned int> MigrationExecutor::Lazy_request_pages(const SSD_Components::User_Request* request,
    const ZoneDirectory& directory, bool full_only) const
{
    std::vector<unsigned int> pages;
    LHA_type lba = request->Start_LBA;
    unsigned int remaining = request->SizeInSectors;
    while (remaining) {
        ZoneResolveResult r;
        directory.Resolve(lba, r);
        const unsigned int page_offset = r.Zone_lba_offset % lazy.Page_sectors;
        const unsigned int count = std::min(remaining, lazy.Page_sectors - page_offset);
        if (r.Zone_id == lazy.Task.Op.Hot_zone && (!full_only || (!page_offset && count == lazy.Page_sectors)))
            pages.push_back(static_cast<unsigned int>(r.Zone_lba_offset / lazy.Page_sectors));
        remaining -= count;
        lba += count;
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    return pages;
}

bool MigrationExecutor::Lazy_touches_cold(const SSD_Components::User_Request* request, const ZoneDirectory& directory) const
{
    LHA_type lba = request->Start_LBA;
    unsigned int remaining = request->SizeInSectors;
    while (remaining) {
        ZoneResolveResult r; directory.Resolve(lba, r);
        if (r.Zone_id == lazy.Task.Op.Cold_zone) return true;
        const auto chunk = static_cast<unsigned int>(std::min<uint64_t>(remaining, directory.Zone_size_lba() - r.Zone_lba_offset));
        lba += chunk; remaining -= chunk;
    }
    return false;
}

StripeCopyPlan MigrationExecutor::Lazy_page_plan(const ZoneDirectory& directory, unsigned int page) const
{
    StripeCopyPlan plan;
    plan.Stream_id = 0; // validated: one host address domain
    plan.Stripe_offset = page;
    plan.Lba_count = lazy.Page_sectors;
    const uint64_t source = lazy.Cold ? lazy.Task.Op.Cold_zone : lazy.Task.Op.Hot_zone;
    const uint64_t offset = static_cast<uint64_t>(page) * lazy.Page_sectors;
    directory.Resolve_zone_lba(source, offset, plan.Source_disk_id, plan.Source_lba);
    directory.Resolve_zone_lba(lazy.Target_zone, offset, plan.Destination_disk_id, plan.Destination_lba);
    return plan;
}

bool MigrationExecutor::Lazy_prepare_page(ZoneDirectory& directory, unsigned int page)
{
    lazy.Page = page;
    lazy.Runs.clear();
    lazy.Run_index = 0;
    const auto plan = Lazy_page_plan(directory, page);
    const auto zone = lazy.Cold ? lazy.Task.Op.Cold_zone : lazy.Task.Op.Hot_zone;
    for (auto run : directory.Valid_sector_runs(0, zone, static_cast<uint64_t>(page) * lazy.Page_sectors, lazy.Page_sectors)) {
        auto copy = plan;
        copy.Source_lba += run.first;
        copy.Destination_lba += run.first;
        copy.Lba_count = run.second;
        lazy.Runs.push_back(copy);
    }
    if (lazy.Runs.empty()) { lazy.Moved[page] = true; return false; }
    lazy.Phase = LazyPhase::READ;
    return true;
}

bool MigrationExecutor::Lazy_copy_completed(const io_request_id_type& id)
{
    if (!lazy.Active || id.empty() || lazy.Copy_id != id) return false;
    lazy.Copy_id.clear();
    if (lazy.Phase == LazyPhase::READ) lazy.Phase = LazyPhase::WRITE;
    else if (lazy.Phase == LazyPhase::WRITE) {
        ++lazy.Run_index;
        lazy.Phase = lazy.Run_index < lazy.Runs.size() ? LazyPhase::READ : LazyPhase::DISCARD;
    } else throw std::logic_error("Unexpected lazy copy completion");
    Update_peaks();
    return true;
}

void MigrationExecutor::Lazy_poll(ZoneDirectory& directory, const SubmitCopyFunction& submit,
    const DiscardFunction& discard, sim_time_type now)
{
    if (!lazy.Active) return;
    lazy.Started = true;
    if (now >= lazy.Deadline && !lazy.Deadline_counted) {
        lazy.Deadline_counted = true;
        ++lazy_deadline_drains;
        lazy.Force = true;
    }
    if (!lazy.Copy_id.empty() || !lazy.Host_id.empty()) return;
    if (lazy.Host_discard_ready) {
        while (!lazy.Pending_host_discards.empty()) {
            auto plan = Lazy_page_plan(directory, lazy.Pending_host_discards.front());
            if (!discard || !discard(plan, 0)) return;
            ++discarded_source_blocks;
            discarded_source_sectors += plan.Lba_count;
            lazy.Pending_host_discards.pop_front();
        }
        lazy.Host_discard_ready = false;
    }
    if (lazy.Phase == LazyPhase::READ || lazy.Phase == LazyPhase::WRITE) {
        lazy.Copy_id = submit(lazy.Runs.at(lazy.Run_index), lazy.Phase == LazyPhase::WRITE, 0);
        return;
    }
    if (lazy.Phase == LazyPhase::DISCARD) {
        auto plan = Lazy_page_plan(directory, lazy.Page);
        if (!discard || !discard(plan, 0)) return;
        lazy.Moved[lazy.Page] = true;
        ++grabbed_blocks; ++restored_blocks; ++discarded_source_blocks;
        discarded_source_sectors += plan.Lba_count;
        if (!lazy.Cold) ++lazy_copied_pages;
        lazy.Phase = LazyPhase::IDLE;
    }

    if (lazy.Cold) {
        for (unsigned int page = 0; page < lazy.Moved.size(); ++page)
            if (!lazy.Moved[page] && Lazy_prepare_page(directory, page)) {
                lazy.Copy_id = submit(lazy.Runs.front(), false, 0);
                return;
            }
        // Cold bytes and source invalidations have completed. Only now may the
        // freed cold physical zone become the hot destination.
        directory.Swap_placement(lazy.Task.Op.Cold_zone, lazy.Task.Op.Reserve_zone);
        directory.Complete_migration(lazy.Task.Op.Cold_zone, lazy.Task.Op.Reserve_zone, {});
        directory.Mark_migrating(lazy.Task.Op.Cold_zone, true);
        directory.Mark_migrating(lazy.Task.Op.Reserve_zone, true);
        lazy.Cold = false;
        ++cold_stages_completed;
        lazy.Moved.assign(lazy.Moved.size(), false);
        directory.Resolve_zone_lba(lazy.Target_zone, 0, lazy.Target_disk, lazy.Target_lba);
    }

    // Complete an already-prepared host write before a forced drain: its
    // overwrite pages have no copied destination data until the real I/O ends.
    if (!lazy.Force || !lazy.Preparing_id.empty()) {
        if (lazy.Requests.empty()) return;
        const auto& next = lazy.Requests.front();
        if (next.Request->Type == SSD_Components::UserRequestType::READ) {
            // An ordinary empty logical destination shares the physical zone
            // receiving hot pages until commit. Its reads must await remapping.
            if (lazy.Task.Op.Reserve_zone == INVALID_ZONE_ID && Lazy_touches_cold(next.Request, directory)) return;
            replay_queue.push_back(next);
            lazy.Requests.pop_front();
            return;
        }
        lazy.Preparing_id = next.Request->ID;
        const auto pages = Lazy_request_pages(next.Request, directory);
        const auto full = Lazy_request_pages(next.Request, directory, true);
        for (auto page : pages) {
            if (lazy.Moved[page]) continue;
            const bool overwrite = std::find(full.begin(), full.end(), page) != full.end();
            if (overwrite) {
                lazy.Moved[page] = true;
                lazy.Pending_host_discards.push_back(page);
                ++lazy_offloaded_pages;
            } else if (Lazy_prepare_page(directory, page)) {
                lazy.Copy_id = submit(lazy.Runs.front(), false, 0);
                return;
            }
        }
        lazy.Host_id = next.Request->ID;
        lazy.Preparing_id.clear();
        replay_queue.push_back(next);
        lazy.Requests.pop_front();
        return;
    }

    for (unsigned int page = 0; page < lazy.Moved.size(); ++page)
        if (!lazy.Moved[page] && Lazy_prepare_page(directory, page)) {
            lazy.Copy_id = submit(lazy.Runs.front(), false, 0);
            return;
        }
    // All pages are authoritative at the destination; no in-flight user I/O
    // remains (the controller drains it before polling). Rotate zone roles.
    directory.Swap_placement(lazy.Task.Op.Hot_zone, lazy.Target_zone);
    directory.Complete_migration(lazy.Task.Op.Hot_zone, lazy.Target_zone, {});
    directory.Mark_migrating(lazy.Task.Op.Cold_zone, false);
    directory.Mark_migrating(lazy.Task.Op.Hot_zone, false);
    if (lazy.Task.Op.Reserve_zone != INVALID_ZONE_ID) directory.Mark_migrating(lazy.Task.Op.Reserve_zone, false);
    while (!lazy.Requests.empty()) {
        replay_queue.push_back(lazy.Requests.front());
        lazy.Requests.pop_front();
    }
    directory.On_migration_completed();
    const auto hot = lazy.Task.Op.Hot_zone;
    lazy.Active = false;
    if (Task_finished) Task_finished(hot, false);
}

} // namespace RAID_Policy
