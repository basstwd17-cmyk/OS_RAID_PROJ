#ifndef RAID_SWANS_POLICY_TYPES_H
#define RAID_SWANS_POLICY_TYPES_H

#include <limits>
#include <map>
#include <vector>
#include "../ssd/SSD_Defs.h"

namespace RAID_Policy {

static const uint64_t INVALID_ZONE_ID = std::numeric_limits<uint64_t>::max(); // 유효한 zone을 찾지 못했을 때 쓰는 sentinel 값

enum class PolicyState {
	NORMAL,
	REDIRECT,
	MIGRATION
};	// 현재 정책의 상태 정의

struct RedirectOp
{
	bool Valid = false;    // redirect 연산이 유효한지 여부
	unsigned int Hot_ssd = 0; // hot 영역이 있는 SSD의 ID
	unsigned int Cold_ssd = 0; // cold 영역이 있는 SSD의 ID
};

struct MigrationOp
{
	uint64_t Hot_zone = INVALID_ZONE_ID; // hot 영역의 zone ID
	uint64_t Cold_zone = INVALID_ZONE_ID; // cold 영역의 zone ID
	unsigned int Hot_ssd = 0; // hot 영역이 있는 SSD의 ID
	unsigned int Cold_ssd = 0; // cold 영역이 있는 SSD의 ID
};

struct PolicyDecision	//policy가 컨트롤러에 한 epoch마다 전달하는 정책 결정 정보
{
	PolicyState State = PolicyState::NORMAL;
	double Mu = 0.0; // epoch write imbalance 표준편차
	RedirectOp Redirect;
	std::vector<MigrationOp> Migrations; // 수행할 마이그레이션 연산 목록
};

struct StripeCopyPlan
{
	stream_id_type Stream_id = 0;
	unsigned int Source_disk_id = 0;
	LHA_type Source_lba = 0;
	unsigned int Destination_disk_id = 0;
	LHA_type Destination_lba = 0;
	unsigned int Lba_count = 0;
	unsigned int Stripe_offset = 0; // reused as zone-local block offset in BlockWriteMap-based migration
};

struct MigrationTask
{
	MigrationOp Op;
	std::vector<StripeCopyPlan> Copies;
	uint64_t Moved_write_count = 0;
};

struct MigrationBuffer
{
	uint64_t Logical_zone = INVALID_ZONE_ID;
	unsigned int Source_ssd = 0;
	uint64_t Source_physical_zone = INVALID_ZONE_ID;
	unsigned int Target_ssd = 0;
	uint64_t Target_physical_zone = INVALID_ZONE_ID;
	std::map<stream_id_type, std::vector<bool>> Grabbed_blocks;
	std::map<stream_id_type, std::vector<bool>> Dirty_blocks;
	bool Valid = false;
};

} // namespace RAID_Policy

#endif // RAID_SWANS_POLICY_TYPES_H
