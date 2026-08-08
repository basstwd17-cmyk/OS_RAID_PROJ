#include "Stats.h"


namespace SSD_Components
{
	unsigned long Stats::IssuedReadCMD = 0;
	unsigned long Stats::IssuedCopybackReadCMD = 0;
	unsigned long Stats::IssuedInterleaveReadCMD = 0;
	unsigned long Stats::IssuedMultiplaneReadCMD = 0;
	unsigned long Stats::IssuedMultiplaneCopybackReadCMD = 0;
	unsigned long Stats::IssuedProgramCMD = 0;
	unsigned long Stats::IssuedInterleaveProgramCMD = 0;
	unsigned long Stats::IssuedMultiplaneProgramCMD = 0;
	unsigned long Stats::IssuedMultiplaneCopybackProgramCMD = 0;
	unsigned long Stats::IssuedInterleaveMultiplaneProgramCMD = 0;
	unsigned long Stats::IssuedSuspendProgramCMD = 0;
	unsigned long Stats::IssuedCopybackProgramCMD = 0;
	unsigned long Stats::IssuedEraseCMD = 0;
	unsigned long Stats::IssuedInterleaveEraseCMD = 0;
	unsigned long Stats::IssuedMultiplaneEraseCMD = 0;
	unsigned long Stats::IssuedInterleaveMultiplaneEraseCMD = 0;
	unsigned long Stats::IssuedSuspendEraseCMD = 0;
	unsigned long Stats::Total_flash_reads_for_mapping = 0;
	unsigned long Stats::Total_flash_writes_for_mapping = 0;
	unsigned long Stats::Total_flash_reads_for_mapping_per_stream[MAX_SUPPORT_STREAMS] = { 0 };
	unsigned long Stats::Total_flash_writes_for_mapping_per_stream[MAX_SUPPORT_STREAMS] = { 0 };
	unsigned int  Stats::CMT_hits = 0, Stats::readTR_CMT_hits = 0, Stats::writeTR_CMT_hits = 0;
	unsigned int  Stats::CMT_miss = 0, Stats::readTR_CMT_miss = 0, Stats::writeTR_CMT_miss = 0;
	unsigned int  Stats::total_CMT_queries = 0, Stats::total_readTR_CMT_queries = 0, Stats::total_writeTR_CMT_queries = 0;

	unsigned int Stats::Total_gc_executions = 0, Stats::Total_gc_executions_per_stream[MAX_SUPPORT_STREAMS] = { 0 };
	unsigned int Stats::Total_page_movements_for_gc = 0, Stats::Total_gc_page_movements_per_stream[MAX_SUPPORT_STREAMS] = { 0 };

	unsigned int Stats::Total_wl_executions = 0, Stats::Total_wl_executions_per_stream[MAX_SUPPORT_STREAMS] = { 0 };
	unsigned int Stats::Total_page_movements_for_wl = 0, Stats::Total_wl_page_movements_per_stream[MAX_SUPPORT_STREAMS] = { 0 };

	unsigned int Stats::CMT_hits_per_stream[MAX_SUPPORT_STREAMS] = { 0 }, Stats::readTR_CMT_hits_per_stream[MAX_SUPPORT_STREAMS] = { 0 }, Stats::writeTR_CMT_hits_per_stream[MAX_SUPPORT_STREAMS] = { 0 };
	unsigned int Stats::CMT_miss_per_stream[MAX_SUPPORT_STREAMS] = { 0 }, Stats::readTR_CMT_miss_per_stream[MAX_SUPPORT_STREAMS] = { 0 }, Stats::writeTR_CMT_miss_per_stream[MAX_SUPPORT_STREAMS] = { 0 };
	unsigned int Stats::total_CMT_queries_per_stream[MAX_SUPPORT_STREAMS] = { 0 }, Stats::total_readTR_CMT_queries_per_stream[MAX_SUPPORT_STREAMS] = { 0 }, Stats::total_writeTR_CMT_queries_per_stream[MAX_SUPPORT_STREAMS] = { 0 };


	void Stats::Init_stats(unsigned int channel_no, unsigned int chip_no_per_channel, unsigned int die_no_per_chip, unsigned int plane_no_per_die, 
		unsigned int block_no_per_plane, unsigned int page_no_per_block, unsigned int max_allowed_block_erase_count)
	{
		IssuedReadCMD = 0; IssuedCopybackReadCMD = 0; IssuedInterleaveReadCMD = 0; IssuedMultiplaneReadCMD = 0; IssuedMultiplaneCopybackReadCMD = 0;
		IssuedProgramCMD = 0; IssuedInterleaveProgramCMD = 0; IssuedMultiplaneProgramCMD = 0; IssuedMultiplaneCopybackProgramCMD = 0; IssuedInterleaveMultiplaneProgramCMD = 0; IssuedSuspendProgramCMD = 0; IssuedCopybackProgramCMD = 0;
		IssuedEraseCMD = 0; IssuedInterleaveEraseCMD = 0; IssuedMultiplaneEraseCMD = 0; IssuedInterleaveMultiplaneEraseCMD = 0;
		IssuedSuspendEraseCMD = 0;
		Total_flash_reads_for_mapping = 0; Total_flash_writes_for_mapping = 0; 
		CMT_hits = 0; readTR_CMT_hits = 0; writeTR_CMT_hits = 0;
		CMT_miss = 0; readTR_CMT_miss = 0; writeTR_CMT_miss = 0;
		total_CMT_queries = 0; total_readTR_CMT_queries = 0; total_writeTR_CMT_queries = 0;

		Total_gc_executions = 0;  Total_page_movements_for_gc = 0;
		Total_wl_executions = 0;  Total_page_movements_for_wl = 0;

		for (stream_id_type stream_id = 0; stream_id < MAX_SUPPORT_STREAMS; stream_id++) {
			Total_flash_reads_for_mapping_per_stream[stream_id] = 0;
			Total_flash_writes_for_mapping_per_stream[stream_id] = 0;
			CMT_hits_per_stream[stream_id] = 0; readTR_CMT_hits_per_stream[stream_id] = 0; writeTR_CMT_hits_per_stream[stream_id] = 0;
			CMT_miss_per_stream[stream_id] = 0; readTR_CMT_miss_per_stream[stream_id] = 0;  writeTR_CMT_miss_per_stream[stream_id] = 0;
			total_CMT_queries_per_stream[stream_id] = 0; total_readTR_CMT_queries_per_stream[stream_id] = 0; total_writeTR_CMT_queries_per_stream[stream_id] = 0;
			Total_gc_executions_per_stream[stream_id] = 0;
			Total_gc_page_movements_per_stream[stream_id] = 0;
			Total_wl_executions_per_stream[stream_id] = 0;
			Total_wl_page_movements_per_stream[stream_id] = 0;
		}
	}

	void Stats::Clear_stats(unsigned int channel_no, unsigned int chip_no_per_channel, unsigned int die_no_per_chip, unsigned int plane_no_per_die,
		unsigned int block_no_per_plane, unsigned int page_no_per_block, unsigned int max_allowed_block_erase_count)
	{
	}
}
